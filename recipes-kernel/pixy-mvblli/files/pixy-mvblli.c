// SPDX-License-Identifier: GPL-2.0
/*
 * pixy-mvblli - MVB Link Layer Interface fuer die Pixy-1000
 *
 * Setzt auf dem Board-Treiber pixy-mvb auf und bedient den im FPGA
 * nachgebildeten MVB-Controller (MVBC02D). Nach aussen stellt das Modul
 * /dev/mvblliN bereit:
 *
 *   open()    initialisiert den Controller, exklusiv (zweites open: EBUSY)
 *   close()   baut ihn wieder ab
 *   read()    Prozessdaten eines Ports bzw. ein Message-Frame
 *   write()   Prozessdaten schreiben bzw. ein Message-Frame senden
 *   poll()    wartet auf eingegangene Message-Frames
 *   ioctl()   Konfiguration, Statistik, Start/Stop
 *
 * Die Kopplung an pixy-mvb laeuft ueber filp_open("/dev/mvbN") und dessen
 * K*-ioctls - es gibt bewusst keine Symbolabhaengigkeit zwischen den
 * beiden Modulen.
 */

#include <linux/cdev.h>
#include <linux/delay.h>
#include <linux/device.h>
#include <linux/file.h>
#include <linux/fs.h>
#include <linux/io.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/moduleparam.h>
#include <linux/mutex.h>
#include <linux/poll.h>
#include <linux/sched.h>
#include <linux/slab.h>
#include <linux/spinlock.h>
#include <linux/uaccess.h>
#include <linux/wait.h>

#include "pixy-mvb.h"
#include "pixy-mvblli.h"
#include "mvbc-regs.h"

#define DRV_NAME		"pixy-mvblli"
#define DRV_VERSION_STR		"pixy-mvblli v3.0.0 - MVB Link Layer Interface"

#define MVBLLI_MAX_DEV		3
#define MVBLLI_MINOR_COUNT	0x2fd
#define MVBLLI_NSDB_SIZE	0x1000

/* Groesse des Software-Empfangsrings fuer Message-Frames */
#define MVBLLI_RCV_RING_ELEMS	0xde

/* Lage der drei Message-Queues im Traffic Memory (Byteoffsets) */
#define MD_TQ0_OFFSET		0x08000
#define MD_TQ0_LLRS		8
#define MD_TQ1_OFFSET		0x08140
#define MD_TQ1_LLRS		0xde
#define MD_RQ_OFFSET		0x0a0a0
#define MD_RQ_LLRS		0xde

static int mvb_irq;
module_param(mvb_irq, int, 0444);
MODULE_PARM_DESC(mvb_irq,
	"0 = Message-Daten werden gepollt (Vorgabe), >0 = Interruptbetrieb");

struct mvblli_dev {
	int brd_id;
	int enable;
	dev_t devt;
	struct cdev cdev;
	struct device *sysdev;

	unsigned long dev_flags;	/* Bit 0: Geraet belegt */
#define MVBLLI_FLAG_BUSY	0
	int users;

	/* vom Board-Treiber geliehen */
	void __iomem *pisa;
	void __iomem *pgpio;
	int irq_vnum;

	/* Traffic Memory */
	void __iomem *p_tm;
	void __iomem *p_sa;
	u32 tm_size;
	int mcm;

	u32 off_la_pit, off_da_pit;
	u32 off_la_pcs, off_da_pcs;
	u32 off_la_data, off_da_data;
	u32 off_la_frce;
	u32 la_port_count;
	u32 pit_bytes;
	int pit_type;			/* 1 = 16-Bit-Eintraege */

	u16 ts_id;
	u16 mvb_addr;
	int media_type;			/* 0 = ESD, sonst EMD */
	u16 waitstates;

	/* Konfiguration des Traffic Store (HWINIT) */
	u8 ownership;
	u8 ts_type;
	u8 auto_reset_rld;
	u16 prt_addr_max;
	u16 prt_indx_max;

	u16 tmo_shift;			/* Schiebeweite fuer mvb_port.freshness */
	u16 *all_tacks;			/* Sink-Time-Schwelle je Portadresse */

	mvb_stat status;

	/* Message-Daten */
	u16 q_tq_priority;
	u16 p16_tq0, p16_tq1, p16_rq;	/* Softwareposition im jeweiligen Ring */
	u32 rq_overflow;

	u8 *rcv_ring;			/* MVBLLI_RCV_RING_ELEMS * 32 Byte */
	int rcv_elems, rcv_filled, rcv_rd, rcv_wr;
	spinlock_t md_lock;
	wait_queue_head_t wait_poll;

	struct mutex lock;
};

static struct mvblli_drvr {
	dev_t devt_base;
	int total_devices;
	struct class *cls;
	struct mvblli_dev dev[MVBLLI_MAX_DEV];
	int drv_id;
} drvdata;

/* ------------------------------------------------------- TM-Zugriffe */

static inline u16 tm_r16(struct mvblli_dev *d, u32 off)
{
	return ioread16(d->p_tm + off);
}

static inline void tm_w16(struct mvblli_dev *d, u32 off, u16 v)
{
	iowrite16(v, d->p_tm + off);
}

static inline u16 sa_r16(struct mvblli_dev *d, u32 off)
{
	return ioread16(d->p_sa + off);
}

static inline void sa_w16(struct mvblli_dev *d, u32 off, u16 v)
{
	iowrite16(v, d->p_sa + off);
}

/* 16-bittige Blockkopien - der MVBC vertraegt keine breiteren Zugriffe */
static void tm_read_block(struct mvblli_dev *d, u32 off, u16 *dst, unsigned int words)
{
	unsigned int i;

	for (i = 0; i < words; i++)
		dst[i] = tm_r16(d, off + i * 2);
}

static void tm_write_block(struct mvblli_dev *d, u32 off, const u16 *src,
			   unsigned int words)
{
	unsigned int i;

	for (i = 0; i < words; i++)
		tm_w16(d, off + i * 2, src[i]);
}

static void tm_memset16(struct mvblli_dev *d, u32 off, u16 val, unsigned int words)
{
	unsigned int i;

	for (i = 0; i < words; i++)
		tm_w16(d, off + i * 2, val);
}

/* p16-Zeiger des MVBC sind 4-Byte-Einheiten relativ zur TM-Basis */
static inline u32 p16_to_off(u16 p16)
{
	return (u32)p16 * 4;
}

static inline u16 off_to_p16(u32 off)
{
	return (u16)(off / 4);
}

/* ------------------------------------------------------- Port-Hilfen */

static int lp_len_2_fcode(u16 length)
{
	switch (length) {
	case 2:  return 0;
	case 4:  return 1;
	case 8:  return 2;
	case 16: return 3;
	case 32: return 4;
	default: return -1;
	}
}

/* Dock-Index eines Ports aus der Port Index Table */
static u16 lp_port_index(struct mvblli_dev *d, u16 port)
{
	if (d->pit_type)
		return tm_r16(d, d->off_la_pit + port * 2);

	/* kleinere Layouts packen zwei 8-Bit-Eintraege in ein Wort */
	{
		u16 w = tm_r16(d, d->off_la_pit + (port & ~1u));

		return (port & 1) ? (w >> 8) : (w & 0xff);
	}
}

static inline u32 lp_pcs_off(struct mvblli_dev *d, u16 idx)
{
	return d->off_la_pcs + (u32)idx * 8;
}

static inline u32 lp_data_off(struct mvblli_dev *d, u16 idx, unsigned int page)
{
	if (d->ts_type == 3)
		return d->off_la_data + (u32)idx * 256 + page * 128;

	return d->off_la_data + tm_dock_offset(idx, page);
}

/* --------------------------------------------- Prozessdaten lesen/schreiben */

/* Rueckgabe wie im Original: 0 = ok, 3 = Laenge passt nicht, 8 = Port unbekannt */
static int apd_get_port(struct mvblli_dev *d, u16 port, u16 *tack,
			u16 *data, u16 length)
{
	u16 idx = lp_port_index(d, port);
	u32 pcs;
	u16 w0, w1;

	if (!idx || idx > d->prt_indx_max)
		return 8;

	pcs = lp_pcs_off(d, idx);
	w0 = tm_r16(d, pcs);
	*tack = tm_r16(d, pcs + 4);

	if (lp_len_2_fcode(length) != (w0 >> TM_PCS_FCODE_OFF))
		return 3;

	w1 = tm_r16(d, pcs + 2);
	tm_read_block(d, lp_data_off(d, idx, (w1 & TM_PCS_VP_MSK) ? 1 : 0),
		      data, length / 2);

	return 0;
}

static int apd_put_port(struct mvblli_dev *d, u16 port, const u16 *data,
			u16 length)
{
	u16 idx = lp_port_index(d, port);
	u32 pcs;
	u16 w0, w1;

	if (!idx || idx > d->prt_indx_max)
		return 8;

	pcs = lp_pcs_off(d, idx);
	w0 = tm_r16(d, pcs);

	/* auf eine Senke darf nicht geschrieben werden */
	if (((w0 >> TM_PCS_TYPE_OFF) & TM_PCS_TYPE_SNK) != 0)
		return 8;

	if (lp_len_2_fcode(length) != (w0 >> TM_PCS_FCODE_OFF))
		return 3;

	/*
	 * Doppelpufferung: geschrieben wird immer in die gerade NICHT
	 * sichtbare Seite, danach wird VP umgeschaltet.
	 */
	w1 = tm_r16(d, pcs + 2);
	tm_write_block(d, lp_data_off(d, idx, (w1 & TM_PCS_VP_MSK) ? 0 : 1),
		       data, length / 2);
	tm_w16(d, pcs + 2, w1 ^ TM_PCS_VP_MSK);

	/* noch nicht typisierter Port wird beim ersten Schreiben zur Quelle */
	if ((w0 & TM_PCS_TYPE_MSK) == 0)
		tm_w16(d, pcs,
		       (w0 & ~TM_PCS_TYPE_MSK) |
		       (TM_PCS_TYPE_SRC << TM_PCS_TYPE_OFF));

	return 0;
}

/* --------------------------------------------------- MVBC-Grundfunktionen */

static void mvb_wait(struct mvblli_dev *d, unsigned int usec)
{
	usleep_range(usec, usec + usec / 4 + 1);
}

static int mvb_set_device_address(struct mvblli_dev *d, u16 addr)
{
	if (addr > 0xfff)
		return -EINVAL;

	sa_w16(d, MVBC_DAOR, addr);
	sa_w16(d, MVBC_DAOK, TM_DAOK_ENABLE);
	d->mvb_addr = addr;
	d->status.mvb_addr = addr;

	return 0;
}

static u16 mvb_get_device_address(struct mvblli_dev *d)
{
	return sa_r16(d, MVBC_DAOR) & 0xfff;
}

static int mvb_tmo_config(struct mvblli_dev *d, u16 stsr)
{
	sa_w16(d, MVBC_STSR, stsr);
	return 0;
}

static int mvb_go(struct mvblli_dev *d)
{
	u16 scr = sa_r16(d, MVBC_SCR);

	sa_w16(d, MVBC_SCR, (scr & ~TM_SCR_IL_MASK) | TM_SCR_IL_RUNNING);
	d->status.is_active = 1;

	return 0;
}

static int mvb_stop(struct mvblli_dev *d)
{
	u16 scr = sa_r16(d, MVBC_SCR);

	sa_w16(d, MVBC_SCR, (scr & ~TM_SCR_IL_MASK) | TM_SCR_IL_CONFIG);
	d->status.is_active = 0;

	return 0;
}

/*
 * Konfiguration des Controllers. Reihenfolge und Werte sind dem Original
 * nachgebildet; die Pruefungen dazwischen stellen sicher, dass der MVBC
 * wirklich antwortet und das Traffic Memory les- und schreibbar ist.
 */
static int mvb_config(struct mvblli_dev *d)
{
	static const u32 sa_offs[TM_OFFSET_COUNT] = TM_SERVICE_OFFSETS;
	static const u32 la_pit[TM_OFFSET_COUNT] = TM_LA_PIT_OFFSETS;
	static const u32 da_pit[TM_OFFSET_COUNT] = TM_DA_PIT_OFFSETS;
	static const u32 la_pcs[TM_OFFSET_COUNT] = TM_LA_PCS_OFFSETS;
	static const u32 da_pcs[TM_OFFSET_COUNT] = TM_DA_PCS_OFFSETS;
	static const u32 la_dat[TM_OFFSET_COUNT] = TM_LA_DATA_OFFSETS;
	static const u32 da_dat[TM_OFFSET_COUNT] = TM_DA_DATA_OFFSETS;
	static const u32 la_frc[TM_OFFSET_COUNT] = TM_LA_FRCE_OFFSETS;
	static const u32 pit_sz[TM_OFFSET_COUNT] = TM_PIT_BYTE_SIZES;
	static const u32 la_cnt[TM_OFFSET_COUNT] = TM_LA_PORT_COUNTS;
	u16 scr, ver, marker;
	int tries;
	u32 i;

	if (d->mcm < 0 || d->mcm >= TM_OFFSET_COUNT)
		return -EINVAL;

	/*
	 * Der Registersatz liegt je nach Speicherausbau an einer anderen
	 * Stelle. Solange die Groesse noch nicht feststeht, wird SCR an
	 * jedem in Frage kommenden Ort beschrieben.
	 */
	for (i = 0; i < TM_OFFSET_COUNT; i++)
		tm_w16(d, sa_offs[i] + MVBC_SCR, d->waitstates | 0x04c0);

	d->p_sa = d->p_tm + sa_offs[d->mcm];

	ver = sa_r16(d, MVBC_MCR) >> TM_MCR_VERSION_SHIFT;
	if (ver != MVBC_VER_MVBC02D) {
		pr_err(DRV_NAME ": unexpected MVBC version %u\n", ver);
		return -ENODEV;
	}

	scr = d->waitstates | 0x84c5;
	sa_w16(d, MVBC_SCR, scr);
	sa_w16(d, MVBC_DPR, 0xffff);
	if (sa_r16(d, MVBC_SCR) != scr) {
		pr_err(DRV_NAME ": SCR does not read back\n");
		return -EIO;
	}
	if (sa_r16(d, MVBC_DPR) != 0xfffc) {
		pr_err(DRV_NAME ": DPR does not read back\n");
		return -EIO;
	}

	sa_w16(d, MVBC_IMR0, 0);
	sa_w16(d, MVBC_IMR1, 0);
	sa_w16(d, MVBC_ISR0, 0);
	sa_w16(d, MVBC_ISR1, 0);

	/* Message-Ports vorbelegen */
	sa_w16(d, SA_PP_PCS + TM_PP_MSRC * 8, 0x1802);
	sa_w16(d, SA_PP_PCS + TM_PP_MSRC * 8 + 2, 0);
	sa_w16(d, SA_PP_PCS + TM_PP_MSNK * 8, 0x1402);
	sa_w16(d, SA_PP_PCS + TM_PP_MSNK * 8 + 2, 0);

	/* Markierung, die den Initialisierungslauf ueberleben muss */
	sa_w16(d, SA_PP_DATA + TM_PP_EFS * 64, 0xa55a);
	sa_w16(d, SA_MFS, 0);

	sa_w16(d, MVBC_SCR, d->waitstates | 0x84fe);
	mvb_wait(d, 2000);
	sa_w16(d, SA_MFS, 0x1001);
	sa_w16(d, MVBC_MR, 0x0020);		/* Initialisierungslauf starten */

	for (tries = 0x32; tries > 0; tries--) {
		mvb_wait(d, 100);
		if (!(sa_r16(d, MVBC_MR) & TM_MR_BUSY))
			break;
	}

	marker = sa_r16(d, SA_PP_DATA + TM_PP_EFS * 64);
	mvb_wait(d, 2000);
	sa_w16(d, MVBC_SCR, scr);

	if (marker != 0xa55a) {
		pr_err(DRV_NAME ": traffic memory self test failed (0x%04x)\n",
		       marker);
		return -EIO;
	}

	/* Leitungsart: EMD schaltet zwei Bits im Decoder-Register */
	if (d->media_type) {
		sa_w16(d, MVBC_DR, sa_r16(d, MVBC_DR) | 0x0020);
		sa_w16(d, MVBC_DR, sa_r16(d, MVBC_DR) | 0x4000);
	}

	/*
	 * MCR: nur die unteren Bits beschreiben. 15:11 sind die
	 * Chipversion und read-only.
	 */
	sa_w16(d, MVBC_MCR, d->mcm & TM_MCR_MCM_MASK);

	/* Ablagen im Traffic Memory merken und Porttabellen loeschen */
	d->off_la_pit  = la_pit[d->mcm];
	d->off_da_pit  = da_pit[d->mcm];
	d->off_la_pcs  = la_pcs[d->mcm];
	d->off_da_pcs  = da_pcs[d->mcm];
	d->off_la_data = la_dat[d->mcm];
	d->off_da_data = da_dat[d->mcm];
	d->off_la_frce = la_frc[d->mcm];
	d->pit_bytes   = pit_sz[d->mcm];
	d->la_port_count = la_cnt[d->mcm];
	d->pit_type    = (d->mcm >= 3);

	tm_memset16(d, d->off_la_pit, 0, d->pit_bytes / 2);
	tm_memset16(d, d->off_da_pit, 0, d->pit_bytes / 2);
	tm_w16(d, d->off_la_pcs, 0);
	tm_w16(d, d->off_la_pcs + 2, 0);
	tm_w16(d, d->off_da_pcs, 0);
	tm_w16(d, d->off_da_pcs + 2, 0);

	/* Service Area: physische Ports und PCS zuruecksetzen */
	tm_memset16(d, sa_offs[d->mcm] + SA_PP_PCS, 0, 0x80);
	tm_memset16(d, sa_offs[d->mcm] + SA_PP_DATA, 0, 0x100);

	/* Device Status Report Port */
	sa_w16(d, SA_PP_PCS + TM_PP_FC15 * 8, 0xf842);

	return 0;
}

static int mvb_hardw_config(struct mvblli_dev *d)
{
	/* Sink-Time-Raster und Timer wie im laufenden Original */
	sa_w16(d, MVBC_STSR, 0x1000);
	sa_w16(d, MVBC_TCR, TM_TCR_RS1 | TM_TCR_RS2);

	return 0;
}

/* ------------------------------------------------- Message-Queues (MD) */

/*
 * Baut aus einem zusammenhaengenden TM-Bereich einen zirkulaeren Ring aus
 * LLRs und den zugehoerigen 32-Byte-Puffern. Rueckgabe ist der p16-Zeiger
 * auf das erste LLR.
 */
static u16 mvb_md_install_q(struct mvblli_dev *d, u32 base_off, u16 llr_count)
{
	u32 llr_off = base_off;
	u32 data_off;
	u16 i;

	/* Die Puffer liegen hinter dem LLR-Feld, auf 32 Byte ausgerichtet */
	data_off = ALIGN(base_off + (u32)llr_count * 4, MVB_MSG_FRAME_SIZE);

	for (i = 0; i < llr_count; i++) {
		u32 this_llr = llr_off + (u32)i * 4;
		u32 next_llr = llr_off + (u32)((i + 1) % llr_count) * 4;

		/*
		 * Das erste LLR ist der Waechter: sein Datenzeiger bleibt 0.
		 * Alle anderen bekommen einen Puffer.
		 */
		tm_w16(d, this_llr,
		       i ? off_to_p16(data_off + (u32)(i - 1) *
					       MVB_MSG_FRAME_SIZE) : 0);
		tm_w16(d, this_llr + 2, off_to_p16(next_llr));
	}

	return off_to_p16(llr_off);
}

static int mvb_md_q_init(struct mvblli_dev *d)
{
	u16 msnk;

	d->p16_tq0 = mvb_md_install_q(d, MD_TQ0_OFFSET, MD_TQ0_LLRS);
	d->p16_tq1 = mvb_md_install_q(d, MD_TQ1_OFFSET, MD_TQ1_LLRS);
	d->p16_rq  = mvb_md_install_q(d, MD_RQ_OFFSET,  MD_RQ_LLRS);

	/*
	 * Die QDT-Eintraege zeigen auf das Waechterelement. Die Sendequeues
	 * werden erst beim ersten Senden eingetragen - so haelt es auch das
	 * Original.
	 */
	sa_w16(d, SA_QDT + 0, 0);
	sa_w16(d, SA_QDT + 2, 0);
	sa_w16(d, SA_QDT + 4, tm_r16(d, p16_to_off(d->p16_rq) + 2));

	/* Ereignisframe-Quellports */
	sa_w16(d, SA_PP_PCS + TM_PP_EF0 * 8, 0x9802);
	sa_w16(d, SA_PP_PCS + TM_PP_EF1 * 8, 0x9802);

	/* Message-Quelle und -Senke scharf schalten */
	sa_w16(d, SA_PP_PCS + TM_PP_MSRC * 8, 0xc81c);
	msnk = 0xc404;
	if (mvb_irq > 0)
		msnk |= 0x20;
	sa_w16(d, SA_PP_PCS + TM_PP_MSNK * 8, msnk);

	d->status.has_md = 1;

	return 0;
}

/* Ein empfangenes Frame aus dem TM in den Software-Ring uebernehmen */
static void mvb_md_store(struct mvblli_dev *d, u32 data_off)
{
	unsigned long flags;
	u8 *slot;

	spin_lock_irqsave(&d->md_lock, flags);
	if (d->rcv_filled >= d->rcv_elems) {
		/*
		 * Ring voll: das Frame wird stillschweigend verworfen.
		 * Das Original verhaelt sich genauso - kein Fehler, kein Log.
		 */
		d->rq_overflow |= 4;
		spin_unlock_irqrestore(&d->md_lock, flags);
		return;
	}
	slot = d->rcv_ring + (size_t)d->rcv_wr * MVB_MSG_FRAME_SIZE;
	spin_unlock_irqrestore(&d->md_lock, flags);

	tm_read_block(d, data_off, (u16 *)slot, MVB_MSG_FRAME_SIZE / 2);

	spin_lock_irqsave(&d->md_lock, flags);
	d->rcv_wr = (d->rcv_wr + 1) % d->rcv_elems;
	d->rcv_filled++;
	spin_unlock_irqrestore(&d->md_lock, flags);
}

/*
 * Holt alle vom Controller abgelegten Frames aus dem Empfangsring des
 * Traffic Memory. Der Waechter wandert dabei mit.
 */
static void mvb_md_dispatcher(struct mvblli_dev *d)
{
	int guard = MD_RQ_LLRS + 1;

	while (guard--) {
		u32 cur = p16_to_off(d->p16_rq);
		u16 nxt_p16 = tm_r16(d, cur + 2);
		u32 nxt;
		u16 buf_p16;

		if (sa_r16(d, SA_QDT + 4) == nxt_p16)
			break;			/* nichts abzuholen */

		nxt = p16_to_off(nxt_p16);
		buf_p16 = tm_r16(d, nxt);
		if (!buf_p16)
			break;

		/* Link_Header pruefen: nur TCN-RTP-Frames (PT = 1000b) */
		if ((tm_r16(d, p16_to_off(buf_p16) + 2) & 0xf0) == 0x80)
			mvb_md_store(d, p16_to_off(buf_p16));

		tm_w16(d, nxt, 0);		/* Waechter rueckt weiter */
		tm_w16(d, cur, buf_p16);	/* Puffer ans vorherige LLR */
		d->p16_rq = nxt_p16;
	}
}

/*
 * Ein Frame in die gewuenschte Sendequeue einhaengen. control != 0 waehlt
 * die hochpriore Queue. packet zeigt auf 32 Byte; die ersten vier werden
 * mit dem Link_Header ueberschrieben.
 */
static int mvb_sndp(struct mvblli_dev *d, u16 dd, int control, const u16 *packet)
{
	u16 *cur_p16 = control ? &d->p16_tq0 : &d->p16_tq1;
	u32 qdt_off = SA_QDT + (control ? 0 : 2);
	u16 own = mvb_get_device_address(d);
	u16 mode, cur, nxt_p16, buf_p16;
	u32 cur_off, nxt_off, buf_off;

	/* Sendequeues beim ersten Senden in die QDT eintragen */
	if (!sa_r16(d, SA_QDT + 0) || !sa_r16(d, SA_QDT + 2)) {
		sa_w16(d, SA_QDT + 0, d->p16_tq0);
		sa_w16(d, SA_QDT + 2, d->p16_tq1);
	}

	mode = ((dd & 0xfff) == 0xfff) ? 0xf000 : 0x1000;

	cur = *cur_p16;
	cur_off = p16_to_off(cur);
	nxt_p16 = tm_r16(d, cur_off + 2);

	if (sa_r16(d, qdt_off) == nxt_p16)
		return 1;			/* Queue voll */

	nxt_off = p16_to_off(nxt_p16);
	buf_p16 = tm_r16(d, nxt_off);
	if (!buf_p16)
		return 1;
	buf_off = p16_to_off(buf_p16);

	/*
	 * Immer volle 32 Byte kopieren - unabhaengig davon, was das
	 * SZ-Feld als gueltig erklaert. Das Original tut dasselbe, und
	 * abweichendes Verhalten waere auf dem Bus sichtbar.
	 */
	tm_write_block(d, buf_off, packet, MVB_MSG_FRAME_SIZE / 2);

	/* Link_Header nach IEC 61375-1, Fig. 120 */
	tm_w16(d, buf_off, (u16)((dd << 8) | (((dd & 0xfff) | mode) >> 8)));
	tm_w16(d, buf_off + 2, (u16)(((own >> 8) & 0xf) | 0x80 | (own << 8)));

	tm_w16(d, nxt_off, 0);			/* Waechter weiter */
	tm_w16(d, cur_off, buf_p16);		/* Puffer einhaengen */
	*cur_p16 = nxt_p16;

	/* Ereignisframe ausloesen, Prioritaet waehlt den Port */
	{
		u16 ev = (own & 0xfff) | 0xc000;
		u32 pcs = SA_PP_PCS + (control ? TM_PP_EF1 : TM_PP_EF0) * 8;
		u32 dat = SA_PP_DATA + (control ? TM_PP_EF1 : TM_PP_EF0) * 64;
		u16 w1 = sa_r16(d, pcs + 2);

		if (!(w1 & TM_PCS_VP_MSK)) {
			sa_w16(d, dat + 32, ev);
			sa_w16(d, pcs + 2, w1 | TM_PCS_VP_MSK);
		} else {
			sa_w16(d, dat, ev);
			sa_w16(d, pcs + 2, w1 & ~TM_PCS_VP_MSK);
		}
		sa_w16(d, MVBC_MR2, control ? 0x2000 : 0x1000);
	}

	return 0;
}

static void mvb_md_flush_send_queue(struct mvblli_dev *d)
{
	sa_w16(d, SA_QDT + 0, 0);
	sa_w16(d, SA_QDT + 2, 0);
	d->p16_tq0 = mvb_md_install_q(d, MD_TQ0_OFFSET, MD_TQ0_LLRS);
	d->p16_tq1 = mvb_md_install_q(d, MD_TQ1_OFFSET, MD_TQ1_LLRS);
}

/*
 * Status der Message-Schicht. Bit 1: Message-Senke nicht aufgefrischt,
 * Bit 2: Message-Quelle nicht aufgefrischt, Bit 4: Empfangsqueue lief
 * ueber. selector maskiert die Rueckgabe, reset loescht.
 */
static u32 mvb_md_get_status(struct mvblli_dev *d, u16 selector, u16 reset)
{
	u32 st = d->rq_overflow;

	if (sa_r16(d, SA_PP_PCS + TM_PP_MSRC * 8 + 4) == 0xffff)
		st |= 2;
	if (sa_r16(d, SA_PP_PCS + TM_PP_MSNK * 8 + 4) == 0xffff)
		st |= 1;

	if ((st & reset) & 2)
		sa_w16(d, SA_PP_PCS + TM_PP_MSRC * 8 + 4, 0);
	if ((st & reset) & 1)
		sa_w16(d, SA_PP_PCS + TM_PP_MSNK * 8 + 4, 0);
	if ((st & reset) & 4)
		d->rq_overflow = 0;

	return st & selector;
}

/* --------------------------------------------------------- Interrupt */

static void mvblli_irq_server(void *arg)
{
	struct mvblli_dev *d = arg;
	u16 isr0, isr1;

	if (!d->enable || !d->status.is_init)
		return;

	isr0 = sa_r16(d, MVBC_ISR0);
	isr1 = sa_r16(d, MVBC_ISR1);

	/* Quittieren durch Zurueckschreiben der gemeldeten Bits */
	if (isr0)
		sa_w16(d, MVBC_ISR0, isr0);
	if (isr1)
		sa_w16(d, MVBC_ISR1, isr1);

	if (isr1 & 0x0800)		/* RQE: Empfangsqueue hat etwas */
		mvb_md_dispatcher(d);
	if (isr1 & 0x0400)		/* RQC/Overflow */
		d->rq_overflow |= 4;

	wake_up_interruptible(&d->wait_poll);
}

/* -------------------------------------------- Anbindung an pixy-mvb */

static long board_ioctl(struct file *filp, unsigned int cmd, void *arg)
{
	if (!filp->f_op || !filp->f_op->unlocked_ioctl)
		return -ENOTTY;

	return filp->f_op->unlocked_ioctl(filp, cmd, (unsigned long)arg);
}

static int mvblli_attach_board(struct mvblli_dev *d)
{
	struct PixyMvbBoardIrqServer srv;
	char path[24];
	struct file *filp;
	void *p;
	u32 vnum = 0, gpio_dat;
	int ret = 0;

	snprintf(path, sizeof(path), "/dev/mvb%d", d->brd_id);
	filp = filp_open(path, O_RDONLY, 0);
	if (IS_ERR(filp)) {
		pr_err(DRV_NAME ": cannot open %s\n", path);
		return -ENODEV;
	}

	if (board_ioctl(filp, IOCTL_PIXY_MVB_BOARD_KGET_PGPIO, &p)) {
		pr_err(DRV_NAME ": cannot get GPIO block\n");
		ret = -ENODEV;
		goto out;
	}
	d->pgpio = p;

	gpio_dat = ioread32(d->pgpio);
	if (!(gpio_dat & PIXY_MVB_BOARD_GPIO_MVB_PC104n)) {
		pr_err(DRV_NAME ": board %d is not an MVB board\n", d->brd_id);
		ret = -ENODEV;
		goto out;
	}
	d->media_type = gpio_dat & PIXY_MVB_BOARD_GPIO_EMD_ESDn;

	if (board_ioctl(filp, IOCTL_PIXY_MVB_BOARD_KGET_PISA, &p)) {
		pr_err(DRV_NAME ": cannot get ISA block\n");
		ret = -ENODEV;
		goto out;
	}
	d->pisa = p;

	if (board_ioctl(filp, IOCTL_PIXY_MVB_BOARD_KGET_IRQ_VNUM, &vnum)) {
		pr_err(DRV_NAME ": cannot get irq vector count\n");
		ret = -ENODEV;
		goto out;
	}
	d->irq_vnum = vnum;

	srv.func = mvblli_irq_server;
	srv.arg = d;
	srv.irq_vect_id = (vnum > 1) ? 1 : 0;
	srv.remove = 0;
	if (board_ioctl(filp, IOCTL_PIXY_MVB_BOARD_KSET_IRQ_SERVER, &srv)) {
		pr_err(DRV_NAME ": cannot register irq server\n");
		ret = -ENODEV;
		goto out;
	}

out:
	filp_close(filp, NULL);
	return ret;
}

static void mvblli_detach_board(struct mvblli_dev *d)
{
	struct PixyMvbBoardIrqServer srv;
	char path[24];
	struct file *filp;

	snprintf(path, sizeof(path), "/dev/mvb%d", d->brd_id);
	filp = filp_open(path, O_RDONLY, 0);
	if (IS_ERR(filp))
		return;

	srv.func = mvblli_irq_server;
	srv.arg = d;
	srv.irq_vect_id = (d->irq_vnum > 1) ? 1 : 0;
	srv.remove = 1;
	board_ioctl(filp, IOCTL_PIXY_MVB_BOARD_KSET_IRQ_SERVER, &srv);

	filp_close(filp, NULL);
}

/*
 * Groesse des Traffic Memory ermitteln: absteigend 1 MiB, 512 K, 256 K,
 * 128 K, 64 K probieren. Je Kandidat wird das ISA-Fenster gesetzt und
 * geprueft, ob sich der obere Teil des Fensters vom unteren unterscheidet
 * (kein Alias). Der urspruengliche Speicherinhalt wird wiederhergestellt.
 */
static int mvblli_detect_tm(struct mvblli_dev *d)
{
	static const struct {
		u16 basr0;
		u32 probe;	/* Byteoffset der Alias-Probe */
		int mcm;
	} cand[] = {
		{ 0x1000, 0x80000, 4 },
		{ 0x0400, 0x20000, 3 },
		{ 0x0100, 0x08000, 2 },
		{ 0x0180, 0x04000, 1 },
		{ 0x0140, 0x02000, 0 },
	};
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(cand); i++) {
		void __iomem *win;
		u16 save0, save1;
		bool ok;

		iowrite16(BCR_SETUP, d->pisa + ISA_BCR);
		iowrite16(cand[i].basr0, d->pisa + ISA_BASR0);
		iowrite16((u16)(0xffffu << (ffs(cand[i].basr0) - 1)),
			  d->pisa + ISA_BASR1);
		iowrite16(BCR_RUN, d->pisa + ISA_BCR);

		win = d->pisa + (u32)cand[i].basr0 * 256;

		save0 = ioread16(win);
		save1 = ioread16(win + cand[i].probe);
		iowrite16(0x5555, win);
		iowrite16(0xaaaa, win + cand[i].probe);
		ok = (ioread16(win) == 0x5555) &&
		     (ioread16(win + cand[i].probe) == 0xaaaa);
		iowrite16(save1, win + cand[i].probe);
		iowrite16(save0, win);

		if (ok) {
			static const u32 sizes[TM_OFFSET_COUNT] = TM_MEMORY_SIZES;

			d->p_tm = win;
			d->mcm = cand[i].mcm;
			d->tm_size = sizes[cand[i].mcm];
			pr_info(DRV_NAME
				": traffic memory %u KiB (mcm=%d) at ISA+0x%x\n",
				d->tm_size / 1024, d->mcm,
				(u32)cand[i].basr0 * 256);
			return 0;
		}
	}

	d->mcm = -1;
	pr_err(DRV_NAME ": cannot determine traffic memory size\n");

	return -ENODEV;
}

/* --------------------------------------------- Initialisierung/Abbau */

static int mvb_init_board(struct mvblli_dev *d)
{
	int ret;

	ret = mvblli_attach_board(d);
	if (ret)
		return ret;

	ret = mvblli_detect_tm(d);
	if (ret)
		return ret;

	d->ts_id = d->brd_id;
	d->waitstates = 0;
	d->prt_addr_max = TM_PORT_COUNT - 1;
	d->prt_indx_max = 0xfff;
	d->q_tq_priority = 0;
	d->rq_overflow = 0;

	ret = mvb_config(d);
	if (ret)
		return ret;

	ret = mvb_hardw_config(d);
	if (ret)
		return ret;

	ret = mvb_set_device_address(d, 0);
	if (ret)
		return ret;

	d->all_tacks = kcalloc(TM_PORT_COUNT, sizeof(u16), GFP_KERNEL);
	if (!d->all_tacks)
		return -ENOMEM;

	d->rcv_elems = MVBLLI_RCV_RING_ELEMS;
	d->rcv_ring = kcalloc(d->rcv_elems, MVB_MSG_FRAME_SIZE, GFP_KERNEL);
	if (!d->rcv_ring) {
		kfree(d->all_tacks);
		d->all_tacks = NULL;
		return -ENOMEM;
	}
	d->rcv_filled = d->rcv_rd = d->rcv_wr = 0;

	ret = mvb_md_q_init(d);
	if (ret) {
		kfree(d->rcv_ring);
		kfree(d->all_tacks);
		d->rcv_ring = NULL;
		d->all_tacks = NULL;
		return ret;
	}

	memset(&d->status, 0, sizeof(d->status));
	scnprintf(d->status.hw_version, sizeof(d->status.hw_version),
		  "MVBC02D %s", d->media_type ? "EMD" : "ESD");
	strscpy(d->status.sw_version, DRV_VERSION_STR,
		sizeof(d->status.sw_version));
	d->status.is_init = 1;
	d->status.has_md = 1;

	pr_info(DRV_NAME ": controller %d initialized (%s)\n",
		d->brd_id, d->status.hw_version);

	return 0;
}

static void mvb_deinit_board(struct mvblli_dev *d)
{
	if (!d->status.is_init)
		return;

	mvb_stop(d);
	sa_w16(d, MVBC_IMR0, 0);
	sa_w16(d, MVBC_IMR1, 0);

	mvblli_detach_board(d);

	kfree(d->rcv_ring);
	d->rcv_ring = NULL;
	kfree(d->all_tacks);
	d->all_tacks = NULL;

	memset(&d->status, 0, sizeof(d->status));

	pr_info(DRV_NAME ": controller %d deinitialized\n", d->brd_id);
}

/* ------------------------------------------------- Portkonfiguration */

/*
 * Vergibt die Dock-Indizes und traegt PIT und PCS ein. Die Reihenfolge
 * ist bindend: Start bei Index 4, Groessenklassen absteigend, ein
 * 32-Byte-Port belegt vier Docks, ein 16-Byte-Port zwei, alles Kleinere
 * eines. Aus dem naechsten freien Index leitet sich STSR ab.
 */
static int mvb_conf_ports(struct mvblli_dev *d, PixyMvblliConfigLpPrt *list,
			  u16 count)
{
	static const u16 klass[] = { 32, 16, 8, 4, 2 };
	unsigned int k, i;
	u16 idx = 4, interval;
	u16 shift = 0;

	for (k = 0; k < ARRAY_SIZE(klass); k++) {
		u16 step = (klass[k] == 32) ? 4 : (klass[k] == 16) ? 2 : 1;

		for (i = 0; i < count; i++) {
			u32 pcs;
			int fc;

			if (list[i].size != klass[k])
				continue;
			if (list[i].prt_addr >= TM_PORT_COUNT)
				return -EINVAL;

			fc = lp_len_2_fcode(list[i].size);
			if (fc < 0)
				return -EINVAL;
			if (idx > d->prt_indx_max)
				return -ENOSPC;

			/* Port Index Table */
			if (d->pit_type)
				tm_w16(d, d->off_la_pit + list[i].prt_addr * 2,
				       idx);
			else {
				u32 off = d->off_la_pit +
					  (list[i].prt_addr & ~1u);
				u16 w = tm_r16(d, off);

				if (list[i].prt_addr & 1)
					w = (w & 0x00ff) | (idx << 8);
				else
					w = (w & 0xff00) | (idx & 0xff);
				tm_w16(d, off, w);
			}

			/* Port Control and Status */
			pcs = lp_pcs_off(d, idx);
			tm_w16(d, pcs,
			       ((u16)fc << TM_PCS_FCODE_OFF) |
			       ((list[i].type & 3) << TM_PCS_TYPE_OFF));
			tm_w16(d, pcs + 2, 0);
			tm_w16(d, pcs + 4, 0);
			tm_w16(d, pcs + 6, 0);

			/* beide Seiten des Docks leeren */
			tm_memset16(d, lp_data_off(d, idx, 0), 0,
				    list[i].size / 2);
			tm_memset16(d, lp_data_off(d, idx, 1), 0,
				    list[i].size / 2);

			idx += step;
		}
	}

	if (count >> 7) {
		u16 t = count >> 7;

		while (t) {
			shift++;
			t >>= 1;
		}
	}
	d->tmo_shift = shift;
	interval = (u16)((shift + 1) * 0x1000);

	if (mvb_tmo_config(d, (idx & 0xfff) | interval))
		return -EIO;

	memset(d->all_tacks, 0, TM_PORT_COUNT * sizeof(u16));
	d->status.has_pd = 1;

	return 0;
}

static int mvblli_do_pd_conf(struct mvblli_dev *d, void __user *uarg)
{
	PixyMvblliConfigLpPrt *list;
	u16 count = 0;
	int ret;

	if (get_user(count, (u16 __user *)uarg))
		return -EFAULT;
	if (!count || count > TM_PORT_COUNT)
		return -EINVAL;

	list = kcalloc(count, sizeof(*list), GFP_KERNEL);
	if (!list)
		return -ENOMEM;

	if (copy_from_user(list, (u8 __user *)uarg + sizeof(u16),
			   (size_t)count * sizeof(*list))) {
		kfree(list);
		return -ENOMEM;
	}

	ret = mvb_conf_ports(d, list, count);
	kfree(list);

	return ret ? -EIO : 0;
}

/* ------------------------------------------------------------- fops */

static int pixy_mvblli_open(struct inode *inode, struct file *filp)
{
	struct mvblli_dev *d = NULL;
	int i, ret;

	for (i = 0; i < MVBLLI_MAX_DEV; i++) {
		if (drvdata.dev[i].enable == 1 &&
		    MINOR(drvdata.dev[i].devt) == iminor(inode)) {
			d = &drvdata.dev[i];
			break;
		}
	}
	if (!d)
		return -ENODEV;

	/* exklusiver Zugriff - ein zweites open() bekommt EBUSY */
	if (test_and_set_bit(MVBLLI_FLAG_BUSY, &d->dev_flags)) {
		pr_info(DRV_NAME ": mvblli%d already in use\n", d->brd_id);
		return -EBUSY;
	}

	if (!d->status.is_init) {
		ret = mvb_init_board(d);
		if (ret) {
			clear_bit(MVBLLI_FLAG_BUSY, &d->dev_flags);
			return -ENODEV;
		}
	}

	d->users++;
	filp->private_data = d;

	return 0;
}

static int pixy_mvblli_release(struct inode *inode, struct file *filp)
{
	struct mvblli_dev *d = filp->private_data;

	if (!d)
		return -ENODEV;
	if (!d->status.is_init) {
		pr_warn(DRV_NAME ": mvblli%d not initialized\n", d->brd_id);
		return -ENODEV;
	}

	if (--d->users == 0)
		mvb_deinit_board(d);

	clear_bit(MVBLLI_FLAG_BUSY, &d->dev_flags);

	return 0;
}

static ssize_t pixy_mvblli_read(struct file *filp, char __user *data,
				size_t size, loff_t *offset)
{
	struct mvblli_dev *d = filp->private_data;
	mvb_port kp;
	u16 tack = 0;
	int ret;

	if (!d || !d->status.is_init)
		return -EINVAL;
	if (!d->status.has_pd)
		return -ENOTCONN;
	if (!data || copy_from_user(&kp, data, sizeof(kp)))
		return -EINVAL;

	if (kp.type == IOCTL_PIXY_MVBLLI_PD) {
		if (size > MVB_MAX_PORT_SIZE || kp.port >= TM_PORT_COUNT)
			return -EINVAL;

		ret = apd_get_port(d, kp.port, &tack, kp.data, (u16)size);
		if (ret)
			return -EIO;

		kp.freshness = (u16)((0xffffu - tack) << d->tmo_shift);

		/* Sink-Time-Ueberwachung: zu alte Daten werden abgelehnt */
		if (d->all_tacks && tack < d->all_tacks[kp.port])
			return -ETIMEDOUT;

		if (copy_to_user(data, &kp, sizeof(kp)))
			return -EINVAL;

		return size;
	}

	if (kp.type == IOCTL_PIXY_MVBLLI_MD_HIGH ||
	    kp.type == IOCTL_PIXY_MVBLLI_MD_LOW) {
		unsigned long flags;
		u8 *slot;

		if (kp.port >= 0x100)
			return -EINVAL;

		if (mvb_irq <= 0)
			mvb_md_dispatcher(d);

		spin_lock_irqsave(&d->md_lock, flags);
		if (!d->rcv_filled) {
			spin_unlock_irqrestore(&d->md_lock, flags);
			return -ENOBUFS;
		}
		slot = d->rcv_ring + (size_t)d->rcv_rd * MVB_MSG_FRAME_SIZE;
		spin_unlock_irqrestore(&d->md_lock, flags);

		/* Das Frame landet in mvb_port.data, also 4 Byte hinter dem Anfang */
		if (copy_to_user(data + offsetof(mvb_port, data), slot,
				 MVB_MSG_FRAME_SIZE))
			return -ENOBUFS;

		spin_lock_irqsave(&d->md_lock, flags);
		d->rcv_rd = (d->rcv_rd + 1) % d->rcv_elems;
		d->rcv_filled--;
		spin_unlock_irqrestore(&d->md_lock, flags);

		return MVB_MSG_FRAME_SIZE;
	}

	return -EINVAL;
}

static ssize_t pixy_mvblli_write(struct file *filp, const char __user *data,
				 size_t size, loff_t *offset)
{
	struct mvblli_dev *d = filp->private_data;
	mvb_port kp;
	int control;

	if (!d)
		return -ENODEV;
	if (!data || copy_from_user(&kp, data, sizeof(kp)))
		return -EINVAL;

	switch (kp.type) {
	case IOCTL_PIXY_MVBLLI_PD:
		if (kp.port >= TM_PORT_COUNT || size > MVB_MAX_PORT_SIZE)
			return -EINVAL;
		if (apd_put_port(d, kp.port, kp.data, (u16)size))
			return -EIO;
		return size;

	case IOCTL_PIXY_MVBLLI_MD_HIGH:
		control = 1;
		break;
	case IOCTL_PIXY_MVBLLI_MD_LOW:
		control = 0;
		break;
	default:
		return -EINVAL;
	}

	if (kp.port >= 0x100)
		return -EINVAL;
	if (mvb_sndp(d, kp.port, control, kp.data))
		return -ENOBUFS;

	return MVB_MSG_FRAME_SIZE;
}

static __poll_t pixy_mvblli_poll(struct file *filp, poll_table *wait)
{
	struct mvblli_dev *d = filp->private_data;
	__poll_t mask = EPOLLOUT | EPOLLWRNORM;
	unsigned long flags;

	if (!d || !d->status.is_init)
		return EPOLLERR;

	poll_wait(filp, &d->wait_poll, wait);

	if (mvb_irq <= 0)
		mvb_md_dispatcher(d);

	spin_lock_irqsave(&d->md_lock, flags);
	if (d->rcv_filled)
		mask |= EPOLLIN | EPOLLRDNORM;
	spin_unlock_irqrestore(&d->md_lock, flags);

	return mask;
}

static long pixy_mvblli_ioctl(struct file *filp, unsigned int cmd,
			      unsigned long arg)
{
	struct mvblli_dev *d = filp->private_data;
	void __user *uarg = (void __user *)arg;
	long ret = 0;
	u16 v16;
	u32 v32;

	if (!d || !d->enable || !d->status.is_init)
		return -ENODEV;
	if (_IOC_TYPE(cmd) != PIXY_PIXY_MVBLLI_IOCTL_MAGIC ||
	    _IOC_NR(cmd) > PIXY_PIXY_MVBLLI_MAX_IOCTL_NR)
		return -ENOTTY;

	switch (cmd) {
	case IOCTL_PIXY_MVBLLI_READ_DEV_ADDR:
		v16 = mvb_get_device_address(d);
		if (put_user(v16, (u16 __user *)uarg))
			ret = -EFAULT;
		break;

	case IOCTL_PIXY_MVBLLI_WRITE_DEV_ADDR:
		if (get_user(v16, (u16 __user *)uarg))
			ret = -EFAULT;
		else if (v16 > 0xfff)
			ret = -EINVAL;
		else
			ret = mvb_set_device_address(d, v16);
		break;

	case IOCTL_PIXY_MVBLLI_READ_DSW:
		v16 = sa_r16(d, SA_PP_DATA + TM_PP_FC15 * 64);
		if (put_user(v16, (u16 __user *)uarg))
			ret = -EFAULT;
		break;

	case IOCTL_PIXY_MVBLLI_WRITE_DSW:
		if (get_user(v32, (u32 __user *)uarg)) {
			ret = -EFAULT;
		} else {
			u32 pcs = SA_PP_PCS + TM_PP_FC15 * 8;
			u32 dat = SA_PP_DATA + TM_PP_FC15 * 64;
			u16 w1 = sa_r16(d, pcs + 2);

			sa_w16(d, dat + ((w1 & TM_PCS_VP_MSK) ? 0 : 32),
			       (u16)v32);
			sa_w16(d, pcs + 2, w1 ^ TM_PCS_VP_MSK);
		}
		break;

	case IOCTL_PIXY_MVBLLI_START:
		if (d->status.is_active)
			ret = -EALREADY;
		else if (!d->status.has_pd)
			ret = -EPERM;
		else
			ret = mvb_go(d) ? -EIO : 0;
		break;

	case IOCTL_PIXY_MVBLLI_STOP:
		if (!d->status.is_active)
			ret = -EPERM;
		else
			ret = mvb_stop(d) ? -EIO : 0;
		break;

	case IOCTL_PIXY_MVBLLI_RETRIGGER:
		/* Watchdog: nicht bestueckt, aber ABI-seitig vorhanden */
		break;

	case IOCTL_PIXY_MVBLLI_READ_STATS: {
		mvb_stat st = d->status;

		st.frames   = sa_r16(d, MVBC_FC);
		st.errors   = sa_r16(d, MVBC_EC);
		st.errors_a = 0;
		st.errors_b = 0;
		st.mvb_addr = mvb_get_device_address(d);
		st.t_ignore = (sa_r16(d, MVBC_SCR) & TM_SCR_TMO_MASK) >> 10;
		st.line_config = d->media_type ? 2 : 1;
		if (copy_to_user(uarg, &st, sizeof(st)))
			ret = -EFAULT;
		break;
	}

	case IOCTL_PIXY_MVBLLI_PD_CONF:
		if (d->status.is_active)
			ret = -EALREADY;
		else
			ret = mvblli_do_pd_conf(d, uarg);
		break;

	case IOCTL_PIXY_MVBLLI_MD_CONF: {
		PixyMvblliConfigMex mex;

		if (copy_from_user(&mex, uarg, sizeof(mex)))
			ret = -EFAULT;
		else
			d->q_tq_priority = mex.q_tq_priority;
		break;
	}

	case IOCTL_PIXY_MVBLLI_HWINIT: {
		PixyMvblliConfigLpTs ts;

		if (copy_from_user(&ts, uarg, sizeof(ts))) {
			ret = -EFAULT;
			break;
		}
		d->ownership = ts.ownership;
		d->ts_type = ts.ts_type;
		d->auto_reset_rld = ts.auto_reset_rld;
		if (ts.prt_addr_max)
			d->prt_addr_max = ts.prt_addr_max;
		if (ts.prt_indx_max)
			d->prt_indx_max = ts.prt_indx_max;
		break;
	}

	case IOCTL_PIXY_MVBLLI_DISABLE_PORT:
		if (get_user(v16, (u16 __user *)uarg)) {
			ret = -EFAULT;
		} else if (v16 >= TM_PORT_COUNT) {
			ret = -EINVAL;
		} else {
			u16 idx = lp_port_index(d, v16);

			if (idx)
				tm_w16(d, lp_pcs_off(d, idx), 0);
			if (d->pit_type)
				tm_w16(d, d->off_la_pit + v16 * 2, 0);
		}
		break;

	case IOCTL_PIXY_MVBLLI_READ_TM: {
		mvb_tm tm;

		tm.ts_id = d->ts_id;
		tm.address = (u16 *)d->p_tm;
		tm.size_id = d->mcm;
		if (copy_to_user(uarg, &tm, sizeof(tm)))
			ret = -EFAULT;
		break;
	}

	case IOCTL_PIXY_MVBLLI_MD_FLUSH_QUEUE:
		mvb_md_flush_send_queue(d);
		break;

	case IOCTL_PIXY_MVBLLI_MD_GET_STATUS:
		v32 = mvb_md_get_status(d, 0xffff, 0);
		if (put_user(v32, (u32 __user *)uarg))
			ret = -EFAULT;
		break;

	case IOCTL_PIXY_MVBLLI_WRITE_CONTROL: {
		mvb_ctrl ctrl;

		if (copy_from_user(&ctrl, uarg, sizeof(ctrl))) {
			ret = -EFAULT;
			break;
		}
		if (ctrl.dev_addr <= 0xfff)
			mvb_set_device_address(d, ctrl.dev_addr);
		if (ctrl.t_ignore < 4) {
			u16 scr = sa_r16(d, MVBC_SCR);

			sa_w16(d, MVBC_SCR,
			       (scr & ~TM_SCR_TMO_MASK) |
			       ((ctrl.t_ignore & 3) << 10));
		}
		break;
	}

	case IOCTL_PIXY_MVBLLI_USERS:
		v32 = d->users;
		if (put_user(v32, (u32 __user *)uarg))
			ret = -EFAULT;
		break;

	/*
	 * Die NSDB-Wege sind im Original vorhanden, werden von diesem
	 * Geraet aber nachweislich nicht benutzt (STSR und dti belegen
	 * den PD_CONF-Pfad). Sie melden sich deutlich ab, statt still
	 * etwas Falsches zu tun.
	 */
	case IOCTL_PIXY_MVBLLI_PD_NSDB:
	case IOCTL_PIXY_MVBLLI_MD_NSDB:
	case IOCTL_PIXY_MVBLLI_BA_NSDB:
		pr_warn_once(DRV_NAME
			     ": NSDB configuration is not implemented\n");
		ret = -ENOSYS;
		break;

	case IOCTL_PIXY_MVBLLI_REC_CONF:
	case IOCTL_PIXY_MVBLLI_REC_DEL:
		ret = -ENOSYS;
		break;

	default:
		ret = -ENOTTY;
		break;
	}

	return ret;
}

static const struct file_operations mvblli_fops = {
	.owner		= THIS_MODULE,
	.open		= pixy_mvblli_open,
	.release	= pixy_mvblli_release,
	.read		= pixy_mvblli_read,
	.write		= pixy_mvblli_write,
	.poll		= pixy_mvblli_poll,
	.unlocked_ioctl	= pixy_mvblli_ioctl,
	.compat_ioctl	= pixy_mvblli_ioctl,
	.llseek		= no_llseek,
};

/* ------------------------------------- An-/Abmeldung beim Board-Treiber */

static void mvb_add_board(int brd_id, void *arg)
{
	struct mvblli_dev *d;
	int ret;

	if ((unsigned int)brd_id >= MVBLLI_MAX_DEV) {
		pr_err(DRV_NAME ": invalid board id\n");
		return;
	}

	d = &drvdata.dev[brd_id];
	if (d->enable == 1) {
		pr_warn(DRV_NAME ": board %d already registered\n", brd_id);
		return;
	}

	memset(d, 0, sizeof(*d));
	d->brd_id = brd_id;
	spin_lock_init(&d->md_lock);
	mutex_init(&d->lock);
	init_waitqueue_head(&d->wait_poll);

	d->devt = MKDEV(MAJOR(drvdata.devt_base),
			MINOR(drvdata.devt_base) + drvdata.total_devices);

	cdev_init(&d->cdev, &mvblli_fops);
	d->cdev.owner = THIS_MODULE;

	ret = cdev_add(&d->cdev, d->devt, 1);
	if (ret < 0) {
		pr_err(DRV_NAME ": cannot add char device\n");
		return;
	}

	d->sysdev = device_create(drvdata.cls, NULL, d->devt, NULL,
				  "mvblli%d", brd_id);
	if (IS_ERR(d->sysdev)) {
		d->sysdev = NULL;
		cdev_del(&d->cdev);
		pr_err(DRV_NAME ": cannot create /dev entry\n");
		return;
	}

	d->enable = 1;
	drvdata.total_devices++;
	pr_info(DRV_NAME ": /dev/mvblli%d attached to board %d\n",
		brd_id, brd_id);
}

static void mvb_remove_board(int brd_id, void *arg)
{
	struct mvblli_dev *d;

	if ((unsigned int)brd_id >= MVBLLI_MAX_DEV)
		return;

	d = &drvdata.dev[brd_id];
	if (d->enable != 1)
		return;

	if (d->status.is_init)
		mvb_deinit_board(d);

	if (d->sysdev) {
		device_destroy(drvdata.cls, d->devt);
		d->sysdev = NULL;
	}
	cdev_del(&d->cdev);

	d->enable = 0;
	if (drvdata.total_devices > 0)
		drvdata.total_devices--;

	pr_info(DRV_NAME ": /dev/mvblli%d removed\n", brd_id);
}

static int mvblli_subscribe(bool subscribe)
{
	struct PixyMvbBoardUpperDrvSubscribe sub;
	struct file *filp;
	int ret;

	filp = filp_open("/dev/mvb0", O_RDONLY, 0);
	if (IS_ERR(filp)) {
		pr_err(DRV_NAME ": /dev/mvb0 not available - is pixy-mvb loaded?\n");
		return -ENODEV;
	}

	if (subscribe) {
		memset(&sub, 0, sizeof(sub));
		sub.add_brd.brd_evnt = mvb_add_board;
		sub.add_brd.arg = NULL;
		sub.rm_brd.brd_evnt = mvb_remove_board;
		sub.rm_brd.arg = NULL;
		ret = board_ioctl(filp,
				  IOCTL_PIXY_MVB_BOARD_KSET_DRV_SUBSCRIBE,
				  &sub);
		if (!ret)
			drvdata.drv_id = sub.drv_id;
	} else {
		int id = drvdata.drv_id;

		ret = board_ioctl(filp,
				  IOCTL_PIXY_MVB_BOARD_KSET_DRV_UNSUBSCRIBE,
				  &id);
	}

	filp_close(filp, NULL);

	return ret;
}

/* ------------------------------------------------------------ module */

static int __init pixy_mvblli_init(void)
{
	int ret;

	memset(&drvdata, 0, sizeof(drvdata));
	pr_info("%s\n", DRV_VERSION_STR);

	ret = alloc_chrdev_region(&drvdata.devt_base, 0, MVBLLI_MINOR_COUNT,
				  DRV_NAME);
	if (ret < 0) {
		pr_err(DRV_NAME ": cannot allocate char device region\n");
		return ret;
	}

	drvdata.cls = class_create(THIS_MODULE, DRV_NAME);
	if (IS_ERR(drvdata.cls)) {
		ret = PTR_ERR(drvdata.cls);
		goto err_chrdev;
	}

	ret = mvblli_subscribe(true);
	if (ret)
		goto err_class;

	return 0;

err_class:
	class_destroy(drvdata.cls);
err_chrdev:
	unregister_chrdev_region(drvdata.devt_base, MVBLLI_MINOR_COUNT);
	return ret;
}

static void __exit pixy_mvblli_exit(void)
{
	int i;

	mvblli_subscribe(false);

	for (i = 0; i < MVBLLI_MAX_DEV; i++)
		if (drvdata.dev[i].enable)
			mvb_remove_board(i, NULL);

	class_destroy(drvdata.cls);
	unregister_chrdev_region(drvdata.devt_base, MVBLLI_MINOR_COUNT);
	pr_info(DRV_NAME ": unloaded\n");
}

module_init(pixy_mvblli_init);
module_exit(pixy_mvblli_exit);

MODULE_LICENSE("GPL v2");
MODULE_DESCRIPTION("MVB Link Layer Interface for the Pixy-1000 board");
MODULE_VERSION("3.0.0");
