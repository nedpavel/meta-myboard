// SPDX-License-Identifier: GPL-2.0
///////////////////////////////////////////////////////////////////////////////
//
// Meng Engineering, Badstrasse 18b 5408 Ennetbaden, Markus Meng
// Kernel-Modul fuer eine MVBIP-Implementation des Kunden PIXY AG (Turgi).
//
// Original: Linux 2.6.x Device Driver (2009).
// 2026: an Linux 6.x (6.18.15) angepasst -mk-  Aenderungen sind mit "MOD:"
// markiert; die Hardware-Semantik (ISA/LPC I/O-Ports + 64K-TM @0xD0000)
// bleibt unveraendert.
//
// Anbindung ist LPC/ISA:
//   - FPGA-Steuerregister ueber I/O-Ports BASR0=0x320 / BASR1=0x322 / BCR=0x324
//   - 64 KB Traffic-Memory memory-mapped bei physisch 0xD0000
// Zugriffsmodell: Userspace oeffnet /dev/pixymvbip und mmap()t das 64K-TM;
// die TM-Struktur liefert mvbc.h. Kein read/write/ioctl.
//
///////////////////////////////////////////////////////////////////////////////
#include <linux/module.h>
#include <linux/init.h>
#include <linux/fs.h>
#include <linux/io.h>          // MOD: war <asm/io.h> — inw/outw/ioremap/iounmap
#include <linux/ioport.h>      // request_region / request_mem_region
#include <linux/mm.h>          // io_remap_pfn_range, pgprot_noncached
#include <linux/device.h>      // MOD: class_create/device_create -> /dev-Node automatisch
#include <linux/version.h>

/* Device Driver Information and Identification                               */
#define MVBIPDRV_MAJOR 241
#define MVBIPDRV_NAME "pixymvbip"

/* ISA-Bus IO-Space FPGA Registers of the MVBIP Design                       */
#define BASR0 0x0320
#define BASR1 0x0322
#define BCR   0x0324
#define BCR_INIT_VALUE 0x1600

/* ISA-Bus Mem-Space for the MVBIP Traffic Memory Access                     */
#define MVBIP_BASE 0xD0000
#define MVBIP_SIZE_64K 0x10000

static void __iomem *tm;          // MOD: __iomem — IO-Memory-Ptr zum 64K-TM
static struct class *mvbip_class;  // MOD: fuer automatische /dev-Node-Erzeugung

///////////////////////////////////////////////////////////////////////////////
// Device Specific HW Init Routine. Call this on Device Open Function.
static int cf_pc104_init(void)
{
    if (tm)                       // MOD: einfacher "bereits offen"-Schutz
        return -EBUSY;

    /* Get Access to the ISA Bus IO Ports of the MVBIP "Card" */
    if (!request_region(BASR0, 6, MVBIPDRV_NAME)) {
        pr_err("pixymvbip: IO-Port allocation (0x%x) failed\n", BASR0);
        return -EBUSY;
    }

    /* Reset the 'Card'. Disable Addr. Logic and MVBC, no IRQs. */
    outw((unsigned short)BCR_INIT_VALUE, BCR);
    /* Open a 64KB data segment through the ISA Bus registers in the MVBIP */
    outw((unsigned short)0x0D00, BASR0);
    outw((unsigned short)0xFF00, BASR1);
    /* Enable address translation, negate MVBIP reset state */
    outw((unsigned short)0x2600, BCR);

    /* Verify FPGA register contents (read-back) */
    if (inw(BASR0) != 0x0D00 || inw(BASR1) != 0xFF00 || inw(BCR) != 0x2600) {
        pr_err("pixymvbip: FPGA reg readback mismatch (BASR0=%04X BASR1=%04X BCR=%04X)\n",
               inw(BASR0), inw(BASR1), inw(BCR));
        release_region(BASR0, 6);
        return -EIO;
    }

    /* Reserve the 64 KB IO-memory window.
       MOD: auf x86-64 ist die Legacy-ISA-Region <1MB im e820 oft schon als
       "reserved" verbucht; request_mem_region kann dann fehlschlagen, obwohl
       das Fenster physisch der FPGA gehoert. Daher NICHT-fatal: nur warnen und
       trotzdem ioremap-en (ioremap benoetigt request_mem_region nicht). */
    if (!request_mem_region(MVBIP_BASE, MVBIP_SIZE_64K, MVBIPDRV_NAME))
        pr_warn("pixymvbip: mem region 0x%x busy — continuing with ioremap\n",
                MVBIP_BASE);

    tm = ioremap(MVBIP_BASE, MVBIP_SIZE_64K);  // MOD: ioremap_nocache() entfiel in 5.6
    if (!tm) {
        pr_err("pixymvbip: ioremap of TM @0x%x failed\n", MVBIP_BASE);
        release_mem_region(MVBIP_BASE, MVBIP_SIZE_64K);
        release_region(BASR0, 6);
        return -ENOMEM;
    }

    pr_info("pixymvbip: TM mapped @%p (phys 0x%x, 64K)\n", tm, MVBIP_BASE);
    return 0;
}

///////////////////////////////////////////////////////////////////////////////
// open — /dev/pixymvbip opened in userspace
static int pixymvbip_open(struct inode *inode, struct file *file)
{
    int result = cf_pc104_init();
    if (result == 0)
        pr_info("pixymvbip: opened\n");
    return result;
}

///////////////////////////////////////////////////////////////////////////////
// release — /dev/pixymvbip closed in userspace
static int pixymvbip_release(struct inode *inode, struct file *file)
{
    if (tm) {
        iounmap(tm);
        tm = NULL;
        release_mem_region(MVBIP_BASE, MVBIP_SIZE_64K);
    }
    /* Reset the card, then free the I/O ports */
    outw((unsigned short)BCR_INIT_VALUE, BCR);
    release_region(BASR0, 6);
    pr_info("pixymvbip: released\n");
    return 0;
}

///////////////////////////////////////////////////////////////////////////////
// mmap — map the 64K MVBIP Traffic Memory into user space
static int pixymvbip_mmap(struct file *filp, struct vm_area_struct *vma)
{
    unsigned long length = vma->vm_end - vma->vm_start;

    if (length > MVBIP_SIZE_64K)
        return -EIO;

    /* MOD: MMIO -> uncached in den Userspace mappen */
    vma->vm_page_prot = pgprot_noncached(vma->vm_page_prot);

    /* MOD: physische Basis 0xD0000 direkt verwenden. Das Original nahm
       virt_to_phys(tm) — fuer eine ioremap()-Adresse falsch. */
    if (io_remap_pfn_range(vma, vma->vm_start,
                           MVBIP_BASE >> PAGE_SHIFT,
                           length, vma->vm_page_prot))
        return -EAGAIN;

    return 0;
}

///////////////////////////////////////////////////////////////////////////////
// MOD: file_operations abgespeckt — .readdir/.ioctl gibt es in 6.x nicht mehr.
static const struct file_operations pixymvbip_fops = {
    .owner   = THIS_MODULE,
    .open    = pixymvbip_open,
    .release = pixymvbip_release,
    .mmap    = pixymvbip_mmap,
};

///////////////////////////////////////////////////////////////////////////////
static int __init pixymvbip_init_module(void)
{
    int ret;

    ret = register_chrdev(MVBIPDRV_MAJOR, MVBIPDRV_NAME, &pixymvbip_fops);
    if (ret < 0) {
        pr_err("pixymvbip: register_chrdev(major %d) failed: %d\n",
               MVBIPDRV_MAJOR, ret);
        return ret;
    }

    /* MOD: /dev/pixymvbip automatisch anlegen (frueher manuelles mknod).
       class_create() nimmt seit 6.4 nur noch den Namen. */
    mvbip_class = class_create(MVBIPDRV_NAME);
    if (IS_ERR(mvbip_class)) {
        unregister_chrdev(MVBIPDRV_MAJOR, MVBIPDRV_NAME);
        return PTR_ERR(mvbip_class);
    }
    device_create(mvbip_class, NULL, MKDEV(MVBIPDRV_MAJOR, 0), NULL, MVBIPDRV_NAME);

    pr_info("pixymvbip: initialized, /dev/%s (major %d)\n",
            MVBIPDRV_NAME, MVBIPDRV_MAJOR);
    return 0;
}

///////////////////////////////////////////////////////////////////////////////
static void __exit pixymvbip_cleanup_module(void)
{
    device_destroy(mvbip_class, MKDEV(MVBIPDRV_MAJOR, 0));
    class_destroy(mvbip_class);
    unregister_chrdev(MVBIPDRV_MAJOR, MVBIPDRV_NAME);
    pr_info("pixymvbip: removed\n");
}

module_init(pixymvbip_init_module);
module_exit(pixymvbip_cleanup_module);

MODULE_AUTHOR("Markus.meng@meng-engineering.ch");
MODULE_LICENSE("GPL v2");        // MOD: war "GPLv2" (nicht kernel-erkannt -> taint)
MODULE_DESCRIPTION("Device Driver for the OnBoard MVBIP (LPC/ISA) Interface");
