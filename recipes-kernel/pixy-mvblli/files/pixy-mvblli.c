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
#include <linux/workqueue.h>

#include "pixy-mvb.h"
#include "pixy-mvblli.h"
#include "mvbc-regs.h"

#define DRV_NAME		"pixy-mvblli"
#define DRV_VERSION		"1.0.3"
#define DRV_VERSION_STR		"pixy-mvblli v" DRV_VERSION \
	" - Pixy-1000 MVB link layer interface Driver"
/*
 * Kennung im Statusblock (READ_STATUS), wie das Original sie bildet:
 * "%s-V%s-%s" aus Modulname, Version und dem eingebauten Datumsfeld.
 * Die Anwendung bekommt dieselbe Zeichenkette zu sehen wie beim Original.
 */
#define DRV_SW_VERSION		"pixy-mvblli-V" DRV_VERSION "-03.12.24"

#define MVBLLI_MAX_DEV		3
#define MVBLLI_MINOR_COUNT	0x2fd
#define MVBLLI_NSDB_SIZE	0x1000

/* Groesse des Software-Empfangsrings fuer Message-Frames */
#define MVBLLI_RCV_RING_ELEMS	0xde

/*
 * Ereignisaufzeichnung: zwei Plaetze (mvb_rec = 2), je Aufzeichnung
 * hoechstens 0x14 Eintraege (mvb_recbuf_size) - beides fest im Original.
 */
#define MVB_REC_SLOTS		2
#define MVB_REC_BUF_SIZE	0x14

/* Lage der drei Message-Queues im Traffic Memory (Byteoffsets) */
#define MD_TQ0_OFFSET		0x08000
#define MD_TQ0_LLRS		8
#define MD_TQ1_OFFSET		0x08140
#define MD_TQ1_LLRS		0xde
#define MD_RQ_OFFSET		0x0a0a0
#define MD_RQ_LLRS		0xde

/*
 * Wie im Original: Parametername "irq", Vorgabe 7. Der Wert landet als
 * ISA-Interruptnummer im BCR; groesser 0 heisst ausserdem, dass
 * empfangene Messages per Interrupt (DTI1) abgeholt werden und read()
 * bzw. poll() den Empfangsdispatcher nicht selbst aufrufen. Ein
 * "options pixy_mvblli irq=..." fuer das Original muss auch hier laden.
 */
static int mvb_irq = 7;
module_param_named(irq, mvb_irq, int, 0444);
MODULE_PARM_DESC(irq,
	"ISA-Interrupt des MVBC; 0 = Message-Daten werden gepollt (Vorgabe 7)");

/*
 * Diagnosezaehler, lesbar unter /sys/module/pixy_mvblli/parameters/.
 * Sie kosten nichts und beantworten die Frage, ob der Interruptdienst
 * ueberhaupt laeuft und was das Vektorregister meldet.
 */
static int dbg_irq;		/* Aufrufe des Interruptdienstes   */
static int dbg_dti1;		/* dispatchte Quelle DTI1          */
static int dbg_dti2;		/* dispatchte Quelle DTI2          */
static int dbg_fev;		/* dispatchte Quelle FEV           */
static int dbg_rqe;		/* dispatchte Quelle RQE           */
static int dbg_other;		/* gemeldete, aber unbekannte Nr.  */
static int dbg_last_other = -1;	/* zuletzt gesehene unbekannte Nr. */

module_param(dbg_irq, int, 0444);
module_param(dbg_dti1, int, 0444);
module_param(dbg_dti2, int, 0444);
module_param(dbg_fev, int, 0444);
module_param(dbg_rqe, int, 0444);
module_param(dbg_other, int, 0444);
module_param(dbg_last_other, int, 0444);

/*
 * Quittung bis zum leeren Durchgang (Abweichung vom Original, abschaltbar).
 *
 * Beide Interruptausgaenge des MVBC liegen ueber BCR 0x77 auf derselben
 * ISA-Leitung 7, und die Karte macht daraus einen flankengesteuerten
 * MSI. Das Original leert IVR1, dann IVR0, und kehrt zurueck. Meldet
 * IVR1 eine neue Quelle, waehrend IVR0 noch geleert wird, faellt die
 * gemeinsame Leitung nie ab: Es gibt keine neue Flanke und damit keinen
 * Interrupt mehr. Gemessen mit Original und Nachbau: IPR0/IPR1 stehen mit
 * freigegebenen Bits an (DTI1, DTI2, FEV), /proc/interrupts steht still,
 * und der Message-Empfang (DTI1) ist tot.
 *
 * Mit irq_rearm=1 wird so lange wiederholt, bis ein ganzer Durchgang
 * ueber beide Register nichts mehr findet. Erst dann sind beide
 * Ausgaenge unten, und das naechste Ereignis erzeugt wieder eine
 * Flanke. irq_rearm=0 verhaelt sich wie das Original.
 */
static int irq_rearm = 1;
module_param(irq_rearm, int, 0644);
MODULE_PARM_DESC(irq_rearm,
	"IVR1/IVR0 wiederholt leeren, bis beide leer sind (1, Vorgabe) oder wie das Original einmal (0)");

static int dbg_rearm;		/* Durchgaenge, die noch etwas fanden */
module_param(dbg_rearm, int, 0444);

/*
 * Waechter gegen eine verlorene Flanke (Abweichung vom Original).
 *
 * Der MSI der Karte reagiert auf eine Flanke. Faellt eine einzige aus -
 * weil jemand IVR nebenbei gelesen hat, oder weil eine neue Quelle genau
 * zwischen Quittung und Ruecksprung eintrifft -, bleibt die Leitung oben
 * und es kommt nie wieder ein Interrupt. Am Geraet beobachtet: IPR steht
 * mit freigegebenen Bits an, /proc/interrupts steht, der Message-Empfang
 * ist tot, und nur ein Neustart hilft. Das Original hat dagegen keine
 * Vorkehrung.
 *
 * Der Waechter sieht alle irq_watchdog Millisekunden nach, ob etwas
 * Freigegebenes ansteht, ohne dass der Interruptdienst laeuft, und holt
 * es nach. Bleibt IPR danach stehen, wird die Maske kurz aus- und wieder
 * eingeschaltet; das erzeugt eine neue Flanke. irq_watchdog = 0 schaltet
 * ihn ab und stellt das Verhalten des Originals her.
 */
static int irq_watchdog = 200;
module_param(irq_watchdog, int, 0644);
MODULE_PARM_DESC(irq_watchdog,
	"Abstand der Interruptaufsicht in ms, 0 = aus (Vorgabe 200)");

#define MVB_WD_MIN_MS		20
#define MVB_WD_IDLE_MS		1000

static int dbg_watchdog;	/* nachgeholte Quittungen          */
static int dbg_wd_toggle;	/* davon mit erzwungener Flanke    */
module_param(dbg_watchdog, int, 0444);
module_param(dbg_wd_toggle, int, 0444);

#define MVB_IRQ_MAX_PASSES	8

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
	int configured;			/* mvb_config() ist durchgelaufen */
	u16 line_config;		/* MVB_LINE_A / _B / _BOTH */
	u16 treply_config;		/* TMO-Feld im SCR, 0..3    */

	/* Konfiguration des Traffic Store (HWINIT) */
	u8 ownership;
	u8 ts_type;
	u8 auto_reset_rld;
	u16 prt_addr_max;
	u16 prt_indx_max;
	u8 lp_state;			/* 1 = PD_CONF ist einmal durchgelaufen */

	/* Ereignisaufzeichnung (REC_CONF / REC_DEL) */
	u16 rec_port[MVB_REC_SLOTS];	/* 0 = Platz frei */
	u16 rec_used;

	u16 tmo_shift;			/* Schiebeweite fuer mvb_port.freshness */
	u16 *all_tacks;			/* Sink-Time-Schwelle je Portadresse */
	u16 int_mask[2];		/* Spiegel von IMR0 / IMR1 */
	bool irq_attached;		/* Dienst ist beim Board-Treiber eingetragen */
	u32 debug_overflows;		/* nur Statistik, wie im Original */

	mvb_stat status;

	/* Message-Daten */
	u16 q_tq_priority;
	u16 p16_tq0, p16_tq1, p16_rq;	/* Softwareposition im jeweiligen Ring */
	u32 rq_overflow;

	u8 *rcv_ring;			/* MVBLLI_RCV_RING_ELEMS * 32 Byte */
	int rcv_elems, rcv_filled, rcv_rd, rcv_wr;
	spinlock_t md_lock;
	wait_queue_head_t wait_poll;
	/*
	 * Empfangsmeldungen fuer poll(), beide modulo 16 wie im Original:
	 * der DTI1-Pfad zaehlt dispatched hoch, poll() zieht signaled nach
	 * und meldet je Schritt einmal "lesbar".
	 */
	int md_rcv_irq_dispatched;
	int md_rcv_irq_signaled;

	/* Interruptquittung: Dienst und Waechter schliessen sich aus */
	spinlock_t irq_lock;
	struct delayed_work irq_wd;
	bool wd_stop;			/* der Waechter stellt sich nicht neu */

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
		return 9;

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

/*
 * Warten mit dem Zaehler 2 des MVBC, nicht mit der Kernel-Uhr. Das ist
 * Absicht: die Wartezeiten in mvb_config() beziehen sich auf den Takt des
 * Controllers, nicht auf Mikrosekunden. Ein Wert von 1 entspricht acht
 * Zaehlerschritten.
 *
 * Einziger Unterschied zum Original ist die Zaehlschranke - das Original
 * dreht hier ohne Abbruchbedingung. Solange der Zaehler laeuft, aendert
 * sie nichts; laeuft er nicht, haengt der Kernel damit nicht fest.
 */
static void mvb_wait(struct mvblli_dev *d, u16 time)
{
	unsigned int guard = 1000000;
	u16 target;

	if (time >= 0x2000)
		return;

	target = (u16)(0xffff - 8 * time);

	sa_w16(d, MVBC_TR2, 0xffff);
	sa_w16(d, MVBC_TC2, 0xffff);
	sa_w16(d, MVBC_TCR, sa_r16(d, MVBC_TCR) | TM_TCR_TA2);

	while (guard-- && sa_r16(d, MVBC_TC2) > target)
		cpu_relax();

	sa_w16(d, MVBC_TCR, sa_r16(d, MVBC_TCR) & ~TM_TCR_TA2);
}

static int mvb_set_device_address(struct mvblli_dev *d, u16 addr)
{
	if (addr > 0xfff)
		return -EINVAL;

	sa_w16(d, MVBC_DAOR, addr);
	sa_w16(d, MVBC_DAOK, TM_DAOK_ENABLE);

	/* Das Original prueft die Adresse zurueck und meldet Abweichungen. */
	if (sa_r16(d, MVBC_DAOR) != addr)
		return -EIO;

	d->mvb_addr = addr;
	d->status.mvb_addr = addr;

	return 0;
}

static u16 mvb_get_device_address(struct mvblli_dev *d)
{
	return sa_r16(d, MVBC_DAOR);
}

/*
 * Device Status Word. Der Port FC15 ist doppelt gepuffert: geschrieben
 * wird die gerade nicht sichtbare Seite, danach legt VP auf sie um und
 * die andere Seite wird nachgezogen, damit beide denselben Stand tragen.
 * Die doppelte VP-Schreibung ist aus dem Original uebernommen.
 */
static void mvb_set_device_status_word(struct mvblli_dev *d, u16 mask, u16 value)
{
	const u32 pcs1 = SA_PP_PCS + TM_PP_FC15 * 8 + 2;
	unsigned int vp = (sa_r16(d, pcs1) & TM_PCS_VP_MSK) ? 1 : 0;
	u32 dst = SA_PP_DATA + tm_dock_offset(TM_PP_FC15, vp ^ 1);
	u32 oth = SA_PP_DATA + tm_dock_offset(TM_PP_FC15, vp);
	u16 v;

	v = (value & mask) | (sa_r16(d, dst) & ~mask);
	sa_w16(d, dst, v);

	if (vp)
		sa_w16(d, pcs1, sa_r16(d, pcs1) & ~TM_PCS_VP_MSK);
	else
		sa_w16(d, pcs1, sa_r16(d, pcs1) | TM_PCS_VP_MSK);

	sa_w16(d, oth, v);

	if (vp)
		sa_w16(d, pcs1, sa_r16(d, pcs1) & ~TM_PCS_VP_MSK);
	else
		sa_w16(d, pcs1, sa_r16(d, pcs1) | TM_PCS_VP_MSK);
}

/*
 * Spiegelt Leitungszustand und Stoerungsmeldung aus dem Decoder-Register
 * in das Device Status Word. Wie im Original ueber alle konfigurierten
 * Traffic Stores, jeder mit seinem eigenen DR.
 */
static void mvb_set_laa_rld(void)
{
	int i;

	for (i = 0; i < MVBLLI_MAX_DEV; i++) {
		struct mvblli_dev *d = &drvdata.dev[i];
		u16 dr, laa, rld;

		if (!d->configured)
			continue;

		dr  = sa_r16(d, MVBC_DR);
		laa = (dr << 4) & MVB_DSW_LAA;		/* DR Bit 3  -> DSW Bit 7 */

		if (d->auto_reset_rld)
			rld = (dr << 4) & MVB_DSW_RLD;	/* DR Bit 2  -> DSW Bit 6 */
		else
			rld = (dr >> 6) & MVB_DSW_RLD;	/* DR Bit 12 -> DSW Bit 6 */

		mvb_set_device_status_word(d, MVB_DSW_LAA | MVB_DSW_RLD,
					   laa | rld);
	}
}

static void mvb_reset_rlds(struct mvblli_dev *d)
{
	sa_w16(d, MVBC_DR, sa_r16(d, MVBC_DR) & ~TM_DR_RLD_LATCH);
	mvb_set_laa_rld();
}

/*
 * Interruptquellen des MVBC. Die Nummer ist zugleich die Bitstelle:
 * 0..15 stehen in IMR0/ISR0, 16..31 in IMR1/ISR1. Die vier hier
 * angeschlossenen Quellen ergeben genau die am Geraet gemessenen
 * Maskenwerte IMR0 = 0x0003 und IMR1 = 0x0880.
 */
#define MVB_INT_DTI1		0	/* IMR0 Bit 0  - Deadline Timer 1  */
#define MVB_INT_DTI2		1	/* IMR0 Bit 1  - Deadline Timer 2  */
#define MVB_INT_DTI7		6	/* IMR0 Bit 6  - Ereignisaufzeichnung */
#define MVB_INT_FEV		23	/* IMR1 Bit 7  - Zaehlerueberlauf  */
#define MVB_INT_RQE		27	/* IMR1 Bit 11 - Empfangsqueue     */

#define MVB_INT_BIT(nr)		((u16)(1u << ((nr) & 0xf)))

static void mvb_int_connect(struct mvblli_dev *d, unsigned int nr)
{
	u32 reg = (nr < 16) ? MVBC_IMR0 : MVBC_IMR1;
	u16 bit = MVB_INT_BIT(nr);

	d->int_mask[nr >> 4] |= bit;
	sa_w16(d, reg, sa_r16(d, reg) | bit);
}

static void mvb_int_disconnect(struct mvblli_dev *d, unsigned int nr)
{
	u32 reg = (nr < 16) ? MVBC_IMR0 : MVBC_IMR1;
	u16 bit = MVB_INT_BIT(nr);

	d->int_mask[nr >> 4] &= ~bit;
	sa_w16(d, reg, sa_r16(d, reg) & ~bit);
}

/*
 * Die vier Zaehler des MVBC. Lesen und Loeschen laufen im Original ueber
 * dieselbe Funktion mvb_handle_counter(); hier sind es zwei, weil das
 * lesbarer ist und das Verhalten nicht beruehrt.
 */
enum mvb_counter {
	MVB_CNT_FRAMES = 1,
	MVB_CNT_ERRORS,
	MVB_CNT_ERRORS_A,
	MVB_CNT_ERRORS_B,
};

static u16 mvb_read_counter(struct mvblli_dev *d, enum mvb_counter which)
{
	switch (which) {
	case MVB_CNT_FRAMES:	return sa_r16(d, MVBC_FC);
	case MVB_CNT_ERRORS:	return sa_r16(d, MVBC_EC);
	case MVB_CNT_ERRORS_A:	return sa_r16(d, MVBC_ECA);
	case MVB_CNT_ERRORS_B:	return sa_r16(d, MVBC_ECB);
	}

	return 0;
}

static void mvb_clear_counters(struct mvblli_dev *d, bool errors,
			       bool line_a, bool line_b)
{
	if (errors)
		sa_w16(d, MVBC_EC, 0);
	if (line_a)
		sa_w16(d, MVBC_ECA, 0);
	if (line_b)
		sa_w16(d, MVBC_ECB, 0);
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

/*
 * Wie im Original nur Bit 1 des IL-Felds loeschen: aus RUNNING (3) wird
 * CONFIG (1), aus TEST (2) wuerde RESET (0).
 */
static int mvb_stop(struct mvblli_dev *d)
{
	sa_w16(d, MVBC_SCR, sa_r16(d, MVBC_SCR) & ~0x0002);
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
	 * Entscheidend fuer die ganze Folge: solange MCR nicht programmiert
	 * ist, liegt die Service Area an ihrem Grundplatz sa_offs[0], egal
	 * wie gross das Traffic Memory wirklich ist. Erst das Schreiben von
	 * MCR weiter unten schiebt sie an den zur Groesse passenden Platz.
	 * Bis dahin laeuft jeder Registerzugriff ueber diese Sondieradresse.
	 */
	for (i = 0; i <= (u32)d->mcm && i < TM_OFFSET_COUNT; i++)
		tm_w16(d, sa_offs[i] + MVBC_SCR, d->waitstates | 0x04c0);

	d->p_sa = d->p_tm + sa_offs[0];

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

	/* Interruptmasken und Vektorregister leeren (nicht ISR0/ISR1) */
	sa_w16(d, MVBC_IMR0, 0);
	sa_w16(d, MVBC_IMR1, 0);
	sa_w16(d, MVBC_IVR0, 0);
	sa_w16(d, MVBC_IVR1, 0);

	/* Message-Ports vorbelegen */
	sa_w16(d, SA_PP_PCS + TM_PP_MSRC * 8, 0x1802);
	sa_w16(d, SA_PP_PCS + TM_PP_MSRC * 8 + 2, 0);
	sa_w16(d, SA_PP_PCS + TM_PP_MSNK * 8, 0x1402);
	sa_w16(d, SA_PP_PCS + TM_PP_MSNK * 8 + 2, 0);

	/*
	 * Schleifentest: die Marke wird in die Message-Quelle gelegt und
	 * die Message-Senke geleert. Im Testmodus kopiert der MVBC sie
	 * waehrend des Initialisierungslaufs hinueber - kommt sie dort an,
	 * arbeiten Controller und Traffic Memory zusammen.
	 */
	sa_w16(d, SA_PP_DATA + tm_dock_offset(TM_PP_MSRC, 0), 0xa55a);
	sa_w16(d, SA_PP_DATA + tm_dock_offset(TM_PP_MSNK, 1), 0);

	sa_w16(d, MVBC_SCR, d->waitstates | 0x84fe);
	mvb_wait(d, 2000);
	sa_w16(d, SA_MFS, 0x1001);
	sa_w16(d, MVBC_MR, 0x0020);		/* Initialisierungslauf starten */

	for (tries = 0x32; tries > 0; tries--) {
		mvb_wait(d, 100);
		if (!(sa_r16(d, MVBC_MR) & TM_MR_BUSY))
			break;
	}

	marker = sa_r16(d, SA_PP_DATA + tm_dock_offset(TM_PP_MSNK, 1));
	mvb_wait(d, 2000);
	sa_w16(d, MVBC_SCR, scr);

	/* Leitungsart: EMD schaltet zwei Bits im Decoder-Register */
	if (d->media_type) {
		sa_w16(d, MVBC_DR, sa_r16(d, MVBC_DR) | 0x0020);
		sa_w16(d, MVBC_DR, sa_r16(d, MVBC_DR) | 0x4000);
	}

	if (marker != 0xa55a) {
		pr_err(DRV_NAME ": MVBC loopback self test failed (0x%04x)\n",
		       marker);
		return -EIO;
	}

	/*
	 * MCR: nur die unteren Bits beschreiben. 15:11 sind die
	 * Chipversion und read-only. Ab hier liegt die Service Area an
	 * ihrem endgueltigen Platz.
	 */
	sa_w16(d, MVBC_MCR, d->mcm & TM_MCR_MCM_MASK);
	d->p_sa = d->p_tm + sa_offs[d->mcm];

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

	/*
	 * Grundbelegung der physischen Ports. Die Werte sind dem Original
	 * entnommen und am Geraet wiedergefunden (FC15 = 0xF842).
	 */
	sa_w16(d, SA_PP_PCS + TM_PP_FC15 * 8, 0xf842);	/* Device Status  */
	sa_w16(d, SA_PP_PCS + TM_PP_FC15 * 8 + 2, 0);
	sa_w16(d, SA_PP_DATA + tm_dock_offset(TM_PP_FC15, 0), 0x0082);
	sa_w16(d, SA_PP_DATA + tm_dock_offset(TM_PP_FC15, 1), 0x0082);

	sa_w16(d, SA_PP_PCS + TM_PP_EFS * 8, 0x9402);	/* Event Ident.   */

	sa_w16(d, SA_PP_PCS + TM_PP_FC8 * 8, 0x8802);	/* Mastership     */
	sa_w16(d, SA_PP_PCS + TM_PP_FC8 * 8 + 2, 0);
	sa_w16(d, SA_PP_DATA + tm_dock_offset(TM_PP_FC8, 0), 0x0a01);
	sa_w16(d, SA_PP_DATA + tm_dock_offset(TM_PP_FC8, 1), 0x0a01);

	/* entspricht checklist != 0 im Original */
	d->configured = 1;

	return 0;
}

/*
 * Setzt das Antwortfenster (TMO) und den Leitungsbetrieb. Aufgerufen wird
 * es aus mvb_init_board() mit (MVB_LINE_BOTH, 1) - das ergibt TMO_43US und
 * Zweileitungsbetrieb, genau die Bits des laufenden Herstellerstandes
 * (SCR = 0x87C7, DR mit geloeschtem SLM).
 *
 * STSR und TCR werden hier bewusst NICHT geschrieben: STSR kommt aus
 * PD_CONF, TCR fasst das Original an dieser Stelle nicht an.
 */
static int mvb_hardw_config(struct mvblli_dev *d, u16 line_config,
			    u16 treply_config)
{
	int bad = (treply_config > 3);
	u16 want_laa, scr, dr;
	int tries;

	if (treply_config <= 3)
		sa_w16(d, MVBC_SCR,
		       ((treply_config << 10) & TM_SCR_TMO_MASK) |
		       (sa_r16(d, MVBC_SCR) & ~TM_SCR_TMO_MASK));

	/*
	 * Nur der MVBC02D-Zweig ist nachgebaut; mvb_config() laesst keine
	 * andere Chipversion durch.
	 */
	switch (line_config) {
	case MVB_LINE_BOTH:
		sa_w16(d, MVBC_DR, sa_r16(d, MVBC_DR) & ~TM_DR_SLM);
		goto out;
	case MVB_LINE_A:
		want_laa = TM_DR_LAA;
		break;
	case MVB_LINE_B:
		want_laa = 0;
		break;
	default:
		bad = 1;
		goto out;
	}

	/*
	 * Einleitungsbetrieb: im Testmodus so oft umschalten, bis die
	 * gewuenschte Leitung aktiv ist, danach Registerstand wieder
	 * herstellen und SLM setzen.
	 */
	scr = sa_r16(d, MVBC_SCR);
	dr  = sa_r16(d, MVBC_DR);

	sa_w16(d, MVBC_SCR, TM_SCR_IL_TEST);
	for (tries = 10; tries > 0; tries--) {
		sa_w16(d, MVBC_DR, TM_DR_LS);
		sa_w16(d, MVBC_DR, TM_DR_SLM);
		if (want_laa == (sa_r16(d, MVBC_DR) & TM_DR_LAA))
			break;
	}
	sa_w16(d, MVBC_SCR, scr);
	sa_w16(d, MVBC_DR, dr | TM_DR_SLM);

	dr = sa_r16(d, MVBC_DR);
	if (want_laa != (dr & TM_DR_LAA) || !(dr & TM_DR_SLM))
		bad = 1;

out:
	d->line_config = line_config;
	d->treply_config = treply_config;
	d->status.t_ignore = treply_config;
	d->status.line_config = line_config;

	mvb_set_laa_rld();

	return bad ? -EIO : 0;
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

	/*
	 * Jedes LLR hat seinen eigenen Puffer, LLR k den Puffer k - auch der
	 * Waechter. Dessen Datenzeiger bleibt aber 0, Puffer 0 wird also nie
	 * benutzt. Alle Puffer werden geleert. So legt es mvb_md_install_q
	 * im Original an; am Geraet nachgemessen (xmit_q0: LLR 1 -> 0x2010,
	 * nicht 0x2008).
	 */
	for (i = 0; i < llr_count; i++) {
		u32 this_llr = llr_off + (u32)i * 4;
		u32 next_llr = llr_off + (u32)((i + 1) % llr_count) * 4;
		u32 buf = data_off + (u32)i * MVB_MSG_FRAME_SIZE;

		tm_w16(d, this_llr, i ? off_to_p16(buf) : 0);
		tm_memset16(d, buf, 0, MVB_MSG_FRAME_SIZE / 2);
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

	/*
	 * Bit 5 der Message-Senke meldet den Eingang per Interrupt (DTI1).
	 * Das Original setzt es nur im Interruptbetrieb; mit der Vorgabe
	 * irq = 7 ergibt das die gemessenen 0xC424.
	 */
	msnk = 0xc404;
	if (mvb_irq > 0)
		msnk |= 0x20;
	sa_w16(d, SA_PP_PCS + TM_PP_MSNK * 8, msnk);

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
		 * Das Original verhaelt sich genauso - kein Fehler, kein Log,
		 * und auch kein Statusbit: rq_overflow wird dort nirgends
		 * gesetzt, nur geloescht.
		 */
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
 * Holt EIN vom Controller abgelegtes Frame aus dem Empfangsring des
 * Traffic Memory - wie mvb_md_dispatcher im Original, das keine
 * Schleife hat. Im Pollbetrieb holt also jedes read() hoechstens ein
 * Frame ab, im Interruptbetrieb jede DTI1-Meldung eines. Was nicht
 * abgeholt ist, bleibt im Ring des Controllers. Der Waechter wandert
 * dabei mit.
 */
static void mvb_md_dispatcher(struct mvblli_dev *d)
{
	u32 cur = p16_to_off(d->p16_rq);
	u16 nxt_p16 = tm_r16(d, cur + 2);
	u32 nxt;
	u16 buf_p16;

	if (sa_r16(d, SA_QDT + 4) == nxt_p16)
		return;				/* nichts abzuholen */

	nxt = p16_to_off(nxt_p16);
	buf_p16 = tm_r16(d, nxt);

	/* Link_Header pruefen: nur TCN-RTP-Frames (PT = 1000b) */
	if ((tm_r16(d, p16_to_off(buf_p16) + 2) & 0xf0) == 0x80)
		mvb_md_store(d, p16_to_off(buf_p16));

	tm_w16(d, nxt, 0);			/* Waechter rueckt weiter */
	tm_w16(d, cur, buf_p16);		/* Puffer ans vorherige LLR */
	d->p16_rq = nxt_p16;
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

	/*
	 * Ereignisframe ausloesen. Welcher der beiden Quellports benutzt
	 * wird, entscheidet die per MD_CONF gesetzte Queue-Prioritaet, nicht
	 * die Prioritaet des einzelnen Frames. Geschrieben wird in die
	 * gerade nicht sichtbare Seite, danach schaltet VP um.
	 */
	{
		bool use_ef1 = (s16)d->q_tq_priority < 0;
		u16 ev = (own & 0xfff) | 0xc000;
		u16 pp = use_ef1 ? TM_PP_EF1 : TM_PP_EF0;
		u32 pcs = SA_PP_PCS + pp * 8;
		u16 w1 = sa_r16(d, pcs + 2);

		if (!(w1 & TM_PCS_VP_MSK)) {
			sa_w16(d, SA_PP_DATA + tm_dock_offset(pp, 1), ev);
			sa_w16(d, pcs + 2, w1 | TM_PCS_VP_MSK);
		} else {
			sa_w16(d, SA_PP_DATA + tm_dock_offset(pp, 0), ev);
			sa_w16(d, pcs + 2, w1 & ~TM_PCS_VP_MSK);
		}
		sa_w16(d, MVBC_MR, use_ef1 ? 0x2000 : 0x1000);
	}

	return 0;
}

/*
 * lm_m_v_send_queue_flush im Original: dem Controller den Sendeabbruch
 * melden (MR = 0x0800) und beide Sendeeintraege der QDT austragen. Ringe
 * und Softwarepositionen bleiben stehen; beim naechsten Senden traegt
 * mvb_sndp die QDT von der aktuellen Position aus neu ein, alles
 * dazwischen ist verworfen.
 */
static void mvb_md_flush_send_queue(struct mvblli_dev *d)
{
	sa_w16(d, MVBC_MR, 0x0800);
	sa_w16(d, SA_QDT + 0, 0);
	sa_w16(d, SA_QDT + 2, 0);
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

/*
 * FEV meldet, dass die 16-Bit-Zaehler des MVBC ueberzulaufen drohen. Das
 * Original traegt sie dann in die 32-Bit-Zaehler des Statusblocks nach
 * und leert die Hardware. Laeuft dabei auch der Softwarezaehler ueber,
 * werden alle vier zurueckgesetzt.
 */
static void mvb_fev_handler(struct mvblli_dev *d)
{
	u32 old = d->status.frames;

	d->status.frames   += mvb_read_counter(d, MVB_CNT_FRAMES);
	d->status.errors   += mvb_read_counter(d, MVB_CNT_ERRORS);
	d->status.errors_a += mvb_read_counter(d, MVB_CNT_ERRORS_A);
	d->status.errors_b += mvb_read_counter(d, MVB_CNT_ERRORS_B);

	sa_w16(d, MVBC_FC, 0);
	mvb_clear_counters(d, true, true, true);

	if (d->status.frames < old) {
		d->status.frames = 0;
		d->status.errors = 0;
		d->status.errors_a = 0;
		d->status.errors_b = 0;
	}
}

static void mvb_run_int_handler(struct mvblli_dev *d, unsigned int nr)
{
	switch (nr) {
	/*
	 * mvb_md_wakeup_messenger -> mvb_md_rcv_cb im Original: nur im
	 * Interruptbetrieb abholen, Empfangsmeldung zaehlen, poll() wecken.
	 */
	case MVB_INT_DTI1:
		dbg_dti1++;
		if (mvb_irq == 0)
			break;
		mvb_md_dispatcher(d);
		d->md_rcv_irq_dispatched = (d->md_rcv_irq_dispatched + 1) & 0xf;
		wake_up_interruptible(&d->wait_poll);
		break;
	case MVB_INT_DTI2:
		dbg_dti2++;
		mvb_set_laa_rld();
		break;
	case MVB_INT_FEV:
		dbg_fev++;
		mvb_fev_handler(d);
		break;
	case MVB_INT_RQE:
		dbg_rqe++;
		d->debug_overflows++;
		break;
	/*
	 * Das Original kopiert hier den gemeldeten Port in einen Ringpuffer
	 * je Aufzeichnung. Ausgelesen wird der nirgends - read() kennt nur
	 * Prozess- und Message-Daten. Die Quittung ueber das IVR genuegt.
	 */
	case MVB_INT_DTI7:
		break;
	default:
		dbg_other++;
		dbg_last_other = (int)nr;
		break;
	}
}

/*
 * Quittiert wird ueber das Vektorregister, nicht ueber ISR0/ISR1.
 *
 * Jedes Lesen von IVR holt die naechste anstehende Quelle heraus: Bit 8
 * sagt "gueltig", Bits 7..0 tragen die Nummer. Damit raeumt sich das
 * Register selbst leer, und zwar auch fuer Quellen, die gar nicht
 * freigegeben sind - deshalb steht IPR beim Original auf null. Zum
 * Schluss wird das Register genullt. Genau deshalb darf man IVR auch
 * niemals nebenbei auslesen, solange ein Stack laeuft: jedes Lesen
 * nimmt dem Treiber eine Meldung weg.
 *
 * Die Schranke von 20 Durchlaeufen ist aus dem Original uebernommen.
 */
static int mvb_drain_ivr(struct mvblli_dev *d, u32 ivr_reg, unsigned int base)
{
	u16 ivr = sa_r16(d, ivr_reg);
	int guard = 0;

	if (ivr & 0x100) {
		do {
			mvb_run_int_handler(d, base + (ivr & 0xff));
			ivr = sa_r16(d, ivr_reg);
			guard++;
		} while ((ivr & 0x100) && guard != 0x14);
	}

	sa_w16(d, ivr_reg, 0);

	return guard;
}

/*
 * Reihenfolge wie im Original: erst IVR1, dann IVR0. Mit irq_rearm
 * weiter, bis ein Durchgang ueber beide Register nichts mehr findet.
 * Der Aufrufer haelt irq_lock.
 */
static void mvb_irq_drain_all(struct mvblli_dev *d)
{
	int pass, found;

	for (pass = 0; pass < MVB_IRQ_MAX_PASSES; pass++) {
		found = mvb_drain_ivr(d, MVBC_IVR1, 16);
		found += mvb_drain_ivr(d, MVBC_IVR0, 0);
		if (!found || !irq_rearm)
			return;
		if (pass)
			dbg_rearm++;
	}
}

static void mvblli_irq_server(void *arg)
{
	struct mvblli_dev *d = arg;
	unsigned long flags;

	dbg_irq++;

	if (!d->enable || !d->status.is_init)
		return;

	spin_lock_irqsave(&d->irq_lock, flags);
	mvb_irq_drain_all(d);
	spin_unlock_irqrestore(&d->irq_lock, flags);
}

/* Was ansteht und freigegeben ist. Die Maske kommt aus dem Spiegel. */
static u16 mvb_irq_pending(struct mvblli_dev *d)
{
	return (sa_r16(d, MVBC_IPR0) & d->int_mask[0]) |
	       (sa_r16(d, MVBC_IPR1) & d->int_mask[1]);
}

static void mvb_irq_watchdog_fn(struct work_struct *w)
{
	struct mvblli_dev *d = container_of(w, struct mvblli_dev, irq_wd.work);
	unsigned long flags;
	int ms = irq_watchdog;

	if (ms > 0 && d->enable && d->status.is_init) {
		spin_lock_irqsave(&d->irq_lock, flags);
		if (mvb_irq_pending(d)) {
			dbg_watchdog++;
			mvb_irq_drain_all(d);
			/*
			 * Was ueber IVR nicht abzuholen war, steht noch an.
			 * Maske aus und wieder an laesst die Leitung fallen
			 * und neu steigen - die Flanke, die gefehlt hat.
			 */
			if (mvb_irq_pending(d)) {
				dbg_wd_toggle++;
				sa_w16(d, MVBC_IMR0, 0);
				sa_w16(d, MVBC_IMR1, 0);
				sa_w16(d, MVBC_IMR0, d->int_mask[0]);
				sa_w16(d, MVBC_IMR1, d->int_mask[1]);
			}
		}
		spin_unlock_irqrestore(&d->irq_lock, flags);
	}

	if (ms > 0 && ms < MVB_WD_MIN_MS)
		ms = MVB_WD_MIN_MS;

	/*
	 * Auch abgeschaltet weiterlaufen, nur traege: so wirkt ein
	 * spaeteres Einschalten zur Laufzeit ohne Neuladen. Nur wd_stop
	 * beendet die Kette - sonst koennte sich der Waechter nach dem
	 * Abbruch im Abbau noch einmal selbst einreihen.
	 */
	if (!d->wd_stop)
		schedule_delayed_work(&d->irq_wd,
			msecs_to_jiffies(ms > 0 ? ms : MVB_WD_IDLE_MS));
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
	d->irq_attached = true;

out:
	filp_close(filp, NULL);
	return ret;
}

/*
 * Meldet den Interruptdienst beim Board-Treiber ab.
 *
 * Heikel ist der Zeitpunkt: release() laeuft beim Prozessende aus
 * __fput heraus, und dort hat der Kernel den Dateisystemkontext von
 * current bereits abgeraeumt. filp_open() greift dann ins Leere und
 * nagelt die Maschine fest - kein Abbruch, kein Oops im Log, nur ein
 * haengender Prozess, der auch einen reboot nicht mehr zulaesst.
 *
 * Das Original prueft an genau dieser Stelle ein Feld von current und
 * ueberspringt das Abmelden, wenn es null ist. Welches der beiden es
 * genau prueft, liess sich nicht aufloesen; geprueft werden deshalb
 * beide. Die Richtung stimmt in jedem Fall: im Zweifel nicht abmelden.
 *
 * Bleibt der Dienst dabei eingetragen, holt irq_attached das beim
 * Entladen nach - dort ist der Kontext wieder in Ordnung.
 */
static void mvblli_detach_board(struct mvblli_dev *d)
{
	struct PixyMvbBoardIrqServer srv;
	char path[24];
	struct file *filp;

	if (!d->irq_attached)
		return;

	if (!current->fs || !current->files) {
		pr_info(DRV_NAME
			": deferring irq server removal (task is exiting)\n");
		return;
	}

	snprintf(path, sizeof(path), "/dev/mvb%d", d->brd_id);
	filp = filp_open(path, O_RDONLY, 0);
	if (IS_ERR(filp))
		return;

	srv.func = mvblli_irq_server;
	srv.arg = d;
	srv.irq_vect_id = (d->irq_vnum > 1) ? 1 : 0;
	srv.remove = 1;
	if (!board_ioctl(filp, IOCTL_PIXY_MVB_BOARD_KSET_IRQ_SERVER, &srv))
		d->irq_attached = false;

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

	/*
	 * Ab hier haengt ein Interruptdienst im Board-Treiber, der auf
	 * Speicher dieses Moduls zeigt. Jeder Fehlerausgang muss ihn wieder
	 * abmelden - sonst ruft der Board-Treiber nach einem rmmod in
	 * freigegebenen Code.
	 */
	ret = mvblli_attach_board(d);
	if (ret)
		return ret;

	ret = mvblli_detect_tm(d);
	if (ret)
		goto err_detach;

	d->ts_id = d->brd_id;
	/*
	 * WS-Feld im SCR. Am Geraet gemessen: das Original schreibt
	 * SCR = 0x87C5 und an der Sondieradresse 0x07C0 - beide tragen
	 * 0x0300, also drei Wartezyklen fuer den Zugriff auf das
	 * Traffic Memory. Mit 0 laeuft der Controller zu schnell und
	 * der Fehlerzaehler steigt.
	 */
	d->waitstates = TM_SCR_WS_3;
	d->prt_addr_max = TM_PORT_COUNT - 1;
	d->prt_indx_max = 0xfff;
	d->lp_state = 0;
	memset(d->rec_port, 0, sizeof(d->rec_port));
	d->rec_used = 0;
	d->q_tq_priority = 0;
	d->rq_overflow = 0;

	/*
	 * Der Statusblock wird zuerst geleert; alles Folgende - Geraete-
	 * adresse, Leitungsbetrieb, Antwortfenster - traegt sich dort ein
	 * und darf danach nicht mehr ueberschrieben werden.
	 */
	memset(&d->status, 0, sizeof(d->status));
	scnprintf(d->status.hw_version, sizeof(d->status.hw_version),
		  "MVBC02D %s", d->media_type ? "EMD" : "ESD");
	strscpy(d->status.sw_version, DRV_SW_VERSION,
		sizeof(d->status.sw_version));

	ret = mvb_config(d);
	if (ret)
		goto err_detach;

	ret = mvb_hardw_config(d, MVB_LINE_BOTH, 1);
	if (ret)
		goto err_detach;

	ret = mvb_set_device_address(d, 0);
	if (ret)
		goto err_detach;

	d->all_tacks = kcalloc(TM_PORT_COUNT, sizeof(u16), GFP_KERNEL);
	if (!d->all_tacks) {
		ret = -ENOMEM;
		goto err_detach;
	}

	d->rcv_elems = MVBLLI_RCV_RING_ELEMS;
	d->rcv_ring = kcalloc(d->rcv_elems, MVB_MSG_FRAME_SIZE, GFP_KERNEL);
	if (!d->rcv_ring) {
		ret = -ENOMEM;
		goto err_free;
	}
	d->rcv_filled = d->rcv_rd = d->rcv_wr = 0;
	d->md_rcv_irq_dispatched = 0;
	d->md_rcv_irq_signaled = 0;

	ret = mvb_md_q_init(d);
	if (ret)
		goto err_free;

	/*
	 * Das untere Byte des BCR traegt die Interruptnummer, in beiden
	 * Halbbytes. Wie im Original nur bei irq > 0; sonst bleibt das BCR
	 * unberuehrt.
	 */
	if (mvb_irq > 0)
		iowrite16(((u16)mvb_irq & 0xf) | (((u16)mvb_irq << 4) & 0xf0) |
			  (ioread16(d->pisa + ISA_BCR) & 0xff00),
			  d->pisa + ISA_BCR);

	/*
	 * Erst jetzt die Interruptquellen freigeben - vorher steht der
	 * Empfangsring nicht. FEV und DTI2 immer (mvb_init_board), DTI1
	 * (Message eingegangen) und RQE (Empfangsqueue voll) nur im
	 * Interruptbetrieb (mvb_md_q_init mit use_ints). Mit der Vorgabe
	 * irq = 7 ergibt das IMR0 = 0x0003 und IMR1 = 0x0880, den
	 * gemessenen Stand.
	 */
	/*
	 * Muss vor dem Freigeben der Masken stehen: der Interruptdienst
	 * steigt bei is_init == 0 sofort wieder aus, und zwischen dem
	 * ersten freigegebenen Bit und dieser Zeile koennte schon eine
	 * Meldung eintreffen - die waere dann unquittiert liegengeblieben.
	 * has_pd kommt aus PD_CONF, has_md aus MD_CONF, nicht von hier.
	 */
	d->status.is_init = 1;

	mvb_int_connect(d, MVB_INT_FEV);
	mvb_int_connect(d, MVB_INT_DTI2);
	if (mvb_irq > 0) {
		mvb_int_connect(d, MVB_INT_DTI1);
		mvb_int_connect(d, MVB_INT_RQE);
	}

	d->wd_stop = false;
	schedule_delayed_work(&d->irq_wd,
			      msecs_to_jiffies(irq_watchdog > 0 ?
					       irq_watchdog : MVB_WD_IDLE_MS));

	pr_info("pixy_mvblli: MVB %s controller of board /dev/mvblli%d initialized\n",
		d->media_type ? "EMD" : "ESD", d->brd_id);

	return 0;

err_free:
	kfree(d->rcv_ring);
	d->rcv_ring = NULL;
	kfree(d->all_tacks);
	d->all_tacks = NULL;
err_detach:
	d->configured = 0;
	mvblli_detach_board(d);
	return ret;
}

static void mvb_deinit_board(struct mvblli_dev *d)
{
	if (!d->status.is_init)
		return;

	/* Der Waechter darf nicht in den Abbau hineinlaufen */
	d->wd_stop = true;
	cancel_delayed_work_sync(&d->irq_wd);

	/*
	 * Reihenfolge wie im Original: Interruptnummer aus dem BCR nehmen,
	 * Quellen abmelden, Dienst beim Board-Treiber austragen, dann
	 * mvb_stop() und mvb_deinit(). Letzteres schreibt SCR = 0: der
	 * Controller geht in RESET, und der Registerblock springt an den
	 * Grundplatz TM + 0x3C00 zurueck. Am Geraet gemessen - nach close()
	 * des Originals stehen SCR, MCR, DR und TCR bei TM + 0x3F80. Ohne
	 * das blieb der Controller konfiguriert und hoerend am Bus.
	 */
	iowrite16(ioread16(d->pisa + ISA_BCR) & 0xff00, d->pisa + ISA_BCR);

	sa_w16(d, MVBC_IMR0, 0);
	sa_w16(d, MVBC_IMR1, 0);
	d->int_mask[0] = 0;
	d->int_mask[1] = 0;

	d->configured = 0;
	mvblli_detach_board(d);

	mvb_stop(d);
	sa_w16(d, MVBC_SCR, 0);

	kfree(d->rcv_ring);
	d->rcv_ring = NULL;
	kfree(d->all_tacks);
	d->all_tacks = NULL;

	memset(&d->status, 0, sizeof(d->status));

	pr_info("pixy_mvblli: MVB %s controller of /dev/mvblli%d deinitialized\n",
		d->media_type ? "EMD" : "ESD", d->brd_id);
}

/* ------------------------------------------------- Portkonfiguration */

/*
 * Einen Port eintragen, wie lp_ts_open_port() im Original:
 *
 *  - PIT-Eintrag schon belegt oder Index schon typisiert: Fehler.
 *  - Die PIT wird nur bei 16-Bit-Eintraegen beschrieben (pit_type 1) -
 *    so steht es im Original, fuer 8-Bit-PITs fehlt der Schreibzugriff.
 *  - Im PCS-Wort 0 werden nur F-Code und Typ geaendert, der Rest bleibt
 *    (& 0xf3fd). Als Typ wird nur "Senke" gesetzt: eine Quelle bleibt
 *    passiv, bis die Anwendung sie zum ersten Mal beschreibt
 *    (apd_put_port). Sonst saendete sie nach START Nullen.
 *  - PCS-Woerter 1..3 und die Datenpuffer bleiben unberuehrt.
 *  - Bei ungueltiger Groesse bleibt der F-Code stehen.
 *
 * Rueckgabe 0 oder 5 wie im Original.
 */
static int lp_ts_open_port(struct mvblli_dev *d, u16 port, u16 idx,
			   u16 size, u16 type)
{
	u32 pcs;
	u16 w0;
	int fc;

	if (port > d->prt_addr_max || idx > d->prt_indx_max)
		return 5;
	if (lp_port_index(d, port))
		return 5;

	pcs = lp_pcs_off(d, idx);
	w0 = tm_r16(d, pcs);
	if ((w0 & TM_PCS_TYPE_MSK) || type > 3)
		return 5;

	if (d->pit_type == 1)
		tm_w16(d, d->off_la_pit + port * 2, idx);

	fc = lp_len_2_fcode(size);
	if (fc >= 0)
		w0 = (w0 & ~TM_PCS_FCODE_MSK) | ((u16)fc << TM_PCS_FCODE_OFF);

	tm_w16(d, pcs, (w0 & 0xf3fd) |
	       ((type == TM_PCS_TYPE_SNK) ? (TM_PCS_TYPE_SNK << TM_PCS_TYPE_OFF)
					  : 0));
	return 0;
}

/*
 * PD_CONF in der Reihenfolge des Originals:
 *
 *  1. Dock-Indizes vergeben: Start bei 4, Groessenklassen absteigend,
 *     ein 32-Byte-Port belegt vier Docks, ein 16-Byte-Port zwei, alles
 *     Kleinere eines. Innerhalb einer Klasse zaehlt die Listenfolge.
 *  2. STSR aus dem naechsten freien Index - auch dann, wenn das
 *     Eintragen danach scheitert.
 *  3. Die Ports in Listenfolge eintragen (lp_init_pd).
 */
static int mvb_conf_ports(struct mvblli_dev *d, PixyMvblliConfigLpPrt *list,
			  u16 count)
{
	static const u16 klass[] = { 32, 16, 8, 4, 2 };
	unsigned int k, i;
	u16 idx = 4, interval;
	u16 shift = 0;
	u16 *index;

	index = kcalloc(count ? count : 1, sizeof(*index), GFP_KERNEL);
	if (!index)
		return -ENOMEM;

	for (k = 0; k < ARRAY_SIZE(klass); k++) {
		u16 step = (klass[k] == 32) ? 4 : (klass[k] == 16) ? 2 : 1;

		for (i = 0; i < count; i++) {
			if (list[i].size != klass[k])
				continue;
			index[i] = idx;
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

	if (mvb_tmo_config(d, (idx & 0xfff) | interval)) {
		kfree(index);
		return -EIO;
	}
	memset(d->all_tacks, 0, TM_PORT_COUNT * sizeof(u16));

	for (i = 0; i < count; i++) {
		/* Groesse ausserhalb der fuenf Klassen: kein Index vergeben */
		if (!index[i] ||
		    lp_ts_open_port(d, list[i].prt_addr, index[i],
				    list[i].size, list[i].type)) {
			kfree(index);
			return -EIO;
		}
	}

	kfree(index);
	d->status.has_pd = 1;
	return 0;
}

/*
 * PIT und PCS vollstaendig leeren, so weit, wie HWINIT sie beschrieben
 * hat: (prt_addr_max + 1) Eintraege PIT, (prt_indx_max + 1) PCS.
 */
static void lp_clear_tables(struct mvblli_dev *d)
{
	tm_memset16(d, d->off_la_pit, 0,
		    d->pit_type ? (d->prt_addr_max + 1u)
				: (d->prt_addr_max + 1u) / 2);
	tm_memset16(d, d->off_la_pcs, 0, (d->prt_indx_max + 1u) * 4);
}

/*
 * Das Original laesst die Portliste durch lp_init_pd() laufen. Das leert
 * PIT und PCS vor jeder Konfiguration ausser der ersten nach HWINIT -
 * die hat HWINIT schon geleert - und ebenso nach jedem Fehlschlag, damit
 * keine halbe Konfiguration stehen bleibt. Eine leere Liste ist gueltig
 * und ergibt STSR = 0x1004.
 *
 * Fehlercodes wie im Original: alles vor der Konfiguration selbst
 * (Kopieren, Speicher) ist EINVAL, ein Fehler beim Eintragen EIO.
 */
static int mvblli_do_pd_conf(struct mvblli_dev *d, void __user *uarg)
{
	PixyMvblliConfigLpPrt *list;
	u16 count = 0;
	int ret;

	if (copy_from_user(&count, uarg, sizeof(count)))
		return -EINVAL;

	list = kcalloc(count ? count : 1, sizeof(*list), GFP_KERNEL);
	if (!list)
		return -EINVAL;

	if (copy_from_user(list, (u8 __user *)uarg + sizeof(u16),
			   (size_t)count * sizeof(*list))) {
		kfree(list);
		return -EINVAL;
	}

	if (d->lp_state == 1 && count)
		lp_clear_tables(d);

	ret = mvb_conf_ports(d, list, count);
	kfree(list);

	if (ret) {
		lp_clear_tables(d);
		return -EIO;
	}

	d->lp_state = 1;
	return 0;
}

/*
 * HWINIT, im Original mvb_conf_lpts() mit lp_create() darunter.
 *
 * Von der uebergebenen Struktur zaehlen nur ownership, ts_type und
 * auto_reset_rld. prt_addr_max und prt_indx_max leitet das Original aus
 * der Groesse des Traffic Memory ab und uebergeht die Werte des
 * Aufrufers. ts_type muss 1 sein, sonst EIO - erst nach dem Uebernehmen
 * der Grenzen, genau wie in lp_ts_create().
 */
static int mvb_rec_desubscribe_all(struct mvblli_dev *d);

static int mvb_conf_lpts(struct mvblli_dev *d, void __user *uarg)
{
	PixyMvblliConfigLpTs ts;

	if (copy_from_user(&ts, uarg, sizeof(ts)))
		return -EINVAL;

	d->auto_reset_rld = ts.auto_reset_rld;

	/*
	 * Zaehler auf null, in Hardware wie im Statusblock. Das Original
	 * macht das hier und sonst nur noch im PD_NSDB-Pfad; ohne Anwendung
	 * fasst sie also niemand an. Ein gesaettigter Framezaehler bleibt
	 * dann stehen und FEV meldet sich nie wieder - genau der Zustand,
	 * in dem die Karte beim Geraetetest hing.
	 */
	sa_w16(d, MVBC_FC, 0);
	mvb_clear_counters(d, true, true, true);
	d->status.frames = 0;
	d->status.errors = 0;
	d->status.errors_a = 0;
	d->status.errors_b = 0;

	if (mvb_rec_desubscribe_all(d))
		return -EIO;

	d->prt_addr_max = 0xfff;
	if (d->mcm == 2)
		d->prt_indx_max = 0xff;
	else if (d->mcm == 3 || d->mcm == 4)
		d->prt_indx_max = 0x3ff;
	else
		d->prt_indx_max = 0x3f;
	d->ts_type = ts.ts_type;

	if (ts.ts_type != 1)
		return -EIO;

	d->ownership = ts.ownership;
	if (ts.ownership == 1)
		lp_clear_tables(d);

	mvb_set_device_status_word(d, 0x0002, 0);
	return 0;
}

/*
 * DISABLE_PORT, im Original apd_port_disable(): wirkt nur auf eine
 * Quelle und nimmt ihr dort die Typbits im PCS - der Port bleibt
 * eingetragen, sendet aber nicht mehr. Unbekannter Port (8) und
 * Nicht-Quelle (9) werden vom ioctl zu EIO.
 */
static int apd_port_disable(struct mvblli_dev *d, u16 port)
{
	u16 idx = lp_port_index(d, port);
	u32 pcs;
	u16 w0;

	if (!idx || idx > d->prt_indx_max)
		return 8;

	pcs = lp_pcs_off(d, idx);
	w0 = tm_r16(d, pcs);
	if (((w0 >> TM_PCS_TYPE_OFF) & 3) != 2)
		return 9;

	tm_w16(d, pcs, w0 & 0xf3ff);
	return 0;
}

/* ------------------------------------------------ Ereignisaufzeichnung */

/*
 * REC_CONF meldet einen Senkenport zur Aufzeichnung an: das DTI-Feld
 * seines PCS bekommt den Wert 7, beim ersten Port wird zusaetzlich DTI7
 * freigegeben. Die Daten sammelt das Original in einem Ringpuffer, den
 * aber keine Schnittstelle ausliest (siehe mvb_run_int_handler) - nach
 * aussen sichtbar sind nur PCS, IMR0 und die Rueckgabewerte. Genau die
 * werden hier nachgebildet.
 */
#define MVB_PCS_DTI_REC		0x00e0	/* (6 + 1) << 5 */

static int apd_rec_subscribe(struct mvblli_dev *d, u16 port, bool first)
{
	u16 idx = lp_port_index(d, port);
	u32 pcs;
	u16 w0;

	if (!idx || idx > d->prt_indx_max)
		return 8;

	pcs = lp_pcs_off(d, idx);
	w0 = tm_r16(d, pcs);
	if (((w0 >> TM_PCS_TYPE_OFF) & 3) != 1)
		return 10;

	if (first)
		mvb_int_connect(d, MVB_INT_DTI7);
	tm_w16(d, pcs, w0 | MVB_PCS_DTI_REC);
	return 0;
}

/*
 * Wie im Original ohne Pruefung des Index: steht der Port nicht mehr in
 * der PIT, trifft das Loeschen den PCS-Eintrag 0.
 */
static void apd_rec_desubscribe(struct mvblli_dev *d, u16 port, bool last)
{
	u32 pcs = lp_pcs_off(d, lp_port_index(d, port));

	tm_w16(d, pcs, tm_r16(d, pcs) & 0xff1f);
	if (last)
		mvb_int_disconnect(d, MVB_INT_DTI7);
}

static int mvb_rec_desubscribe_all(struct mvblli_dev *d)
{
	int i;

	for (i = 0; i < MVB_REC_SLOTS; i++) {
		u16 port = d->rec_port[i];

		if (!port)
			continue;
		d->rec_used--;
		d->rec_port[i] = 0;
		apd_rec_desubscribe(d, port, d->rec_used == 0);
	}

	return 0;
}

static long mvb_conf_rec_event(struct mvblli_dev *d, void __user *uarg)
{
	mvb_rec_event ev;
	int i;

	if (copy_from_user(&ev, uarg, sizeof(ev)))
		return -EINVAL;

	if (d->rec_used >= MVB_REC_SLOTS)
		return -ENOSPC;
	if ((u16)(ev.ts_port - 1) > 0xffe || !ev.buf_len ||
	    ev.buf_len > MVB_REC_BUF_SIZE)
		return -EINVAL;

	/* erster freier Platz; ein Doppel davor ist EBUSY */
	for (i = 0; i < MVB_REC_SLOTS; i++) {
		if (d->rec_port[i] == ev.ts_port)
			return -EBUSY;
		if (d->rec_port[i])
			continue;

		if (apd_rec_subscribe(d, ev.ts_port, d->rec_used == 0))
			return -EIO;
		d->rec_used++;
		d->rec_port[i] = ev.ts_port;
		return 0;
	}

	return -ENOSPC;
}

static long mvb_del_rec_event(struct mvblli_dev *d, void __user *uarg)
{
	mvb_rec_event ev;
	int i;

	if (copy_from_user(&ev, uarg, sizeof(ev)))
		return -EINVAL;
	if (!ev.ts_port)
		return -EINVAL;

	for (i = 0; i < MVB_REC_SLOTS; i++) {
		if (d->rec_port[i] != ev.ts_port)
			continue;

		d->rec_used--;
		d->rec_port[i] = 0;
		apd_rec_desubscribe(d, ev.ts_port, d->rec_used == 0);
		return 0;
	}

	return -EINVAL;
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
		return -EBADF;

	/* exklusiver Zugriff - ein zweites open() bekommt EBUSY */
	if (test_and_set_bit(MVBLLI_FLAG_BUSY, &d->dev_flags)) {
		pr_err("pixy_mvblli: Cannot open device /dev/mvblli%d, already in use\n",
		       d->brd_id);
		return -EBUSY;
	}

	if (!d->status.is_init) {
		ret = mvb_init_board(d);
		if (ret) {
			clear_bit(MVBLLI_FLAG_BUSY, &d->dev_flags);
			return -EBADF;
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
	/* Gelesen wird erst, wenn der Controller laeuft (MVB_GO). */
	if (!d->status.is_active)
		return -ENETDOWN;
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
		return -ENETDOWN;
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

/*
 * poll() wie im Original: ohne Interruptbetrieb nie lesbar. Im
 * Interruptbetrieb genau einmal "lesbar" je Empfangsmeldung aus dem
 * DTI1-Pfad - nicht etwa, solange der Softwarering Frames enthaelt.
 * "Schreibbar" meldet das Original nie. Geprueft wird hier, wie dort,
 * irq != 0 (nicht > 0).
 */
static __poll_t pixy_mvblli_poll(struct file *filp, poll_table *wait)
{
	struct mvblli_dev *d = filp->private_data;

	if (!d)
		return EPOLLERR;
	if (mvb_irq == 0)
		return 0;

	if (d->md_rcv_irq_dispatched == d->md_rcv_irq_signaled) {
		poll_wait(filp, &d->wait_poll, wait);
		return 0;
	}

	d->md_rcv_irq_signaled = (d->md_rcv_irq_signaled + 1) & 0xf;
	return EPOLLIN | EPOLLRDNORM;
}

/*
 * access_ok() mit der Groesse, die das Original an der jeweiligen Stelle
 * prueft. Auch bei den drei Wert-ioctls (WRITE_DEV_ADDR, WRITE_DSW,
 * DISABLE_PORT) laeuft die Pruefung ueber den Wert selbst - sinnlos, aber
 * so steht es im Original, und fuer kleine Werte ist sie immer erfuellt.
 */
#define MVB_UARG_OK(arg, n)	access_ok((void __user *)(arg), (n))

/*
 * Verteiler, Fall fuer Fall nach dem Disassembly des Originals: dieselbe
 * Reihenfolge der Pruefungen, dieselben Fehlercodes. Der Dekompilierer
 * hat in den abgespaltenen Fallfunktionen die Register falsch zugeordnet
 * und das an drei Stellen verschleiert - massgeblich ist der Maschinencode.
 */
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
		if (!uarg)
			return -EINVAL;
		if (!MVB_UARG_OK(arg, 2))
			return -EFAULT;
		v16 = mvb_get_device_address(d);
		d->mvb_addr = v16;
		if (copy_to_user(uarg, &v16, 2))
			ret = -EINVAL;
		break;

	/*
	 * Die Adresse kommt als Wert, nicht ueber einen Zeiger - obwohl der
	 * Kopf _IOW(..., uint16_t) sagt. Damit ist 0 nicht setzbar: arg == 0
	 * faellt vorher in EINVAL. Gemerkt wird sie vor dem Schreiben, auch
	 * wenn das anschliessend scheitert.
	 */
	case IOCTL_PIXY_MVBLLI_WRITE_DEV_ADDR:
		if (!arg)
			return -EINVAL;
		if (!MVB_UARG_OK(arg, 2) || d->status.is_active)
			return -EFAULT;
		if (arg > 0xfff)
			return -EINVAL;
		d->mvb_addr = (u16)arg;
		if (mvb_set_device_address(d, (u16)arg))
			ret = -EIO;
		break;

	case IOCTL_PIXY_MVBLLI_READ_DSW:
		if (!uarg)
			return -EINVAL;
		if (!MVB_UARG_OK(arg, 2))
			return -EFAULT;
		v16 = sa_r16(d, SA_PP_DATA +
			     tm_dock_offset(TM_PP_FC15,
					    (sa_r16(d, SA_PP_PCS + TM_PP_FC15 * 8 + 2)
					     & TM_PCS_VP_MSK) ? 1 : 0));
		if (copy_to_user(uarg, &v16, 2))
			ret = -EINVAL;
		break;

	/*
	 * Ebenfalls als Wert: oberes Wort Maske, unteres Wort Wert. Nur die
	 * maskierten Bits werden veraendert. Steht RLD im Wert, raeumt das
	 * Original im Zweileitungsbetrieb zuvor die Stoerungsmeldung weg.
	 */
	case IOCTL_PIXY_MVBLLI_WRITE_DSW:
		if (!arg)
			return -EINVAL;
		if (!MVB_UARG_OK(arg, 4))
			return -EFAULT;
		v32 = (u32)arg;
		if ((v32 & MVB_DSW_RLD) && d->line_config == MVB_LINE_BOTH)
			mvb_reset_rlds(d);
		mvb_set_device_status_word(d, (u16)(v32 >> 16), (u16)v32);
		break;

	/*
	 * START verlangt eine gesetzte Geraeteadresse 1..0x1000, nicht etwa
	 * eine fertige Portkonfiguration - das ist die Bedingung des
	 * Originals.
	 */
	case IOCTL_PIXY_MVBLLI_START:
		if (d->status.is_active)
			return -EALREADY;
		if ((u16)(d->mvb_addr - 1) >= 0x1000)
			return -EINVAL;
		ret = mvb_go(d) ? -EIO : 0;
		break;

	case IOCTL_PIXY_MVBLLI_STOP:
		if (!d->status.is_active)
			return -ENETDOWN;
		ret = mvb_stop(d) ? -EIO : 0;
		break;

	/*
	 * Der Watchdog gehoert zum MVBC1S. Auf dem MVBC02D dieser Karte
	 * lehnt das Original den Aufruf ab, noch vor jeder Pruefung des
	 * Arguments.
	 */
	case IOCTL_PIXY_MVBLLI_RETRIGGER:
		ret = -EINVAL;
		break;

	case IOCTL_PIXY_MVBLLI_READ_STATS: {
		mvb_stat st;

		if (!uarg)
			return -EINVAL;
		if (!MVB_UARG_OK(arg, sizeof(st)))
			return -EFAULT;

		/*
		 * Die Zaehler im Statusblock tragen die Summe aller bereits
		 * abgeschlossenen Ueberlaufperioden (mvb_fev_handler); dazu
		 * kommt der laufende Stand der Hardware. Der gespeicherte
		 * Wert bleibt dabei unberuehrt, sonst wuerde zyklisches
		 * Abfragen doppelt zaehlen.
		 */
		st = d->status;
		st.mvb_addr  = d->mvb_addr;
		st.frames   += mvb_read_counter(d, MVB_CNT_FRAMES);
		st.errors   += mvb_read_counter(d, MVB_CNT_ERRORS);
		st.errors_a += mvb_read_counter(d, MVB_CNT_ERRORS_A);
		st.errors_b += mvb_read_counter(d, MVB_CNT_ERRORS_B);
		if (copy_to_user(uarg, &st, sizeof(st)))
			ret = -EINVAL;
		break;
	}

	/*
	 * Die Aufzeichnung hat im Original immer zwei Plaetze; EPERM
	 * (mvb_rec == 0) kommt deshalb nie vor.
	 */
	case IOCTL_PIXY_MVBLLI_REC_CONF:
	case IOCTL_PIXY_MVBLLI_REC_DEL:
		if (!uarg)
			return -EINVAL;
		if (!MVB_UARG_OK(arg, sizeof(mvb_rec_event)))
			return -EFAULT;
		if (cmd == IOCTL_PIXY_MVBLLI_REC_CONF)
			ret = mvb_conf_rec_event(d, uarg);
		else
			ret = mvb_del_rec_event(d, uarg);
		break;

	/*
	 * PD_NSDB ist im Original vorhanden, wird von diesem Geraet aber
	 * nachweislich nicht benutzt (STSR und dti belegen den
	 * PD_CONF-Pfad). Was das Original schon beim Einlesen ablehnt, lehnt
	 * der Nachbau genauso ab; eine formal brauchbare Datenbank meldet er
	 * deutlich mit ENOSYS, statt still etwas Falsches zu tun.
	 */
	case IOCTL_PIXY_MVBLLI_PD_NSDB: {
		mvb_config_nsdb nsdb;

		if (d->status.is_active)
			return -EALREADY;
		if (!uarg)
			return -EINVAL;
		if (!MVB_UARG_OK(arg, sizeof(nsdb)))
			return -EFAULT;
		if (copy_from_user(&nsdb, uarg, sizeof(nsdb)))
			return -EINVAL;
		if (!nsdb.length || nsdb.length > MVBLLI_NSDB_SIZE ||
		    !nsdb.nsdb)
			return -EINVAL;
		pr_warn_once(DRV_NAME
			     ": NSDB configuration is not implemented\n");
		ret = -ENOSYS;
		break;
	}

	case IOCTL_PIXY_MVBLLI_PD_CONF:
		if (d->status.is_active)
			return -EALREADY;
		if (!uarg)
			return -EINVAL;
		if (!MVB_UARG_OK(arg, 2))
			return -EFAULT;
		ret = mvblli_do_pd_conf(d, uarg);
		break;

	case IOCTL_PIXY_MVBLLI_MD_CONF: {
		PixyMvblliConfigMex mex;

		if (d->status.is_active)
			return -EALREADY;
		if (!uarg)
			return -EINVAL;
		if (!MVB_UARG_OK(arg, sizeof(mex)))
			return -EFAULT;
		if (copy_from_user(&mex, uarg, sizeof(mex)))
			return -EINVAL;
		d->q_tq_priority = mex.q_tq_priority;
		d->status.has_md = 1;
		break;
	}

	/* Wert, nicht Zeiger - wie WRITE_DEV_ADDR */
	case IOCTL_PIXY_MVBLLI_DISABLE_PORT:
		if (!arg)
			return -EINVAL;
		if (!MVB_UARG_OK(arg, 2))
			return -EFAULT;
		if (arg > 0xfff)
			return -EINVAL;
		ret = apd_port_disable(d, (u16)arg) ? -EIO : 0;
		break;

	case IOCTL_PIXY_MVBLLI_READ_TM: {
		mvb_tm tm;

		if (!uarg)
			return -EINVAL;
		if (!MVB_UARG_OK(arg, sizeof(tm)))
			return -EFAULT;
		memset(&tm, 0, sizeof(tm));
		tm.ts_id = d->ts_id;
		tm.address = (u16 *)d->p_tm;
		tm.size_id = d->mcm;
		if (copy_to_user(uarg, &tm, sizeof(tm)))
			ret = -EINVAL;
		break;
	}

	/* ohne Argument - ein Argument ungleich 0 ist EINVAL */
	case IOCTL_PIXY_MVBLLI_MD_FLUSH_QUEUE:
		if (arg)
			return -EINVAL;
		mvb_md_flush_send_queue(d);
		break;

	/*
	 * Das Original liest ein 32-Bit-Wort ein und prueft, dass in beiden
	 * Haelften nur die Bits 0..2 stehen. Als selector und reset reicht
	 * es dann aber nicht dieses Wort weiter, sondern den Zeiger selbst:
	 * Bits 16..31 der Adresse sind der selector, Bits 0..15 reset.
	 * Zurueck kommen 2 Byte, nicht 4.
	 *
	 * Das ist ein Fehler im Original. Er wird hier bewusst nachgebaut:
	 * die Anwendung laeuft seit Jahren mit genau diesem Verhalten, und
	 * welche Statusbits sie sieht und welche dabei zurueckgesetzt werden,
	 * haengt davon ab.
	 */
	case IOCTL_PIXY_MVBLLI_MD_GET_STATUS:
		if (!uarg)
			return -EINVAL;
		if (!MVB_UARG_OK(arg, 4))
			return -EFAULT;
		if (copy_from_user(&v32, uarg, 4))
			return -EINVAL;
		if ((v32 | (v32 >> 16)) & 0xfff8)
			return -EINVAL;
		v16 = (u16)mvb_md_get_status(d, (u16)(arg >> 16), (u16)arg);
		if (copy_to_user(uarg, &v16, 2))
			ret = -EINVAL;
		break;

	/*
	 * WRITE_CONTROL fasst drei Dinge zusammen: Geraeteadresse,
	 * Antwortfenster und Leitungsbetrieb. t_ignore ist dabei eine
	 * Zeitangabe in Mikrosekunden, keine Feldnummer - sie wird ueber
	 * Schwellen auf das TMO-Feld abgebildet. Die Kommandobits sind
	 * big-endian gepackt: aon|aof|spl|tms|sla|slb|cla|clb.
	 */
	case IOCTL_PIXY_MVBLLI_WRITE_CONTROL: {
		mvb_ctrl ctrl;
		u8 cmd8;
		u16 treply, line;
		bool keep_treply = false;

		if (!uarg)
			return -EINVAL;
		if (!MVB_UARG_OK(arg, sizeof(ctrl)))
			return -EFAULT;
		if (copy_from_user(&ctrl, uarg, sizeof(ctrl)))
			return -EINVAL;
		cmd8 = *(u8 *)&ctrl.command;

		if (ctrl.dev_addr < 0x1000) {
			if (d->status.is_active)
				return -EFAULT;
			if (mvb_set_device_address(d, ctrl.dev_addr))
				return -EIO;
		}

		if (ctrl.t_ignore == 0)
			treply = 1;
		else if (ctrl.t_ignore < 0x20)
			treply = 0;		/* 1..31   us -> 21 us */
		else if (ctrl.t_ignore < 0x35)
			treply = 1;		/* 32..52  us -> 43 us */
		else if (ctrl.t_ignore < 0x4b)
			treply = 2;		/* 53..74  us -> 64 us */
		else if (ctrl.t_ignore < 0x100)
			treply = 3;		/* 75..255 us -> 83 us */
		else
			keep_treply = true;	/* unveraendert lassen */

		if (keep_treply)
			treply = d->treply_config;

		if ((cmd8 & 0x0c) == 0x0c)
			line = MVB_LINE_BOTH;
		else if (cmd8 & 0x08)
			line = MVB_LINE_A;
		else if (cmd8 & 0x04)
			line = MVB_LINE_B;
		else
			line = d->line_config;

		/*
		 * Aendert sich weder Antwortfenster noch Leitung, ruft das
		 * Original mvb_hardw_config() gar nicht erst auf.
		 */
		if (!keep_treply || (cmd8 & 0x0c)) {
			if (mvb_hardw_config(d, line, treply))
				return -EIO;
			if ((cmd8 & 0x0c) == 0x0c) {
				sa_w16(d, MVBC_DR,
				       sa_r16(d, MVBC_DR) | TM_DR_LS);
				mvb_set_laa_rld();
			}
		}

		/* cla/clb setzen die Fehlerzaehler zurueck */
		if ((cmd8 & 3) == 3) {
			mvb_clear_counters(d, true, true, true);
			d->status.errors = 0;
			d->status.errors_a = 0;
			d->status.errors_b = 0;
		} else if (cmd8 & 2) {
			mvb_clear_counters(d, false, true, false);
			d->status.errors_a = 0;
		} else if (cmd8 & 1) {
			mvb_clear_counters(d, false, false, true);
			d->status.errors_b = 0;
		}
		break;
	}

	case IOCTL_PIXY_MVBLLI_HWINIT:
		if (!uarg)
			return -EINVAL;
		if (!MVB_UARG_OK(arg, sizeof(PixyMvblliConfigLpTs)) ||
		    d->status.is_active)
			return -EFAULT;
		ret = mvb_conf_lpts(d, uarg);
		break;

	case IOCTL_PIXY_MVBLLI_USERS:
		if (!uarg)
			return -EINVAL;
		if (!MVB_UARG_OK(arg, 4))
			return -EFAULT;
		v32 = d->users;
		if (copy_to_user(uarg, &v32, 4))
			ret = -EINVAL;
		break;

	/*
	 * Alles andere mit dem richtigen Magic und einer Nummer bis 22 -
	 * darunter MD_NSDB und BA_NSDB, die im Kopf stehen, im Verteiler
	 * des Originals aber fehlen - endet dort in EINVAL, nicht ENOTTY.
	 */
	default:
		ret = -EINVAL;
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
	spin_lock_init(&d->irq_lock);
	INIT_DELAYED_WORK(&d->irq_wd, mvb_irq_watchdog_fn);
	mutex_init(&d->lock);
	init_waitqueue_head(&d->wait_poll);

	d->devt = MKDEV(MAJOR(drvdata.devt_base),
			MINOR(drvdata.devt_base) + drvdata.total_devices);

	cdev_init(&d->cdev, &mvblli_fops);
	d->cdev.owner = THIS_MODULE;

	/* 255 Minor je Karte, wie im Original - MVBLLI_MINOR_COUNT = 3 * 255 */
	ret = cdev_add(&d->cdev, d->devt, 0xff);
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

	/*
	 * Falls release() das Abmelden ueberspringen musste, weil der
	 * Prozess gerade beendet wurde, wird es hier nachgeholt - rmmod
	 * laeuft in einem intakten Kontext.
	 */
	mvblli_detach_board(d);

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

/*
 * Sucht /dev/mvb0..2 ab. Jede Karte, die sich oeffnen laesst, wird sofort
 * selbst angemeldet; das Abonnement beim Board-Treiber deckt nur die
 * spaeter hinzukommenden Karten ab. Genau so macht es das Original - und
 * nur so laeuft dieses Modul auch auf dem originalen Board-Treiber, der
 * beim Abonnieren nichts ueber bereits vorhandene Karten meldet.
 */
static int mvblli_scan_boards(void)
{
	struct PixyMvbBoardUpperDrvSubscribe sub;
	bool subscribed = false;
	char path[16];
	int i, ret;

	for (i = 0; i < MVBLLI_MAX_DEV; i++) {
		struct file *filp;

		scnprintf(path, sizeof(path), "/dev/mvb%d", i);
		filp = filp_open(path, O_RDONLY, 0);
		if (IS_ERR(filp))
			continue;

		mvb_add_board(i, NULL);

		if (!subscribed) {
			memset(&sub, 0, sizeof(sub));
			sub.add_brd.brd_evnt = mvb_add_board;
			sub.rm_brd.brd_evnt = mvb_remove_board;

			ret = board_ioctl(filp,
				IOCTL_PIXY_MVB_BOARD_KSET_DRV_SUBSCRIBE,
				&sub);
			if (ret) {
				pr_err(DRV_NAME
				       ": cannot subscribe as upper driver\n");
				filp_close(filp, NULL);
				return ret;
			}
			drvdata.drv_id = sub.drv_id;
			subscribed = true;
		}

		filp_close(filp, NULL);
	}

	if (!subscribed) {
		pr_err(DRV_NAME ": /dev/mvb0 not available - is pixy-mvb loaded?\n");
		return -ENODEV;
	}

	return 0;
}

static void mvblli_unsubscribe(void)
{
	struct file *filp;
	int id = drvdata.drv_id;

	filp = filp_open("/dev/mvb0", O_RDONLY, 0);
	if (IS_ERR(filp))
		return;

	board_ioctl(filp, IOCTL_PIXY_MVB_BOARD_KSET_DRV_UNSUBSCRIBE, &id);
	filp_close(filp, NULL);
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

	ret = mvblli_scan_boards();
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

	mvblli_unsubscribe();

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
MODULE_DESCRIPTION("Pixy-1000 MVB link layer interface Driver");
MODULE_VERSION(DRV_VERSION);
