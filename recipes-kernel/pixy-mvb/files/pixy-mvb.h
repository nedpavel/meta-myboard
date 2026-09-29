/* SPDX-License-Identifier: GPL-2.0 */
/*
 * pixy-mvb.h - ABI des Board-Treibers fuer die Pixy-1000 MVB/PC104-PCIe-Karte
 *
 * Diese Datei definiert die Schnittstelle zwischen dem Board-Treiber und
 * seinen Benutzern: Anwendungen im Userspace und der aufgesetzte
 * Link-Layer-Treiber pixy-mvblli im Kernel.
 *
 * Die Werte sind ABI und duerfen nicht veraendert werden - bestehende
 * Bibliotheken und der Original-Oberbautreiber sind dagegen uebersetzt.
 */

#ifndef PIXY_MVB_H
#define PIXY_MVB_H

#include <asm/ioctl.h>

#ifndef __KERNEL__
#include <stdint.h>
#else
#include <linux/types.h>
#endif

#define PIXY_MVB_MODULE_VERSION_LEN	128

struct PixyMvbModuleVerStr {
	char version[PIXY_MVB_MODULE_VERSION_LEN];
};

enum PixyMvbBoardModuleID {
	COREID,
	GPIOID,
	ISAID,
	SPAREID,
	NumberOfModules
};

/* CoreID-Block: Kennung und Firmwarestand der FPGA-Ladung */
#define PIXY_MVB_BOARD_MAGIC_NUM	0x50495859	/* 'P' 'I' 'X' 'Y' */

struct __attribute__((packed)) PixyMvbBoardCoreID {
	uint32_t SelMagic;
	uint16_t SelModel;
	uint16_t HwIndex;
	uint32_t Epoch;
	uint32_t codeID;
};

/* GPIO-Block: Bestueckungs- und Betriebsartkennungen der Karte */
#define PIXY_MVB_BOARD_GPIO_MVB_PC104n		0x00000001
#define PIXY_MVB_BOARD_GPIO_EMD_ESDn		0x00000002
#define PIXY_MVB_BOARD_GPIO_OFFSET_CONFIG	0x00000004
#define PIXY_MVB_BOARD_GPIO_IRQ_CONFIG		0x00000008
#define PIXY_MVB_BOARD_GPIO_CLASS_MODE		0x00000010
#define PIXY_MVB_BOARD_GPIO_EMD_HYSn		0x00000020
#define PIXY_MVB_BOARD_GPIO_TDI			0x00000040
#define PIXY_MVB_BOARD_GPIO_TDO			0x00000080
#define PIXY_MVB_BOARD_GPIO_TCK			0x00000100
#define PIXY_MVB_BOARD_GPIO_TMS			0x00000200
#define PIXY_MVB_BOARD_GPIO_SW_RESET		0x00000400
#define PIXY_MVB_BOARD_GPIO_CFG_LED		0x00000800
#define PIXY_MVB_BOARD_GPIO_PROGRAMN		0x00001000

struct __attribute__((packed)) PixyMvbBoardGPIO {
	uint32_t DATreg;
	uint32_t ODRreg;
	uint32_t DIRreg;
	uint32_t RESreg;
	uint32_t IMRreg;
	uint32_t ICR1reg;
	uint32_t ICR2reg;
	uint32_t IERreg;
};

/*
 * Eintrag in die Interruptverteilung. Wird ausschliesslich vom
 * Oberbautreiber im Kernel benutzt; func laeuft im Threaded-IRQ-Kontext.
 */
struct __attribute__((packed)) PixyMvbBoardIrqServer {
	void (*func)(void *arg);
	void *arg;
	int irq_vect_id;
	int remove;
};

struct __attribute__((packed)) PixyMvbBoardEventMgr {
	void (*brd_evnt)(int brd_id, void *arg);
	void *arg;
};

struct __attribute__((packed)) PixyMvbBoardUpperDrvSubscribe {
	struct PixyMvbBoardEventMgr add_brd;
	struct PixyMvbBoardEventMgr rm_brd;
	uint32_t drv_id;
};

#define PIXY_MVB_BOARD_IOCTL_MAGIC	'L'

#define IOCTL_PIXY_MVB_BOARD_GET_MODULE_INFO	_IOR(PIXY_MVB_BOARD_IOCTL_MAGIC,  0, struct PixyMvbModuleVerStr)
#define IOCTL_PIXY_MVB_BOARD_GET_COREID		_IOR(PIXY_MVB_BOARD_IOCTL_MAGIC,  1, struct PixyMvbBoardCoreID)
#define IOCTL_PIXY_MVB_BOARD_GET_GPIO		_IOR(PIXY_MVB_BOARD_IOCTL_MAGIC,  2, struct PixyMvbBoardGPIO)
#define IOCTL_PIXY_MVB_BOARD_SET_GPIO_DAT	_IOW(PIXY_MVB_BOARD_IOCTL_MAGIC,  3, uint32_t)
#define IOCTL_PIXY_MVB_BOARD_CLR_GPIO_DAT	_IOW(PIXY_MVB_BOARD_IOCTL_MAGIC,  4, uint32_t)
#define IOCTL_PIXY_MVB_BOARD_GET_GPIO_DAT	_IOR(PIXY_MVB_BOARD_IOCTL_MAGIC,  5, uint32_t)
#define IOCTL_PIXY_MVB_BOARD_SET_GPIO_DIR	_IOW(PIXY_MVB_BOARD_IOCTL_MAGIC,  6, uint32_t)
#define IOCTL_PIXY_MVB_BOARD_GET_BAR0_SIZE	_IOR(PIXY_MVB_BOARD_IOCTL_MAGIC,  7, size_t)
#define IOCTL_PIXY_MVB_BOARD_GET_MAGIC_NUMBER	_IOR(PIXY_MVB_BOARD_IOCTL_MAGIC,  8, uint32_t)
#define IOCTL_PIXY_MVB_BOARD_GET_FW_VERSION	_IOR(PIXY_MVB_BOARD_IOCTL_MAGIC,  9, uint32_t)
#define IOCTL_PIXY_MVB_BOARD_GET_COREID_OFFSET	_IOR(PIXY_MVB_BOARD_IOCTL_MAGIC, 10, uint32_t)
#define IOCTL_PIXY_MVB_BOARD_GET_GPIO_OFFSET	_IOR(PIXY_MVB_BOARD_IOCTL_MAGIC, 11, uint32_t)
#define IOCTL_PIXY_MVB_BOARD_GET_ISA_OFFSET	_IOR(PIXY_MVB_BOARD_IOCTL_MAGIC, 12, uint32_t)
#define IOCTL_PIXY_MVB_BOARD_GET_SPAREID_OFFSET	_IOR(PIXY_MVB_BOARD_IOCTL_MAGIC, 13, uint32_t)
#define IOCTL_PIXY_MVB_BOARD_SET_SW_RESET	_IO (PIXY_MVB_BOARD_IOCTL_MAGIC, 14)
#define IOCTL_PIXY_MVB_BOARD_SET_FW_RELOAD	_IO (PIXY_MVB_BOARD_IOCTL_MAGIC, 15)

#ifdef __KERNEL__
/*
 * Kernelseitige Aufrufe. Das Argument ist hier ein KERNEL-Zeiger, nicht
 * ein Userspace-Zeiger - der Oberbautreiber ruft sie ueber
 * filp->f_op->unlocked_ioctl() auf. Es darf daher weder copy_to_user()
 * noch copy_from_user() verwendet werden.
 */
#define IOCTL_PIXY_MVB_BOARD_KGET_IRQ_VNUM	_IOR (PIXY_MVB_BOARD_IOCTL_MAGIC, 16, uintptr_t)
#define IOCTL_PIXY_MVB_BOARD_KGET_PISA		_IOR (PIXY_MVB_BOARD_IOCTL_MAGIC, 17, uintptr_t)
#define IOCTL_PIXY_MVB_BOARD_KSET_IRQ_SERVER	_IOW (PIXY_MVB_BOARD_IOCTL_MAGIC, 18, uintptr_t)
#define IOCTL_PIXY_MVB_BOARD_KGET_PGPIO		_IOR (PIXY_MVB_BOARD_IOCTL_MAGIC, 19, uintptr_t)
#define IOCTL_PIXY_MVB_BOARD_KSET_DRV_SUBSCRIBE	_IOWR(PIXY_MVB_BOARD_IOCTL_MAGIC, 20, uintptr_t)
#define IOCTL_PIXY_MVB_BOARD_KSET_DRV_UNSUBSCRIBE _IOW(PIXY_MVB_BOARD_IOCTL_MAGIC, 21, uintptr_t)
#endif /* __KERNEL__ */

#define PIXY_MVB_BOARD_MAX_IOCTL_NR	21

#endif /* PIXY_MVB_H */
