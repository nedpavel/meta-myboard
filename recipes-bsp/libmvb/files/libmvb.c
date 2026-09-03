/* libmvb.c — Userspace-MVB-Zugriff ueber das gemappte Traffic-Memory
 *
 * Siehe libmvb.h fuer das TM-Layout. Alle Zugriffe erfolgen 16-bit-weise,
 * weil das FPGA-Fenster ein Word-Port ist (INC-100: "Auf diesen Speicher
 * kann nur ueber einen Word-Port zugegriffen werden [16 bit]").
 */
#include "libmvb.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/mman.h>

#define TM_SIZE        0x10000      /* 64 KB */

/* --- TM-Regionen (MCM=2), Byte-Offsets, vgl. TM_LAYOUT_64K in mvbc.h --- */
#define LA_PIT         0x0000       /* 4096 Worte: log. Adresse -> Dock-Index */
#define DA_PIT         0x2000
#define LA_DATA        0x4000       /* 1024 Docks x 2 Seiten                  */
#define LA_FRCE        0x8000
#define LA_PCS         0xC000       /* 1024 Ports x 4 Worte                   */
#define DA_DATA        0xE000
#define DA_PCS         0xF000
#define SRV_AREA       0xFC00

/* Service Area intern (vgl. TM_TYPE_SERVICE_AREA) */
#define SA_PP_DATA     (SRV_AREA + 0x000)   /* 8 x 64 Byte                    */
#define SA_PP_PCS      (SRV_AREA + 0x200)   /* 32 x 8 Byte                    */
#define SA_MFS         (SRV_AREA + 0x300)   /* Master Frame Slot              */
#define SA_QDT         (SRV_AREA + 0x310)   /* Queue Descriptor Table         */
#define QDT_XQ0        (SA_QDT + 0)         /* Sende-Queue 0 (hohe Prioritaet)*/
#define QDT_XQ1        (SA_QDT + 2)         /* Sende-Queue 1                  */
#define QDT_RQ         (SA_QDT + 4)         /* Empfangs-Queue                 */

/* Ablage der Message-Queue: der 1-KB-Block, den TM_LAYOUT_64K zwischen
   da_pcs und der Service Area als 'free_1' unbenutzt laesst. */
#define MSGQ_BASE      0xF800
#define MSGQ_END       0xFC00

/* Queue-Zeiger sind 16 Bit und adressieren in 4-Byte-Schritten:
   TM-Adresse = (Zeiger << 2) + Queue-Offset (QO im MCR, hier 0). */
#define PTR_OF(addr)   ((uint16_t)((addr) >> 2))
#define ADDR_OF(ptr)   ((uint32_t)(ptr) << 2)
/* SA + 0x380 == 0xFF80 == MVB_REG_SCR (siehe libmvb.h)                       */

/* Physische Ports (Indizes in pp_pcs) */
#define PP_MSRC        0x8          /* Message Source Port                    */
#define PP_MSNK        0xC          /* Message Sink Port                      */

#define LA_PORT_COUNT  1024         /* Docks im LA-Bereich bei MCM=2          */
#define MAX_PORTS      256          /* von dieser Lib verwaltete Ports        */

struct port_entry {
    uint16_t log_addr;
    uint16_t index;     /* Dock-/Port-Index im TM       */
    uint8_t  size;      /* Nutzdaten in Byte (2..32)    */
    uint8_t  dir;       /* MVB_SINK / MVB_SRC           */
    uint8_t  used;
};

struct mvb_dev {
    int                 fd;
    volatile uint8_t   *tm;
    mvb_config          cfg;
    struct port_entry   ports[MAX_PORTS];
    int                 next_dock;   /* einfacher Dock-Allokator */

    /* Message-Empfangs-Queue (Ringpuffer) */
    int                 msg_n;       /* Anzahl Bloecke, 0 = nicht eingerichtet */
    uint32_t            msg_blocks;  /* TM-Offset des ersten Datenblocks       */
    int                 msg_rd;      /* Leseindex der Software                 */
};

/* --- 16-bit-Zugriffe auf das TM --------------------------------------- */
static inline uint16_t rd(mvb_dev *d, uint32_t off)
{
    return *(volatile uint16_t *)(d->tm + off);
}
static inline void wr(mvb_dev *d, uint32_t off, uint16_t v)
{
    *(volatile uint16_t *)(d->tm + off) = v;
}

uint16_t mvb_tm_rd(mvb_dev *d, uint32_t off)          { return rd(d, off); }
void     mvb_tm_wr(mvb_dev *d, uint32_t off, uint16_t v) { wr(d, off, v); }
uint16_t mvb_get_reg(mvb_dev *d, uint32_t off)        { return rd(d, off); }
uint16_t mvb_get_dr(mvb_dev *d)                       { return rd(d, MVB_REG_DR); }

/* --- Adressrechnung ---------------------------------------------------- */
/* Der Datenbereich ist in Gruppen zu 4 Docks organisiert; jede Gruppe hat
   zwei Seiten (Double Buffering) zu je 32 Byte:
       [Gruppe n] = [Seite0: Dock0..3][Seite1: Dock0..3]
   (vgl. TM_TYPE_DATA/TM_TYPE_PAGE/TM_TYPE_DOCK in mvbc.h). */
static uint32_t data_addr(int idx, int page)
{
    return LA_DATA + (uint32_t)(idx >> 2) * 64 + (uint32_t)page * 32
                   + (uint32_t)(idx & 3) * 8;
}
static uint32_t pcs_addr(int idx) { return LA_PCS + (uint32_t)idx * 8; }
static uint32_t pit_addr(uint16_t la) { return LA_PIT + (uint32_t)la * 2; }

/* MVB-F-Code aus der Portgroesse (2,4,8,16,32 Byte -> FC0..FC4) */
static int size_to_fcode(int bytes)
{
    switch (bytes) {
    case 2:  return 0;
    case 4:  return 1;
    case 8:  return 2;
    case 16: return 3;
    case 32: return 4;
    default: return -1;
    }
}
/* Anzahl belegter Docks (1 Dock = 8 Byte) */
static int size_to_docks(int bytes) { return (bytes <= 8) ? 1 : bytes / 8; }

static struct port_entry *find_port(mvb_dev *d, uint16_t la)
{
    int i;
    for (i = 0; i < MAX_PORTS; i++)
        if (d->ports[i].used && d->ports[i].log_addr == la)
            return &d->ports[i];
    return NULL;
}

/* --- Oeffnen / Schliessen ---------------------------------------------- */
int mvb_open(const char *devnode, mvb_dev **out)
{
    mvb_dev *d;
    void *m;

    if (!out)
        return MVB_ERR_PARAM;
    if (!devnode)
        devnode = "/dev/pixymvbip";

    d = calloc(1, sizeof(*d));
    if (!d)
        return MVB_ERR_IO;

    d->fd = open(devnode, O_RDWR | O_SYNC);
    if (d->fd < 0) {
        free(d);
        return MVB_ERR_IO;
    }
    m = mmap(NULL, TM_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED, d->fd, 0);
    if (m == MAP_FAILED) {
        close(d->fd);
        free(d);
        return MVB_ERR_IO;
    }
    d->tm = (volatile uint8_t *)m;
    d->next_dock = 1;      /* Dock 0 bleibt der "Trash Dock" */
    *out = d;
    return MVB_OK;
}

void mvb_close(mvb_dev *d)
{
    if (!d)
        return;
    if (d->tm)
        munmap((void *)d->tm, TM_SIZE);
    if (d->fd >= 0)
        close(d->fd);
    free(d);
}

/* --- Init-Level -------------------------------------------------------- */
int mvb_get_level(mvb_dev *d)
{
    if (!d) return MVB_ERR_PARAM;
    return rd(d, MVB_REG_SCR) & 0x0003;
}

int mvb_set_level(mvb_dev *d, int il)
{
    uint16_t scr;
    if (!d || il < 0 || il > 3)
        return MVB_ERR_PARAM;
    scr = rd(d, MVB_REG_SCR);
    scr = (uint16_t)((scr & ~0x0003) | (uint16_t)il);
    wr(d, MVB_REG_SCR, scr);
    return (mvb_get_level(d) == il) ? MVB_OK : MVB_ERR_IO;
}

/* --- Konfiguration ----------------------------------------------------- */
int mvb_configure(mvb_dev *d, const mvb_config *cfg)
{
    uint16_t mcr, dr;
    int i;

    if (!d || !cfg)
        return MVB_ERR_PARAM;
    if (cfg->device_addr > 4095 || cfg->sink_supv > 9)
        return MVB_ERR_PARAM;

    d->cfg = *cfg;
    if (d->cfg.mcm == 0)
        d->cfg.mcm = 2;                     /* Default: 64 KB */

    /* 1) Reset-Level */
    wr(d, MVB_REG_SCR, 0x0000);

    /* 2) Memory Configuration Register.
       Bit 7 (ICM, Intercycle Delay) muss laut MVBIP-Errata '1' sein,
       Bit 10 (DMA_DIRECT) '0'. Bits [2:0] = MCM. Bits 15..11 sind die
       (read-only) Versionsnummer, daher nur die unteren Bits schreiben. */
    mcr = (uint16_t)(0x0080 | (d->cfg.mcm & 0x7));
    wr(d, MVB_REG_MCR, mcr);

    /* 3) Decoder-Register: WTR=1 (Reply-Timeout fuer alle F-Codes),
       TRAFO nur fuer EMD, OPTO nur fuer OGF. */
    dr = 0x0010;                             /* WTR */
    if (d->cfg.phy == MVB_PHY_EMD)
        dr |= 0x0020;                        /* TRAFO */
    else if (d->cfg.phy == MVB_PHY_OGF)
        dr |= 0x0040;                        /* OPTO  */
    wr(d, MVB_REG_DR, dr);

    /* 4) Sink-Time-Supervision: Intervall in den oberen 4 Bit, Bereich
       (hoechste ueberwachte Portnummer) in den unteren 12 Bit. */
    wr(d, MVB_REG_STSR,
       (uint16_t)(((uint16_t)d->cfg.sink_supv << 12) | (LA_PORT_COUNT - 1)));

    /* 5) Geraeteadresse (Override, falls != 0; sonst gelten die HW-Pins) */
    if (d->cfg.device_addr) {
        wr(d, MVB_REG_DAOK, 0x0094);         /* Override freischalten */
        wr(d, MVB_REG_DAOR, d->cfg.device_addr);
    }

    /* 6) LA-PIT komplett auf Dock 0 setzen -> alle nicht benutzten
       logischen Adressen landen im "Trash Dock" (Datasheet §5.1). */
    for (i = 0; i < 4096; i++)
        wr(d, LA_PIT + (uint32_t)i * 2, 0);

    /* 7) PCS Word0/Word1 aller Ports loeschen -> alle Ports inaktiv */
    for (i = 0; i < LA_PORT_COUNT; i++) {
        wr(d, pcs_addr(i) + 0, 0);
        wr(d, pcs_addr(i) + 2, 0);
    }

    /* 8) Port-Verwaltung zuruecksetzen und in den Config-Level gehen */
    memset(d->ports, 0, sizeof(d->ports));
    d->next_dock = 1;
    return mvb_set_level(d, MVB_IL_CONFIG);
}

/* --- Prozessdaten-Ports ------------------------------------------------ */
int mvb_add_port(mvb_dev *d, uint16_t log_addr, int dir, int size_bytes)
{
    int fc, docks, idx, i, slot = -1;
    uint16_t w0;

    if (!d || log_addr == 0 || log_addr > 4095)
        return MVB_ERR_PARAM;
    if (dir != MVB_SINK && dir != MVB_SRC)
        return MVB_ERR_PARAM;
    fc = size_to_fcode(size_bytes);
    if (fc < 0)
        return MVB_ERR_PARAM;
    if (mvb_get_level(d) != MVB_IL_CONFIG)
        return MVB_ERR_STATE;
    if (find_port(d, log_addr))
        return MVB_ERR_PARAM;               /* schon konfiguriert */

    for (i = 0; i < MAX_PORTS; i++)
        if (!d->ports[i].used) { slot = i; break; }
    if (slot < 0)
        return MVB_ERR_SPACE;

    /* Dock-Index vergeben. Ports > 8 Byte belegen mehrere Docks und
       muessen darauf ausgerichtet sein, damit sie nicht ueber eine
       Seitengrenze (4 Docks) hinausragen. */
    docks = size_to_docks(size_bytes);
    idx = d->next_dock;
    if (docks > 1)
        idx = (idx + docks - 1) & ~(docks - 1);
    if (idx + docks > LA_PORT_COUNT)
        return MVB_ERR_SPACE;
    d->next_dock = idx + docks;

    /* PCS Word0: F-Code + Richtung. Word1 (Status/VP) auf 0. */
    w0 = (uint16_t)((fc << 12) | (dir == MVB_SRC ? 0x0800 : 0x0400));
    wr(d, pcs_addr(idx) + 0, w0);
    wr(d, pcs_addr(idx) + 2, 0);

    /* Datenbereich beider Seiten vorbelegen */
    for (i = 0; i < size_bytes / 2; i++) {
        wr(d, data_addr(idx, 0) + (uint32_t)i * 2, 0);
        wr(d, data_addr(idx, 1) + (uint32_t)i * 2, 0);
    }

    /* Port ueber die Port-Index-Table erreichbar machen */
    wr(d, pit_addr(log_addr), (uint16_t)idx);

    d->ports[slot].log_addr = log_addr;
    d->ports[slot].index    = (uint16_t)idx;
    d->ports[slot].size     = (uint8_t)size_bytes;
    d->ports[slot].dir      = (uint8_t)dir;
    d->ports[slot].used     = 1;
    return MVB_OK;
}

int mvb_del_port(mvb_dev *d, uint16_t log_addr)
{
    struct port_entry *p;
    if (!d)
        return MVB_ERR_PARAM;
    if (mvb_get_level(d) != MVB_IL_CONFIG)
        return MVB_ERR_STATE;
    p = find_port(d, log_addr);
    if (!p)
        return MVB_ERR_NOPORT;

    wr(d, pit_addr(log_addr), 0);           /* zurueck auf Trash Dock */
    wr(d, pcs_addr(p->index) + 0, 0);
    wr(d, pcs_addr(p->index) + 2, 0);
    memset(p, 0, sizeof(*p));
    return MVB_OK;
}

int mvb_get_port(mvb_dev *d, uint16_t log_addr, void *data, int size_bytes,
                 uint16_t *status)
{
    struct port_entry *p;
    uint8_t *out = (uint8_t *)data;
    uint16_t pcs1;
    uint32_t base;
    int i, page;

    if (!d || !data || size_bytes <= 0)
        return MVB_ERR_PARAM;
    p = find_port(d, log_addr);
    if (!p)
        return MVB_ERR_NOPORT;
    if (size_bytes > p->size)
        size_bytes = p->size;

    /* Valid Page: die Seite, in der der MVBC gueltige Daten bereitstellt. */
    pcs1 = rd(d, pcs_addr(p->index) + 2);
    page = (pcs1 >> 6) & 1;
    base = data_addr(p->index, page);

    for (i = 0; i < size_bytes / 2; i++) {
        uint16_t w = rd(d, base + (uint32_t)i * 2);
        out[2 * i]     = (uint8_t)(w & 0xFF);
        out[2 * i + 1] = (uint8_t)(w >> 8);
    }
    if (status)
        *status = (uint16_t)(pcs1 & 0x003F);
    return MVB_OK;
}

int mvb_put_port(mvb_dev *d, uint16_t log_addr, const void *data,
                 int size_bytes)
{
    struct port_entry *p;
    const uint8_t *in = (const uint8_t *)data;
    uint16_t pcs1;
    uint32_t base;
    int i, page;

    if (!d || !data || size_bytes <= 0)
        return MVB_ERR_PARAM;
    p = find_port(d, log_addr);
    if (!p)
        return MVB_ERR_NOPORT;
    if (p->dir != MVB_SRC)
        return MVB_ERR_PARAM;
    if (size_bytes > p->size)
        size_bytes = p->size;

    /* In die *inaktive* Seite schreiben, danach VP umschalten — so sieht
       der MVBC immer einen konsistenten Datensatz. */
    pcs1 = rd(d, pcs_addr(p->index) + 2);
    page = !((pcs1 >> 6) & 1);
    base = data_addr(p->index, page);

    for (i = 0; i < size_bytes / 2; i++) {
        uint16_t w = (uint16_t)(in[2 * i] | ((uint16_t)in[2 * i + 1] << 8));
        wr(d, base + (uint32_t)i * 2, w);
    }
    pcs1 = (uint16_t)((pcs1 & ~0x0040) | (page ? 0x0040 : 0));
    wr(d, pcs_addr(p->index) + 2, pcs1);
    return MVB_OK;
}

/* --- Variablen im Portpuffer ------------------------------------------- */
/* MVB uebertraegt MSB zuerst: Bit 0 ist das hoechstwertige Bit von Byte 0. */
int mvb_get_var(const void *portdata, int portsize_bytes,
                int bit_off, int bit_size, uint32_t *val)
{
    const uint8_t *b = (const uint8_t *)portdata;
    uint32_t v = 0;
    int i;

    if (!portdata || !val || bit_size < 1 || bit_size > 32 || bit_off < 0)
        return MVB_ERR_PARAM;
    if (bit_off + bit_size > portsize_bytes * 8)
        return MVB_ERR_PARAM;

    for (i = 0; i < bit_size; i++) {
        int bit = bit_off + i;
        int byte = bit >> 3;
        int shift = 7 - (bit & 7);          /* MSB first */
        v = (v << 1) | ((b[byte] >> shift) & 1);
    }
    *val = v;
    return MVB_OK;
}

int mvb_put_var(void *portdata, int portsize_bytes,
                int bit_off, int bit_size, uint32_t val)
{
    uint8_t *b = (uint8_t *)portdata;
    int i;

    if (!portdata || bit_size < 1 || bit_size > 32 || bit_off < 0)
        return MVB_ERR_PARAM;
    if (bit_off + bit_size > portsize_bytes * 8)
        return MVB_ERR_PARAM;

    for (i = 0; i < bit_size; i++) {
        int bit = bit_off + i;
        int byte = bit >> 3;
        int shift = 7 - (bit & 7);
        int v = (val >> (bit_size - 1 - i)) & 1;
        if (v)
            b[byte] |= (uint8_t)(1 << shift);
        else
            b[byte] &= (uint8_t)~(1 << shift);
    }
    return MVB_OK;
}

/* --- Message-Daten (Klasse 2), Empfang --------------------------------- */
/* Aufbau im TM (alles im freien 1-KB-Block ab MSGQ_BASE):
 *
 *   MSGQ_BASE          LLR[0..n-1]      je 2 Worte: Data-Ptr, Next-Ptr
 *   blocks_base        Block[0..n-1]    je 32 Byte (16 Worte)
 *
 * Die LLRs sind zu einem Ring verkettet. Der MVBC arbeitet ihn ab und
 * schreibt nach jedem empfangenen Frame den Next-Pointer in QDT_RQ —
 * daran erkennt die Software, wie weit er gekommen ist.
 */
static uint32_t llr_addr(int i)        { return MSGQ_BASE + (uint32_t)i * 4; }

int mvb_msg_setup(mvb_dev *d, int nblocks)
{
    uint32_t blocks_base;
    int i;

    if (!d || nblocks < 2 || nblocks > MVB_MSG_MAXBLK)
        return MVB_ERR_PARAM;
    if (mvb_get_level(d) != MVB_IL_CONFIG)
        return MVB_ERR_STATE;

    /* Datenbloecke hinter den LLRs, auf 32 Byte ausgerichtet (Vorgabe des
       Datenblatts: Data-Pointer muessen 16-Wort-ausgerichtet sein). */
    blocks_base = (MSGQ_BASE + (uint32_t)nblocks * 4 + 31u) & ~31u;
    if (blocks_base + (uint32_t)nblocks * MVB_MSG_BLOCK > MSGQ_END)
        return MVB_ERR_SPACE;

    /* Bloecke leeren und LLR-Ring aufbauen */
    for (i = 0; i < nblocks; i++) {
        uint32_t blk = blocks_base + (uint32_t)i * MVB_MSG_BLOCK;
        int w;
        for (w = 0; w < MVB_MSG_BLOCK / 2; w++)
            wr(d, blk + (uint32_t)w * 2, 0);
        wr(d, llr_addr(i) + 0, PTR_OF(blk));                       /* DP */
        wr(d, llr_addr(i) + 2, PTR_OF(llr_addr((i + 1) % nblocks)));/* NP */
    }

    /* Message-Sink-Port aktivieren: F-Code 12, SINK, Queue attached (QA). */
    wr(d, SA_PP_PCS + (uint32_t)PP_MSNK * 8 + 0, 0xC000 | 0x0400 | 0x0004);
    wr(d, SA_PP_PCS + (uint32_t)PP_MSNK * 8 + 2, 0);

    /* Queue beim MVBC anmelden; Sende-Queues bleiben leer (= nicht vorhanden) */
    wr(d, QDT_XQ0, 0);
    wr(d, QDT_XQ1, 0);
    wr(d, QDT_RQ, PTR_OF(llr_addr(0)));

    d->msg_n      = nblocks;
    d->msg_blocks = blocks_base;
    d->msg_rd     = 0;
    return MVB_OK;
}

/* Position des MVBC im Ring aus der QDT ableiten; <0 bei ungueltigem Zeiger */
static int msg_mvbc_index(mvb_dev *d)
{
    uint16_t ptr = rd(d, QDT_RQ);
    uint32_t addr;
    int idx;

    if (ptr == 0)
        return -1;                       /* Queue erschoepft/nicht vorhanden */
    addr = ADDR_OF(ptr);
    if (addr < MSGQ_BASE || addr >= MSGQ_BASE + (uint32_t)d->msg_n * 4)
        return -1;                       /* zeigt nicht in unseren Ring */
    idx = (int)((addr - MSGQ_BASE) / 4);
    return idx;
}

int mvb_msg_pending(mvb_dev *d)
{
    int idx, diff;

    if (!d)
        return MVB_ERR_PARAM;
    if (d->msg_n == 0)
        return MVB_ERR_STATE;
    idx = msg_mvbc_index(d);
    if (idx < 0)
        return MVB_ERR_IO;

    diff = idx - d->msg_rd;
    if (diff < 0)
        diff += d->msg_n;
    return diff;
}

int mvb_msg_recv(mvb_dev *d, void *buf, int maxlen)
{
    uint8_t *out = (uint8_t *)buf;
    uint32_t blk;
    int n, i;

    if (!d || !buf || maxlen <= 0)
        return MVB_ERR_PARAM;
    if (d->msg_n == 0)
        return MVB_ERR_STATE;

    n = mvb_msg_pending(d);
    if (n < 0)
        return n;
    if (n == 0)
        return 0;                        /* nichts Neues */

    if (maxlen > MVB_MSG_BLOCK)
        maxlen = MVB_MSG_BLOCK;
    blk = d->msg_blocks + (uint32_t)d->msg_rd * MVB_MSG_BLOCK;
    for (i = 0; i < maxlen / 2; i++) {
        uint16_t w = rd(d, blk + (uint32_t)i * 2);
        out[2 * i]     = (uint8_t)(w & 0xFF);
        out[2 * i + 1] = (uint8_t)(w >> 8);
    }

    d->msg_rd = (d->msg_rd + 1) % d->msg_n;
    return maxlen;
}

int mvb_msg_send(mvb_dev *d, uint16_t dest, const void *buf, int len)
{
    (void)d; (void)dest; (void)buf; (void)len;
    return MVB_ERR_NOSUP;                /* Transmit-Queue noch offen */
}
