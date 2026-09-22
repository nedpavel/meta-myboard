// SPDX-License-Identifier: GPL-2.0
/*
 * pixy-mvb - Board-Treiber fuer die Pixy-1000 MVB/PC104 Extension Board (PCIe)
 *
 * Die Karte meldet sich als PCI 1204:EC30 (Lattice) mit einem einzigen
 * 64-MiB-BAR0. Das BAR ist in vier gleich grosse Bloecke geteilt:
 *
 *   +0*len/4  CoreID   Kennung und Firmwarestand
 *   +1*len/4  GPIO     Bestueckungs- und Betriebsartkennungen
 *   +2*len/4  ISA      der vom FPGA nachgebildete ISA-I/O-Raum des MVBC
 *   +3*len/4  SPARE    unbestueckt
 *
 * Der Treiber selbst kennt kein MVB. Er stellt /dev/mvbN bereit, bildet das
 * ganze BAR per mmap() ab, verteilt die MSI-Interrupts an angemeldete
 * Kernel-Dienste und exportiert Kartendaten ueber sysfs. Die eigentliche
 * Ansteuerung des MVB-Controllers macht der Oberbautreiber pixy-mvblli,
 * der sich ausschliesslich ueber die K*-ioctls hier andockt.
 */

#include <linux/cdev.h>
#include <linux/device.h>
#include <linux/fs.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/kernel.h>
#include <linux/mm.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/pci.h>
#include <linux/spinlock.h>
#include <linux/time.h>
#include <linux/uaccess.h>
#include <linux/version.h>

#include "pixy-mvb.h"

#define DRV_NAME		"pixy-mvb"
#define DRV_VERSION_STR \
	"pixy-mvb v3.0.0 - Pixy-1000 MVB/PC104 Extension Board Driver"

/*
 * Diagnosezaehler, lesbar unter /sys/module/pixy_mvb/parameters/.
 * Sie beantworten, ob ueberhaupt ein MSI ankommt und ob er einem
 * eingetragenen Dienst zugeordnet wird.
 */
static int dbg_hardirq;		/* Aufrufe des Interrupthandlers   */
static int dbg_dispatch;	/* Aufrufe eingetragener Dienste   */
static int dbg_nomatch;		/* Interrupt ohne passenden Vektor */
static int dbg_registered;	/* Dienste am zuletzt getroffenen Vektor */

module_param(dbg_hardirq, int, 0444);
module_param(dbg_dispatch, int, 0444);
module_param(dbg_nomatch, int, 0444);
module_param(dbg_registered, int, 0444);

#define PIXY_MVB_VENDOR_ID	0x1204
#define PIXY_MVB_DEVICE_ID	0xEC30

#define PIXY_MVB_MAX_BOARDS	3
#define PIXY_MVB_MAX_IRQ_VECT	8
#define PIXY_MVB_MAX_IRQ_SRV	5
#define PIXY_MVB_MAX_UPPER_DRV	5
#define PIXY_MVB_MINOR_COUNT	0x2fd

/* Registeroffsets im GPIO-Block */
#define GPIO_DAT		0x00
#define GPIO_ODR		0x04
#define GPIO_DIR		0x08
#define GPIO_RES		0x0c
#define GPIO_IMR		0x10
#define GPIO_ICR1		0x14
#define GPIO_ICR2		0x18
#define GPIO_IER		0x1c

/* Registeroffsets im CoreID-Block */
#define COREID_MAGIC		0x00
#define COREID_MODEL		0x04
#define COREID_HWINDEX		0x06
#define COREID_EPOCH		0x08
#define COREID_CODEID		0x0c

/*
 * Beim Aufsetzen werden die JTAG-Leitungen TDI/TCK/TMS auf Eingang
 * gestellt und die Konfigurations-LED eingeschaltet. Maske und Wert sind
 * vom Original uebernommen.
 */
#define GPIO_DIR_INIT_MASK	0xfffffcbfu

struct pixy_mvb_irq_vect {
	int irq;
	int act_nr_funcs;
	void (*func[PIXY_MVB_MAX_IRQ_SRV])(void *arg);
	void *arg[PIXY_MVB_MAX_IRQ_SRV];
};

struct pixy_mvb_board {
	struct pci_dev *pdev;
	int instance;
	int enable;

	u32 vendor;
	u32 device;
	u32 subsystem_vendor;
	u32 subsystem_device;
	u32 class;
	unsigned int busnum;
	unsigned int devfn;

	dev_t devt;
	struct cdev *cdev;
	struct device *sysdev;

	phys_addr_t bar_start;
	resource_size_t bar_len;
	void __iomem *bar;
	void __iomem *coreid;
	void __iomem *gpio;
	void __iomem *isa;
	u32 coreid_off;
	u32 gpio_off;
	u32 isa_off;
	u32 spare_off;

	struct PixyMvbBoardCoreID coreid_info;
	struct PixyMvbBoardGPIO gpio_info;

	int irq_vnum;
	struct pixy_mvb_irq_vect irq_srv[PIXY_MVB_MAX_IRQ_VECT];
	spinlock_t irq_lock;
};

struct pixy_mvb_upper_drv {
	struct PixyMvbBoardUpperDrvSubscribe info;
	int enable;
};

static struct pixy_mvb_drvr {
	dev_t devt_base;
	int num_boards;
	int dev_num;
	struct class *sys_class;
	struct pixy_mvb_board boards[PIXY_MVB_MAX_BOARDS];
	struct pixy_mvb_upper_drv upper[PIXY_MVB_MAX_UPPER_DRV];
	struct mutex lock;
} mvbdrvr;

static struct PixyMvbModuleVerStr mvbdrvr_version;

/* ------------------------------------------------------------------ sysfs */

static ssize_t board_type_show(struct device *dev,
			       struct device_attribute *attr, char *buf)
{
	struct pixy_mvb_board *brd = dev_get_drvdata(dev);
	u32 reg = ioread32(brd->gpio + GPIO_DAT);

	return scnprintf(buf, PAGE_SIZE, "%s\n",
			 (reg & PIXY_MVB_BOARD_GPIO_MVB_PC104n) ? "MVB" : "PC104");
}

static ssize_t controller_class_show(struct device *dev,
				     struct device_attribute *attr, char *buf)
{
	struct pixy_mvb_board *brd = dev_get_drvdata(dev);
	u32 reg = ioread32(brd->gpio + GPIO_DAT);

	if (!(reg & PIXY_MVB_BOARD_GPIO_MVB_PC104n))
		return scnprintf(buf, PAGE_SIZE, "%s\n", "unspecified");

	return scnprintf(buf, PAGE_SIZE, "%s\n",
			 (reg & PIXY_MVB_BOARD_GPIO_CLASS_MODE) ? "2/3/4" : "1");
}

static ssize_t connector_class_show(struct device *dev,
				    struct device_attribute *attr, char *buf)
{
	struct pixy_mvb_board *brd = dev_get_drvdata(dev);
	u32 reg = ioread32(brd->gpio + GPIO_DAT);

	if (!(reg & PIXY_MVB_BOARD_GPIO_MVB_PC104n))
		return scnprintf(buf, PAGE_SIZE, "%s\n", "unspecified");

	return scnprintf(buf, PAGE_SIZE, "%s\n",
			 (reg & PIXY_MVB_BOARD_GPIO_EMD_ESDn) ? "EMD" : "ESD");
}

static ssize_t sensibility_show(struct device *dev,
				struct device_attribute *attr, char *buf)
{
	struct pixy_mvb_board *brd = dev_get_drvdata(dev);
	u32 reg = ioread32(brd->gpio + GPIO_DAT);
	u32 need = PIXY_MVB_BOARD_GPIO_MVB_PC104n | PIXY_MVB_BOARD_GPIO_EMD_ESDn;

	/* Empfindlichkeit ist nur bei einer EMD-bestueckten MVB-Karte definiert */
	if ((reg & need) != need)
		return scnprintf(buf, PAGE_SIZE, "%s\n", "unspecified");

	return scnprintf(buf, PAGE_SIZE, "%s\n",
			 (reg & PIXY_MVB_BOARD_GPIO_EMD_HYSn) ? "high" : "low");
}

static ssize_t fw_epoch_show(struct device *dev,
			     struct device_attribute *attr, char *buf)
{
	struct pixy_mvb_board *brd = dev_get_drvdata(dev);

	return scnprintf(buf, PAGE_SIZE, "%d\n",
			 ioread32(brd->coreid + COREID_EPOCH));
}

static ssize_t fw_version_show(struct device *dev,
			       struct device_attribute *attr, char *buf)
{
	struct pixy_mvb_board *brd = dev_get_drvdata(dev);

	return scnprintf(buf, PAGE_SIZE, "%d\n",
			 ioread32(brd->coreid + COREID_CODEID));
}

static ssize_t hw_revision_show(struct device *dev,
				struct device_attribute *attr, char *buf)
{
	struct pixy_mvb_board *brd = dev_get_drvdata(dev);

	return scnprintf(buf, PAGE_SIZE, "%d\n",
			 ioread16(brd->coreid + COREID_HWINDEX));
}

static ssize_t vendor_show(struct device *dev,
			   struct device_attribute *attr, char *buf)
{
	struct pixy_mvb_board *brd = dev_get_drvdata(dev);

	return scnprintf(buf, PAGE_SIZE, "0x%04x\n", brd->vendor);
}

static ssize_t device_show(struct device *dev,
			   struct device_attribute *attr, char *buf)
{
	struct pixy_mvb_board *brd = dev_get_drvdata(dev);

	return scnprintf(buf, PAGE_SIZE, "0x%04x\n", brd->device);
}

static ssize_t subsystem_vendor_show(struct device *dev,
				     struct device_attribute *attr, char *buf)
{
	struct pixy_mvb_board *brd = dev_get_drvdata(dev);

	return scnprintf(buf, PAGE_SIZE, "0x%04x\n", brd->subsystem_vendor);
}

static ssize_t subsystem_device_show(struct device *dev,
				     struct device_attribute *attr, char *buf)
{
	struct pixy_mvb_board *brd = dev_get_drvdata(dev);

	return scnprintf(buf, PAGE_SIZE, "0x%04x\n", brd->subsystem_device);
}

static ssize_t class_show(struct device *dev,
			  struct device_attribute *attr, char *buf)
{
	struct pixy_mvb_board *brd = dev_get_drvdata(dev);

	return scnprintf(buf, PAGE_SIZE, "0x%06x\n", brd->class);
}

static ssize_t pci_id_show(struct device *dev,
			   struct device_attribute *attr, char *buf)
{
	struct pixy_mvb_board *brd = dev_get_drvdata(dev);

	return scnprintf(buf, PAGE_SIZE, "%04X:%02X:%02X.%X\n",
			 pci_domain_nr(brd->pdev->bus), brd->busnum,
			 PCI_SLOT(brd->devfn), PCI_FUNC(brd->devfn));
}

static DEVICE_ATTR_RO(board_type);
static DEVICE_ATTR_RO(controller_class);
static DEVICE_ATTR_RO(connector_class);
static DEVICE_ATTR_RO(sensibility);
static DEVICE_ATTR_RO(fw_epoch);
static DEVICE_ATTR_RO(fw_version);
static DEVICE_ATTR_RO(hw_revision);
static DEVICE_ATTR_RO(vendor);
static DEVICE_ATTR_RO(device);
static DEVICE_ATTR_RO(subsystem_vendor);
static DEVICE_ATTR_RO(subsystem_device);
static DEVICE_ATTR_RO(class);
static DEVICE_ATTR_RO(pci_id);

static struct attribute *pixy_mvb_attrs[] = {
	&dev_attr_board_type.attr,
	&dev_attr_controller_class.attr,
	&dev_attr_connector_class.attr,
	&dev_attr_sensibility.attr,
	&dev_attr_fw_epoch.attr,
	&dev_attr_fw_version.attr,
	&dev_attr_hw_revision.attr,
	&dev_attr_vendor.attr,
	&dev_attr_device.attr,
	&dev_attr_subsystem_vendor.attr,
	&dev_attr_subsystem_device.attr,
	&dev_attr_class.attr,
	&dev_attr_pci_id.attr,
	NULL,
};
ATTRIBUTE_GROUPS(pixy_mvb);

/* ----------------------------------------------------------------- IRQ */

/*
 * Ein MSI-Vektor kann von mehreren Kernel-Diensten gemeinsam benutzt
 * werden. Der Handler laeuft als Threaded IRQ und ruft alle fuer diesen
 * Vektor eingetragenen Funktionen der Reihe nach auf.
 */
static irqreturn_t pixy_mvb_irq_handler(int irq, void *dev_id)
{
	struct pixy_mvb_board *brd = dev_id;
	unsigned long flags;
	int i, j, matched = 0;

	dbg_hardirq++;

	spin_lock_irqsave(&brd->irq_lock, flags);
	for (i = 0; i < brd->irq_vnum; i++) {
		struct pixy_mvb_irq_vect *v = &brd->irq_srv[i];

		if (v->irq != irq)
			continue;
		matched = 1;
		dbg_registered = v->act_nr_funcs;
		for (j = 0; j < v->act_nr_funcs; j++)
			if (v->func[j]) {
				dbg_dispatch++;
				v->func[j](v->arg[j]);
			}
		break;
	}
	spin_unlock_irqrestore(&brd->irq_lock, flags);

	if (!matched)
		dbg_nomatch++;

	return IRQ_HANDLED;
}

static int pixy_mvb_irq_server_add(struct pixy_mvb_board *brd,
				   struct PixyMvbBoardIrqServer *srv)
{
	struct pixy_mvb_irq_vect *v = &brd->irq_srv[srv->irq_vect_id];
	unsigned long flags;
	int j, ret = 0;

	spin_lock_irqsave(&brd->irq_lock, flags);

	/* schon eingetragen? dann ist nichts zu tun */
	for (j = 0; j < v->act_nr_funcs; j++)
		if (v->func[j] == srv->func && v->arg[j] == srv->arg)
			goto out;

	if (v->act_nr_funcs >= PIXY_MVB_MAX_IRQ_SRV) {
		ret = -ENOMEM;
		goto out;
	}

	v->func[v->act_nr_funcs] = srv->func;
	v->arg[v->act_nr_funcs] = srv->arg;
	v->act_nr_funcs++;
out:
	spin_unlock_irqrestore(&brd->irq_lock, flags);
	return ret;
}

static int pixy_mvb_irq_server_del(struct pixy_mvb_board *brd,
				   struct PixyMvbBoardIrqServer *srv)
{
	struct pixy_mvb_irq_vect *v = &brd->irq_srv[srv->irq_vect_id];
	unsigned long flags;
	int i, removed = 0;

	if (!srv->func)
		return -EINVAL;

	spin_lock_irqsave(&brd->irq_lock, flags);
	for (i = 0; i < v->act_nr_funcs; ) {
		if (v->func[i] == srv->func && v->arg[i] == srv->arg) {
			int k;

			for (k = i; k < v->act_nr_funcs - 1; k++) {
				v->func[k] = v->func[k + 1];
				v->arg[k] = v->arg[k + 1];
			}
			v->func[v->act_nr_funcs - 1] = NULL;
			v->arg[v->act_nr_funcs - 1] = NULL;
			v->act_nr_funcs--;
			removed++;
			continue;
		}
		i++;
	}
	spin_unlock_irqrestore(&brd->irq_lock, flags);

	return removed ? 0 : -EPERM;
}

/* --------------------------------------------------------------- fops */

static int pixy_mvb_open(struct inode *inode, struct file *filp)
{
	struct pixy_mvb_board *brd = NULL;
	int i;

	for (i = 0; i < PIXY_MVB_MAX_BOARDS; i++) {
		if (mvbdrvr.boards[i].enable == 1 &&
		    MINOR(mvbdrvr.boards[i].devt) == iminor(inode)) {
			brd = &mvbdrvr.boards[i];
			break;
		}
	}
	if (!brd) {
		pr_warn(DRV_NAME ": no board for minor %u\n", iminor(inode));
		return -ENODEV;
	}

	filp->private_data = brd;

	if (pci_enable_device(brd->pdev))
		return -ENODEV;

	if (!brd->coreid) {
		dev_warn(&brd->pdev->dev, "Cannot access CoreID module\n");
		return -ENODEV;
	}
	if (!brd->gpio) {
		dev_warn(&brd->pdev->dev, "Cannot access GPIO module\n");
		return -ENODEV;
	}
	if (!brd->isa) {
		dev_warn(&brd->pdev->dev, "Cannot access ISA module\n");
		return -ENODEV;
	}

	return 0;
}

static int pixy_mvb_release(struct inode *inode, struct file *filp)
{
	struct pixy_mvb_board *brd = filp->private_data;

	if (brd && brd->pdev)
		pci_disable_device(brd->pdev);

	return 0;
}

/*
 * mmap bildet ab Offset 0 das gesamte BAR ab; vm_pgoff waehlt den
 * Startpunkt innerhalb des BAR. Die Seitenattribute werden bewusst NICHT
 * auf uncached gesetzt - das Original tut es ebenfalls nicht, und die
 * darauf aufsetzenden Bibliotheken verlassen sich auf dieses Verhalten.
 */
static int pixy_mvb_mmap(struct file *filp, struct vm_area_struct *vma)
{
	struct pixy_mvb_board *brd = filp->private_data;
	unsigned long size = vma->vm_end - vma->vm_start;

	if (!brd)
		return -ENOMEM;
	if (brd->enable != 1) {
		dev_err(&brd->pdev->dev, "Device not initialized\n");
		return -ENOMEM;
	}
	if (size > brd->bar_len) {
		dev_err(&brd->pdev->dev,
			"Memory mapping asked for too much memory\n");
		return -EINVAL;
	}

	vma->vm_flags |= VM_IO | VM_LOCKED | VM_DONTEXPAND | VM_DONTDUMP;

	if (remap_pfn_range(vma, vma->vm_start,
			    (brd->bar_start + (vma->vm_pgoff << PAGE_SHIFT))
				>> PAGE_SHIFT,
			    size, vma->vm_page_prot) < 0) {
		dev_err(&brd->pdev->dev, "Memory mapping failed\n");
		return -EAGAIN;
	}

	return 0;
}

static long pixy_mvb_ioctl(struct file *filp, unsigned int cmd,
			   unsigned long arg)
{
	struct pixy_mvb_board *brd = filp->private_data;
	void __user *uarg = (void __user *)arg;
	u32 val32;
	int ret = 0;

	if (!brd)
		return -ENODEV;
	if (brd->enable != 1) {
		dev_err(&brd->pdev->dev, "Device not initialized\n");
		return -ENODEV;
	}
	if (_IOC_TYPE(cmd) != PIXY_MVB_BOARD_IOCTL_MAGIC) {
		dev_err(&brd->pdev->dev, "Invalid ioctl command type\n");
		return -ENOTTY;
	}
	if (_IOC_NR(cmd) > PIXY_MVB_BOARD_MAX_IOCTL_NR) {
		dev_err(&brd->pdev->dev, "Invalid ioctl command number\n");
		return -ENOTTY;
	}

	switch (cmd) {
	/* -------- Aufrufe aus dem Userspace -------- */
	case IOCTL_PIXY_MVB_BOARD_GET_MODULE_INFO:
		if (copy_to_user(uarg, &mvbdrvr_version,
				 sizeof(mvbdrvr_version)))
			ret = -EFAULT;
		break;

	case IOCTL_PIXY_MVB_BOARD_GET_COREID: {
		struct PixyMvbBoardCoreID id;

		id.SelMagic = ioread32(brd->coreid + COREID_MAGIC);
		id.SelModel = ioread16(brd->coreid + COREID_MODEL);
		id.HwIndex  = ioread16(brd->coreid + COREID_HWINDEX);
		id.Epoch    = ioread32(brd->coreid + COREID_EPOCH);
		id.codeID   = ioread32(brd->coreid + COREID_CODEID);
		if (copy_to_user(uarg, &id, sizeof(id)))
			ret = -EFAULT;
		break;
	}

	case IOCTL_PIXY_MVB_BOARD_GET_GPIO: {
		struct PixyMvbBoardGPIO g;

		g.DATreg  = ioread32(brd->gpio + GPIO_DAT);
		g.ODRreg  = ioread32(brd->gpio + GPIO_ODR);
		g.DIRreg  = ioread32(brd->gpio + GPIO_DIR);
		g.RESreg  = ioread32(brd->gpio + GPIO_RES);
		g.IMRreg  = ioread32(brd->gpio + GPIO_IMR);
		g.ICR1reg = ioread32(brd->gpio + GPIO_ICR1);
		g.ICR2reg = ioread32(brd->gpio + GPIO_ICR2);
		g.IERreg  = ioread32(brd->gpio + GPIO_IER);
		if (copy_to_user(uarg, &g, sizeof(g)))
			ret = -EFAULT;
		break;
	}

	case IOCTL_PIXY_MVB_BOARD_SET_GPIO_DAT:
		if (get_user(val32, (u32 __user *)uarg))
			ret = -EFAULT;
		else
			iowrite32(ioread32(brd->gpio + GPIO_DAT) | val32,
				  brd->gpio + GPIO_DAT);
		break;

	case IOCTL_PIXY_MVB_BOARD_CLR_GPIO_DAT:
		if (get_user(val32, (u32 __user *)uarg))
			ret = -EFAULT;
		else
			iowrite32(ioread32(brd->gpio + GPIO_DAT) & ~val32,
				  brd->gpio + GPIO_DAT);
		break;

	case IOCTL_PIXY_MVB_BOARD_GET_GPIO_DAT:
		val32 = ioread32(brd->gpio + GPIO_DAT);
		if (put_user(val32, (u32 __user *)uarg))
			ret = -EFAULT;
		break;

	case IOCTL_PIXY_MVB_BOARD_SET_GPIO_DIR:
		if (get_user(val32, (u32 __user *)uarg))
			ret = -EFAULT;
		else
			iowrite32(val32, brd->gpio + GPIO_DIR);
		break;

	case IOCTL_PIXY_MVB_BOARD_GET_BAR0_SIZE: {
		size_t len = brd->bar_len;

		if (copy_to_user(uarg, &len, sizeof(len)))
			ret = -EFAULT;
		break;
	}

	case IOCTL_PIXY_MVB_BOARD_GET_MAGIC_NUMBER:
		val32 = ioread32(brd->coreid + COREID_MAGIC);
		if (put_user(val32, (u32 __user *)uarg))
			ret = -EFAULT;
		break;

	case IOCTL_PIXY_MVB_BOARD_GET_FW_VERSION:
		val32 = ioread32(brd->coreid + COREID_CODEID);
		if (put_user(val32, (u32 __user *)uarg))
			ret = -EFAULT;
		break;

	case IOCTL_PIXY_MVB_BOARD_GET_COREID_OFFSET:
		if (put_user(brd->coreid_off, (u32 __user *)uarg))
			ret = -EFAULT;
		break;

	case IOCTL_PIXY_MVB_BOARD_GET_GPIO_OFFSET:
		if (put_user(brd->gpio_off, (u32 __user *)uarg))
			ret = -EFAULT;
		break;

	case IOCTL_PIXY_MVB_BOARD_GET_ISA_OFFSET:
		if (put_user(brd->isa_off, (u32 __user *)uarg))
			ret = -EFAULT;
		break;

	case IOCTL_PIXY_MVB_BOARD_GET_SPAREID_OFFSET:
		if (put_user(brd->spare_off, (u32 __user *)uarg))
			ret = -EFAULT;
		break;

	case IOCTL_PIXY_MVB_BOARD_SET_SW_RESET:
		iowrite32(ioread32(brd->gpio + GPIO_DAT) |
				PIXY_MVB_BOARD_GPIO_SW_RESET,
			  brd->gpio + GPIO_DAT);
		iowrite32(ioread32(brd->gpio + GPIO_DAT) &
				~PIXY_MVB_BOARD_GPIO_SW_RESET,
			  brd->gpio + GPIO_DAT);
		break;

	case IOCTL_PIXY_MVB_BOARD_SET_FW_RELOAD:
		iowrite32(ioread32(brd->gpio + GPIO_DAT) &
				~PIXY_MVB_BOARD_GPIO_PROGRAMN,
			  brd->gpio + GPIO_DAT);
		iowrite32(ioread32(brd->gpio + GPIO_DAT) |
				PIXY_MVB_BOARD_GPIO_PROGRAMN,
			  brd->gpio + GPIO_DAT);
		break;

	/* -------- Aufrufe aus dem Kernel: arg ist ein Kernelzeiger -------- */
	case IOCTL_PIXY_MVB_BOARD_KGET_IRQ_VNUM:
		*(u32 *)arg = brd->irq_vnum;
		break;

	case IOCTL_PIXY_MVB_BOARD_KGET_PISA:
		*(void **)arg = brd->isa;
		break;

	case IOCTL_PIXY_MVB_BOARD_KGET_PGPIO:
		*(void **)arg = brd->gpio;
		break;

	case IOCTL_PIXY_MVB_BOARD_KSET_IRQ_SERVER: {
		struct PixyMvbBoardIrqServer *srv =
			(struct PixyMvbBoardIrqServer *)arg;

		if (srv->irq_vect_id >= brd->irq_vnum ||
		    srv->irq_vect_id >= PIXY_MVB_MAX_IRQ_VECT ||
		    srv->irq_vect_id < 0) {
			dev_err(&brd->pdev->dev,
				"ioctl IOCTL_PIXY_MVB_BOARD_KSET_IRQ_SERVER failed\n");
			ret = -ENXIO;
			break;
		}
		if (srv->remove)
			ret = pixy_mvb_irq_server_del(brd, srv);
		else
			ret = pixy_mvb_irq_server_add(brd, srv);
		break;
	}

	case IOCTL_PIXY_MVB_BOARD_KSET_DRV_SUBSCRIBE: {
		struct PixyMvbBoardUpperDrvSubscribe *sub =
			(struct PixyMvbBoardUpperDrvSubscribe *)arg;
		int i;

		mutex_lock(&mvbdrvr.lock);
		for (i = 0; i < PIXY_MVB_MAX_UPPER_DRV; i++)
			if (!mvbdrvr.upper[i].enable)
				break;
		if (i == PIXY_MVB_MAX_UPPER_DRV) {
			mutex_unlock(&mvbdrvr.lock);
			pr_warn(DRV_NAME ": no free upper driver slot\n");
			ret = -EFAULT;
			break;
		}
		mvbdrvr.upper[i].info = *sub;
		mvbdrvr.upper[i].info.drv_id = i;
		sub->drv_id = i;
		mvbdrvr.upper[i].enable = 1;
		mutex_unlock(&mvbdrvr.lock);
		pr_info(DRV_NAME ": upper driver %d subscribed\n", i + 1);

		/*
		 * Bereits vorhandene Karten werden hier bewusst NICHT
		 * gemeldet - das Original tut es auch nicht. Der Oberbau-
		 * treiber sucht /dev/mvb0..2 beim Laden selbst ab und
		 * abonniert nur die spaeteren Ereignisse. Wer hier meldet,
		 * loest beim originalen LLI eine doppelte Anmeldung aus.
		 */
		break;
	}

	case IOCTL_PIXY_MVB_BOARD_KSET_DRV_UNSUBSCRIBE: {
		int id = *(int *)arg;

		if (id < 0 || id >= PIXY_MVB_MAX_UPPER_DRV ||
		    !mvbdrvr.upper[id].enable) {
			pr_warn(DRV_NAME ": cannot unsubscribe driver %d\n", id);
			ret = -EFAULT;
			break;
		}
		mutex_lock(&mvbdrvr.lock);
		memset(&mvbdrvr.upper[id], 0, sizeof(mvbdrvr.upper[id]));
		mutex_unlock(&mvbdrvr.lock);
		pr_info(DRV_NAME ": upper driver %d unsubscribed\n", id + 1);
		break;
	}

	default:
		ret = -EINVAL;
		break;
	}

	return ret;
}

static const struct file_operations pixy_mvb_fops = {
	.owner		= THIS_MODULE,
	.open		= pixy_mvb_open,
	.release	= pixy_mvb_release,
	.unlocked_ioctl	= pixy_mvb_ioctl,
	.compat_ioctl	= pixy_mvb_ioctl,
	.mmap		= pixy_mvb_mmap,
	.llseek		= no_llseek,
};

/* --------------------------------------------------------------- PCI */

static void pixy_mvb_notify_upper(int instance, bool added)
{
	int i;

	for (i = 0; i < PIXY_MVB_MAX_UPPER_DRV; i++) {
		struct pixy_mvb_upper_drv *u = &mvbdrvr.upper[i];
		struct PixyMvbBoardEventMgr *ev;

		if (!u->enable)
			continue;
		ev = added ? &u->info.add_brd : &u->info.rm_brd;
		if (ev->brd_evnt)
			ev->brd_evnt(instance, ev->arg);
	}
}

static struct pixy_mvb_board *pixy_mvb_claim_slot(struct pci_dev *pdev)
{
	int i;

	for (i = 0; i < mvbdrvr.num_boards; i++)
		if (mvbdrvr.boards[i].busnum == pdev->bus->number)
			return &mvbdrvr.boards[i];

	if (mvbdrvr.num_boards >= PIXY_MVB_MAX_BOARDS)
		return NULL;

	return &mvbdrvr.boards[mvbdrvr.num_boards++];
}

static int pixy_mvb_probe(struct pci_dev *pdev, const struct pci_device_id *ent)
{
	struct device *dev = &pdev->dev;
	struct pixy_mvb_board *brd;
	u16 vendor, device, subvendor, subdevice;
	u32 classrev;
	int err, i;
	char magic[5];

	err = pci_request_regions(pdev, DRV_NAME);
	if (err) {
		dev_err(dev, "Cannot get PCI device region\n");
		return err;
	}

	pci_set_master(pdev);

	err = pci_enable_device(pdev);
	if (err) {
		dev_err(dev, "Cannot enable pci device\n");
		goto err_regions;
	}

	brd = pixy_mvb_claim_slot(pdev);
	if (!brd) {
		dev_err(dev, "Exceeded maximum number of boards\n");
		err = -ENODEV;
		goto err_regions;
	}

	i = brd->instance = brd - mvbdrvr.boards;
	spin_lock_init(&brd->irq_lock);

	if (pci_read_config_word(pdev, PCI_VENDOR_ID, &vendor) ||
	    pci_read_config_word(pdev, PCI_DEVICE_ID, &device) ||
	    pci_read_config_word(pdev, PCI_SUBSYSTEM_VENDOR_ID, &subvendor) ||
	    pci_read_config_word(pdev, PCI_SUBSYSTEM_ID, &subdevice) ||
	    pci_read_config_dword(pdev, PCI_CLASS_REVISION, &classrev)) {
		dev_err(dev, "Cannot retrieve PCI configuration\n");
		err = -EIO;
		goto err_regions;
	}

	brd->pdev = pdev;
	brd->vendor = vendor;
	brd->device = device;
	brd->subsystem_vendor = subvendor;
	brd->subsystem_device = subdevice;
	brd->class = classrev >> 8;
	brd->busnum = pdev->bus->number;
	brd->devfn = pdev->devfn;
	brd->devt = MKDEV(MAJOR(mvbdrvr.devt_base),
			  MINOR(mvbdrvr.devt_base) + mvbdrvr.dev_num);

	if (vendor != PIXY_MVB_VENDOR_ID || device != PIXY_MVB_DEVICE_ID) {
		dev_err(dev, "Device is not MVB/PC104 Extension board\n");
		err = -ENODEV;
		goto err_regions;
	}

	brd->bar_start = pci_resource_start(pdev, 0);
	brd->bar_len = pci_resource_len(pdev, 0);
	if (!brd->bar_start || !brd->bar_len) {
		dev_err(dev, "Invalid BAR0 address\n");
		err = -ENODEV;
		goto err_regions;
	}

	brd->bar = ioremap(brd->bar_start, brd->bar_len);
	if (!brd->bar) {
		dev_err(dev, "Cannot map BAR0 memory\n");
		err = -ENOMEM;
		goto err_regions;
	}

	/* Vier gleich grosse Bloecke ueber das BAR verteilt */
	brd->coreid_off = 0;
	brd->gpio_off   = (u32)(brd->bar_len >> 2);
	brd->isa_off    = brd->gpio_off * 2;
	brd->spare_off  = brd->gpio_off * 3;
	brd->coreid = brd->bar + brd->coreid_off;
	brd->gpio   = brd->bar + brd->gpio_off;
	brd->isa    = brd->bar + brd->isa_off;

	brd->coreid_info.SelMagic = ioread32(brd->coreid + COREID_MAGIC);
	brd->coreid_info.SelModel = ioread16(brd->coreid + COREID_MODEL);
	brd->coreid_info.HwIndex  = ioread16(brd->coreid + COREID_HWINDEX);
	brd->coreid_info.Epoch    = ioread32(brd->coreid + COREID_EPOCH);
	brd->coreid_info.codeID   = ioread32(brd->coreid + COREID_CODEID);

	snprintf(magic, sizeof(magic), "%c%c%c%c",
		 (brd->coreid_info.SelMagic >> 24) & 0xff,
		 (brd->coreid_info.SelMagic >> 16) & 0xff,
		 (brd->coreid_info.SelMagic >> 8) & 0xff,
		 brd->coreid_info.SelMagic & 0xff);
	dev_info(dev, "CoreID magic '%s' (0x%08x), BAR0 %llu bytes\n",
		 magic, brd->coreid_info.SelMagic,
		 (unsigned long long)brd->bar_len);

	if (brd->coreid_info.SelMagic != PIXY_MVB_BOARD_MAGIC_NUM)
		dev_warn(dev, "Unexpected CoreID magic number\n");

	/*
	 * GPIO aufsetzen: JTAG-Pins auf Eingang, Konfigurations-LED an.
	 * Danach den gesamten Registersatz einlesen.
	 */
	{
		u32 dir = ioread32(brd->gpio + GPIO_DIR) & GPIO_DIR_INIT_MASK;
		u32 dat = ioread32(brd->gpio + GPIO_DAT) |
			  PIXY_MVB_BOARD_GPIO_CFG_LED;

		iowrite32(dir, brd->gpio + GPIO_DIR);
		iowrite32(dat, brd->gpio + GPIO_DAT);

		brd->gpio_info.DATreg  = ioread32(brd->gpio + GPIO_DAT);
		brd->gpio_info.ODRreg  = ioread32(brd->gpio + GPIO_ODR);
		brd->gpio_info.DIRreg  = ioread32(brd->gpio + GPIO_DIR);
		brd->gpio_info.RESreg  = ioread32(brd->gpio + GPIO_RES);
		brd->gpio_info.IMRreg  = ioread32(brd->gpio + GPIO_IMR);
		brd->gpio_info.ICR1reg = ioread32(brd->gpio + GPIO_ICR1);
		brd->gpio_info.ICR2reg = ioread32(brd->gpio + GPIO_ICR2);
		brd->gpio_info.IERreg  = ioread32(brd->gpio + GPIO_IER);
	}

	dev_info(dev,
		 "%s Extension Board detected in bus slot %X, Fw version code %d\n",
		 (brd->gpio_info.DATreg & PIXY_MVB_BOARD_GPIO_MVB_PC104n) ?
			"MVB" : "PC104",
		 brd->busnum, brd->coreid_info.codeID);

	mvbdrvr.dev_num++;

	brd->cdev = cdev_alloc();
	if (!brd->cdev) {
		dev_err(dev, "Cannot allocate char device\n");
		err = -ENOMEM;
		goto err_unmap;
	}
	brd->cdev->owner = THIS_MODULE;
	brd->cdev->ops = &pixy_mvb_fops;
	kobject_set_name(&brd->cdev->kobj, DRV_NAME);

	err = cdev_add(brd->cdev, brd->devt, 1);
	if (err) {
		dev_err(dev, "Cannot add char device\n");
		kobject_put(&brd->cdev->kobj);
		brd->cdev = NULL;
		goto err_unmap;
	}

	brd->sysdev = device_create(mvbdrvr.sys_class, NULL, brd->devt, brd,
				    "mvb%d", brd->instance);
	if (IS_ERR(brd->sysdev)) {
		err = PTR_ERR(brd->sysdev);
		brd->sysdev = NULL;
		dev_err(dev, "Cannot create entry in '/dev/' tree\n");
		goto err_cdev;
	}
	dev_info(dev, "Created device entry /dev/mvb%d\n", brd->instance);

	brd->irq_vnum = pci_alloc_irq_vectors_affinity(pdev, 1,
						       PIXY_MVB_MAX_IRQ_VECT,
						       PCI_IRQ_MSI, NULL);
	if (brd->irq_vnum < 0) {
		dev_err(dev, "Cannot get MSI irq vector\n");
		err = brd->irq_vnum;
		goto err_device;
	}
	dev_info(dev, "Configuring %s MSI interrupt...\n",
		 brd->irq_vnum < 2 ? "single" : "multi");

	for (i = 0; i < brd->irq_vnum; i++) {
		int irq = pci_irq_vector(pdev, i);

		brd->irq_srv[i].irq = irq;
		brd->irq_srv[i].act_nr_funcs = 0;

		err = request_threaded_irq(irq, pixy_mvb_irq_handler, NULL,
					   IRQF_ONESHOT, DRV_NAME, brd);
		if (err) {
			dev_err(dev,
				"Failed to request irq #%d for MSI vector %d\n",
				irq, i);
			goto err_irq;
		}
		dev_info(dev, "Set irq #%d for MSI vector %d\n", irq, i);
	}

	brd->enable = 1;
	pci_set_drvdata(pdev, brd);

	pixy_mvb_notify_upper(brd->instance, true);

	return 0;

err_irq:
	while (--i >= 0)
		free_irq(brd->irq_srv[i].irq, brd);
	pci_free_irq_vectors(pdev);
err_device:
	device_destroy(mvbdrvr.sys_class, brd->devt);
	brd->sysdev = NULL;
err_cdev:
	cdev_del(brd->cdev);
	brd->cdev = NULL;
err_unmap:
	iounmap(brd->bar);
	brd->bar = NULL;
err_regions:
	pci_release_regions(pdev);
	dev_err(dev, "Error initializing the MVB/PC104 Extension Board\n");
	return err ? err : -ENODEV;
}

static void pixy_mvb_remove(struct pci_dev *pdev)
{
	struct pixy_mvb_board *brd = pci_get_drvdata(pdev);
	int i;

	if (!brd)
		return;

	brd->enable = 0;
	pixy_mvb_notify_upper(brd->instance, false);

	for (i = 0; i < brd->irq_vnum; i++)
		free_irq(brd->irq_srv[i].irq, brd);
	pci_free_irq_vectors(pdev);

	if (brd->sysdev) {
		device_destroy(mvbdrvr.sys_class, brd->devt);
		brd->sysdev = NULL;
	}
	if (brd->cdev) {
		cdev_del(brd->cdev);
		brd->cdev = NULL;
	}
	if (brd->bar) {
		iounmap(brd->bar);
		brd->bar = NULL;
	}

	pci_release_regions(pdev);
	pci_disable_device(pdev);

	if (mvbdrvr.dev_num > 0)
		mvbdrvr.dev_num--;

	dev_info(&pdev->dev, "MVB/PC104 Extension Board removed\n");
}

static const struct pci_device_id pixy_mvb_ids[] = {
	{ PCI_DEVICE(PIXY_MVB_VENDOR_ID, PIXY_MVB_DEVICE_ID) },
	{ 0 }
};
MODULE_DEVICE_TABLE(pci, pixy_mvb_ids);

static struct pci_driver pixy_mvb_driver = {
	.name		= "pixy_mvb",
	.id_table	= pixy_mvb_ids,
	.probe		= pixy_mvb_probe,
	.remove		= pixy_mvb_remove,
};

/* -------------------------------------------------------------- module */

static int __init pixy_mvb_init(void)
{
	int err;

	memset(&mvbdrvr, 0, sizeof(mvbdrvr));
	mutex_init(&mvbdrvr.lock);

	memset(&mvbdrvr_version, 0, sizeof(mvbdrvr_version));
	strscpy(mvbdrvr_version.version, DRV_VERSION_STR,
		sizeof(mvbdrvr_version.version));
	pr_info("%s\n", mvbdrvr_version.version);

	err = alloc_chrdev_region(&mvbdrvr.devt_base, 0,
				  PIXY_MVB_MINOR_COUNT, DRV_NAME);
	if (err < 0) {
		pr_err(DRV_NAME ": cannot allocate char device region\n");
		return err;
	}

	mvbdrvr.sys_class = class_create(THIS_MODULE, DRV_NAME);
	if (IS_ERR(mvbdrvr.sys_class)) {
		pr_err(DRV_NAME ": cannot create device class\n");
		err = PTR_ERR(mvbdrvr.sys_class);
		goto err_chrdev;
	}
	mvbdrvr.sys_class->dev_groups = pixy_mvb_groups;

	err = pci_register_driver(&pixy_mvb_driver);
	if (err < 0) {
		pr_err(DRV_NAME ": cannot register pci driver\n");
		goto err_class;
	}

	return 0;

err_class:
	class_destroy(mvbdrvr.sys_class);
err_chrdev:
	unregister_chrdev_region(mvbdrvr.devt_base, PIXY_MVB_MINOR_COUNT);
	return err;
}

static void __exit pixy_mvb_exit(void)
{
	pci_unregister_driver(&pixy_mvb_driver);
	class_destroy(mvbdrvr.sys_class);
	unregister_chrdev_region(mvbdrvr.devt_base, PIXY_MVB_MINOR_COUNT);
	pr_info(DRV_NAME ": unloaded\n");
}

module_init(pixy_mvb_init);
module_exit(pixy_mvb_exit);

MODULE_LICENSE("GPL v2");
MODULE_DESCRIPTION("Pixy-1000 MVB/PC104 Extension Board driver");
MODULE_VERSION("3.0.0");
