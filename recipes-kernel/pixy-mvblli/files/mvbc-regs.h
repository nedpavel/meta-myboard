/* SPDX-License-Identifier: GPL-2.0 */
/*
 * mvbc-regs.h - Register- und Speicherkarte des MVB-Controllers (MVBC02D)
 *
 * Der FPGA der Pixy-1000 bildet den klassischen MVBC im ISA-I/O-Raum nach.
 * Alle Zugriffe sind 16 Bit breit. Adressen sind Byteoffsets.
 */

#ifndef MVBC_REGS_H
#define MVBC_REGS_H

/* ---- ISA-Fensterregister (10 Bit dekodiert, Alias alle 0x400) ---- */
#define ISA_BASR0		0x320	/* Fensterbasis, Wert * 256 */
#define ISA_BASR1		0x322	/* Fenstermaske             */
#define ISA_BCR			0x324	/* Steuerung / Status       */

#define BCR_SETUP		0x1600	/* Fensterregister beschreibbar */
#define BCR_RUN			0x2600	/* Normalbetrieb                */

/* ---- Lage der Bereiche im Traffic Memory, indiziert mit MCM ---- */
#define TM_OFFSET_COUNT		5

#define TM_LA_PIT_OFFSETS	{ 0x00000, 0x00000, 0x00000, 0x00000, 0x00000 }
#define TM_DA_PIT_OFFSETS	{ 0x00000, 0x04000, 0x02000, 0x02000, 0x02000 }
#define TM_LA_PCS_OFFSETS	{ 0x03000, 0x03000, 0x0C000, 0x30000, 0x30000 }
#define TM_DA_PCS_OFFSETS	{ 0x00000, 0x07000, 0x0F000, 0x04000, 0x38000 }
#define TM_LA_DATA_OFFSETS	{ 0x01000, 0x01000, 0x04000, 0x10000, 0x10000 }
#define TM_DA_DATA_OFFSETS	{ 0x00000, 0x05000, 0x0E000, 0x38000, 0x40000 }
#define TM_LA_FRCE_OFFSETS	{ 0x02000, 0x02000, 0x08000, 0x20000, 0x20000 }
#define TM_SERVICE_OFFSETS	{ 0x03C00, 0x07C00, 0x0FC00, 0x0FC00, 0x0FC00 }
#define TM_PIT_BYTE_SIZES	{ 4096,    4096,    8192,    8192,    8192    }
#define TM_LA_PORT_COUNTS	{ 256,     256,     1024,    4096,    4096    }
#define TM_MEMORY_SIZES		{ 0x004000, 0x008000, 0x010000, 0x040000, 0x100000 }

#define TM_PORT_COUNT		4096

/* ---- Service Area, relativ zum Anfang der Service Area ---- */
#define SA_PP_DATA		0x000	/* 8 x 64 Byte physische Ports  */
#define SA_PP_PCS		0x200	/* 32 x 8 Byte                  */
#define SA_MFS			0x300	/* Master Frame Slot            */
#define SA_QDT			0x310	/* xmit_q[0], xmit_q[1], rcve_q */
#define SA_REGS			0x380	/* interne Register des MVBC    */

/* physische Ports, Index in pp_pcs / pp_data */
#define TM_PP_FC8		0x0
#define TM_PP_EFS		0x1
#define TM_PP_EF0		0x4
#define TM_PP_EF1		0x5
#define TM_PP_MOS		0x6
#define TM_PP_FC15		0x7
#define TM_PP_MSRC		0x8
#define TM_PP_MSNK		0xC

/* ---- interne Register: 16 Bit auf 4-Byte-Raster ab SA_REGS ---- */
#define MVBC_SCR		(SA_REGS + 0x00)
#define MVBC_MCR		(SA_REGS + 0x04)
#define MVBC_DR			(SA_REGS + 0x08)
#define MVBC_STSR		(SA_REGS + 0x0c)
#define MVBC_FC			(SA_REGS + 0x10)
#define MVBC_EC			(SA_REGS + 0x14)
#define MVBC_MFR		(SA_REGS + 0x18)
#define MVBC_MFRE		(SA_REGS + 0x1c)
#define MVBC_MR			(SA_REGS + 0x20)
#define MVBC_MR2		(SA_REGS + 0x24)
#define MVBC_DPR		(SA_REGS + 0x28)
#define MVBC_DPR2		(SA_REGS + 0x2c)
#define MVBC_IPR0		(SA_REGS + 0x30)
#define MVBC_IPR1		(SA_REGS + 0x34)
#define MVBC_IMR0		(SA_REGS + 0x38)
#define MVBC_IMR1		(SA_REGS + 0x3c)
#define MVBC_ISR0		(SA_REGS + 0x40)
#define MVBC_ISR1		(SA_REGS + 0x44)
#define MVBC_IVR0		(SA_REGS + 0x48)
#define MVBC_IVR1		(SA_REGS + 0x4c)
#define MVBC_DAOR		(SA_REGS + 0x58)
#define MVBC_DAOK		(SA_REGS + 0x5c)
#define MVBC_TCR		(SA_REGS + 0x60)
#define MVBC_TR1		(SA_REGS + 0x70)
#define MVBC_TR2		(SA_REGS + 0x74)
#define MVBC_TC1		(SA_REGS + 0x78)
#define MVBC_TC2		(SA_REGS + 0x7c)

/* SCR */
#define TM_SCR_IM		0x8000
#define TM_SCR_QUIET		0x4000
#define TM_SCR_MBC		0x2000
#define TM_SCR_TMO_MASK		0x0c00
#define TM_SCR_TMO_21US		0x0000
#define TM_SCR_TMO_43US		0x0400
#define TM_SCR_TMO_64US		0x0800
#define TM_SCR_TMO_83US		0x0c00
#define TM_SCR_WS_MASK		0x0300
#define TM_SCR_WS_0		0x0000
#define TM_SCR_WS_3		0x0300
#define TM_SCR_ARB_MASK		0x00c0
#define TM_SCR_ARB_3		0x00c0
#define TM_SCR_UTS		0x0020
#define TM_SCR_UTQ		0x0010
#define TM_SCR_MAS		0x0008
#define TM_SCR_RCEV		0x0004
#define TM_SCR_IL_MASK		0x0003
#define TM_SCR_IL_RESET		0x0000
#define TM_SCR_IL_CONFIG	0x0001
#define TM_SCR_IL_TEST		0x0002
#define TM_SCR_IL_RUNNING	0x0003

/* MCR: Bits 15:11 sind die READ-ONLY Chipversion */
#define TM_MCR_VERSION_SHIFT	11
#define TM_MCR_MCM_MASK		0x0007
#define TM_MCR_QO_SHIFT		3
#define TM_MCR_MO_SHIFT		5

/* Chipversionen laut MCR>>11 */
#define MVBC_VER_MVBC02A	1
#define MVBC_VER_MVBC02B	2
#define MVBC_VER_MVBC02C	3
#define MVBC_VER_MVBC02D	5
#define MVBC_VER_MVBC1S		6

/* MR */
#define TM_MR_BUSY		0x0200

/* DAOK */
#define TM_DAOK_ENABLE		0x0094

/* TCR */
#define TM_TCR_RS2		0x0020
#define TM_TCR_TA2		0x0010
#define TM_TCR_XSYN		0x0004
#define TM_TCR_RS1		0x0002
#define TM_TCR_TA1		0x0001

/* PCS Wort 0 */
#define TM_PCS_FCODE_MSK	0xf000
#define TM_PCS_FCODE_OFF	12
#define TM_PCS_TYPE_MSK		0x0c00
#define TM_PCS_TYPE_OFF		10
#define TM_PCS_TYPE_CLR		0
#define TM_PCS_TYPE_SNK		1
#define TM_PCS_TYPE_SRC		2
#define TM_PCS_DTI_MSK		0x00e0
#define TM_PCS_DTI_OFF		5
#define TM_PCS_QA		0x0004
#define TM_PCS_NUM		0x0002
#define TM_PCS_FE		0x0001

/* PCS Wort 1 */
#define TM_PCS_VP_MSK		0x0040

/*
 * Datenbereich: je vier Docks teilen sich einen 64-Byte-Block aus zwei
 * Seiten zu 32 Byte. Siehe TM_TYPE_DATA der Herstellerdefinition.
 */
#define TM_DOCK_SIZE		8
#define TM_PAGE_SIZE		32
#define TM_DATA_BLOCK_SIZE	64

static inline unsigned int tm_dock_offset(unsigned int idx, unsigned int page)
{
	return (idx >> 2) * TM_DATA_BLOCK_SIZE + page * TM_PAGE_SIZE +
	       (idx & 3) * TM_DOCK_SIZE;
}

/* Linked List Record der Message-Queues */
struct mvbc_llr {
	__u16 p16_data;
	__u16 p16_next;
};

#define MVB_MSG_FRAME_SIZE	32

#endif /* MVBC_REGS_H */
