/* libmvb.h — Userspace-API fuer den MVBIP (MVBC02) ueber /dev/pixymvbip
 *
 * Der Kernel-Treiber (pixymvbip) macht nichts weiter, als das 64-KB-Traffic-
 * Memory (TM) des MVB-Controllers per mmap() in den Userspace zu legen. Die
 * gesamte MVB-Logik lebt hier: MVBC-Register, Port-Konfiguration und der
 * Datenaustausch ueber das TM.
 *
 * TM-Layout = TM_LAYOUT_64K aus mvbc.h (Memory Configuration Mode 2):
 *
 *   0x0000  la_pit  [4096 Worte]  Port-Index-Table (logische Adressen)
 *   0x2000  da_pit  [4096 Worte]  Port-Index-Table (Geraeteadressen)
 *   0x4000  la_data [8192 Worte]  Datenbereich, 1024 Docks x 2 Seiten
 *   0x8000  la_frce [8192 Worte]  Force-Tabelle
 *   0xC000  la_pcs  [1024 x 4 W]  Port Control & Status
 *   0xE000  da_data [2048 Worte]
 *   0xF000  da_pcs  [256 x 4 W]
 *   0xFC00  Service Area (physische Ports, MFS, QDT)
 *   0xFF80  MVBC-Register (SCR, MCR, DR, STSR, ...)
 *
 * Prozessdaten (Klasse 1) sind vollstaendig implementiert. Fuer Message-Daten
 * (Klasse 2) stellt die Lib die TM-Anbindung bereit (QDT, MSRC/MSNK-Ports,
 * Queue-Zugriff) — siehe Hinweis bei mvb_msg_*().
 */
#ifndef LIBMVB_H
#define LIBMVB_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* --- Rueckgabewerte -------------------------------------------------- */
#define MVB_OK           0
#define MVB_ERR_PARAM   -1   /* ungueltiges Argument                     */
#define MVB_ERR_STATE   -2   /* falscher Init-Level fuer diese Operation */
#define MVB_ERR_IO      -3   /* open/mmap fehlgeschlagen                 */
#define MVB_ERR_NOPORT  -4   /* Port nicht konfiguriert                  */
#define MVB_ERR_SPACE   -5   /* kein freier Dock/Port-Index mehr         */
#define MVB_ERR_NOSUP   -6   /* Funktion (noch) nicht unterstuetzt       */

/* --- Port-Richtung ---------------------------------------------------- */
#define MVB_SINK         0   /* Port empfaengt vom Bus                    */
#define MVB_SRC          1   /* Port sendet auf den Bus                   */

/* --- Init-Level (SCR.IL) --------------------------------------------- */
#define MVB_IL_RESET     0
#define MVB_IL_CONFIG    1
#define MVB_IL_TEST      2
#define MVB_IL_RUNNING   3

/* --- Physical Layer / Leitung ---------------------------------------- */
#define MVB_PHY_OGF      0   /* Lichtwellenleiter                        */
#define MVB_PHY_ESD      1   /* RS485 (Standard auf diesem Board)        */
#define MVB_PHY_EMD      2   /* Trafo-gekoppelt                          */

#define MVB_LINE_B       0
#define MVB_LINE_A       1
#define MVB_LINE_BOTH    2

/* --- Sink-Status (Bits aus PCS Word1) -------------------------------- */
#define MVB_ST_STO    0x0001  /* Sink Time-out: Daten veraltet           */
#define MVB_ST_TERR   0x0002  /* Telegram Error                          */
#define MVB_ST_BNI    0x0004  /* Bus not idle                            */
#define MVB_ST_ALO    0x0008
#define MVB_ST_SQE    0x0010  /* Signal Quality Error                    */
#define MVB_ST_CRC    0x0020

typedef struct mvb_dev mvb_dev;

/* --- Konfiguration ---------------------------------------------------- */
typedef struct {
    uint16_t device_addr;   /* MVB-Geraeteadresse 1..4095                */
    uint8_t  mcm;           /* Memory Config Mode; 2 = 64 KB (Default)   */
    uint8_t  phy;           /* MVB_PHY_*                                 */
    uint8_t  line;          /* MVB_LINE_*                                */
    uint8_t  sink_supv;     /* Sink-Time-Supervision-Intervall 0..9      */
} mvb_config;

/* ---------------------------------------------------------------------- */
/* Geraet oeffnen: /dev/pixymvbip oeffnen und das 64-KB-TM mappen.        */
/* devnode = NULL -> "/dev/pixymvbip".                                    */
int  mvb_open(const char *devnode, mvb_dev **out);
void mvb_close(mvb_dev *d);

/* MVBC konfigurieren: Reset -> Register setzen -> Config-Level.
   Danach koennen Ports angelegt werden. */
int  mvb_configure(mvb_dev *d, const mvb_config *cfg);

/* Init-Level umschalten (z.B. MVB_IL_RUNNING = Betrieb aufnehmen). */
int  mvb_set_level(mvb_dev *d, int il);
int  mvb_get_level(mvb_dev *d);

/* --- Prozessdaten (Klasse 1) ------------------------------------------ */
/* Port anlegen. size_bytes muss 2/4/8/16/32 sein (MVB-F-Codes 0..4).
   Nur im Config-Level erlaubt. */
int  mvb_add_port(mvb_dev *d, uint16_t log_addr, int dir, int size_bytes);
int  mvb_del_port(mvb_dev *d, uint16_t log_addr);

/* Sink-Port lesen. status (optional) liefert die MVB_ST_*-Bits;
   MVB_ST_STO bedeutet: Daten sind nicht mehr frisch. */
int  mvb_get_port(mvb_dev *d, uint16_t log_addr, void *data, int size_bytes,
                  uint16_t *status);

/* Source-Port schreiben (Double-Buffering wird intern gehandhabt). */
int  mvb_put_port(mvb_dev *d, uint16_t log_addr, const void *data,
                  int size_bytes);

/* --- Variablen aus Port-Daten extrahieren ----------------------------- */
/* bit_off zaehlt ab dem MSB von Byte 0 (MVB-Konvention, big-endian auf
   dem Bus). bit_size 1..32. */
int  mvb_get_var(const void *portdata, int portsize_bytes,
                 int bit_off, int bit_size, uint32_t *val);
int  mvb_put_var(void *portdata, int portsize_bytes,
                 int bit_off, int bit_size, uint32_t val);

/* --- Message-Daten (Klasse 2) ----------------------------------------- */
/* Die Message-Ports (MSRC/MSNK) und die Queue Descriptor Table liegen in
   der Service Area. Diese Funktionen stellen den TM-Zugang bereit; der
   vollstaendige Link-Layer (Segmentierung, Quittungen) ist noch offen und
   liefert derzeit MVB_ERR_NOSUP. mvb_msg_ready() zeigt an, ob der MVBC
   ueberhaupt Klasse-2-Verkehr signalisiert. */
int  mvb_msg_ready(mvb_dev *d);
int  mvb_msg_send(mvb_dev *d, uint16_t dest, const void *buf, int len);
int  mvb_msg_recv(mvb_dev *d, uint16_t *src, void *buf, int maxlen);

/* --- Diagnose / Rohzugriff -------------------------------------------- */
uint16_t mvb_tm_rd(mvb_dev *d, uint32_t byte_off);
void     mvb_tm_wr(mvb_dev *d, uint32_t byte_off, uint16_t val);
uint16_t mvb_get_reg(mvb_dev *d, uint32_t reg_byte_off); /* MVB_REG_*    */
uint16_t mvb_get_dr(mvb_dev *d);   /* Decoder-Register (Leitungsdiagnose) */

/* MVBC-Registeradressen (absolut im TM, MCM=2) */
#define MVB_REG_SCR   0xFF80
#define MVB_REG_MCR   0xFF84
#define MVB_REG_DR    0xFF88
#define MVB_REG_STSR  0xFF8C
#define MVB_REG_FC    0xFF90
#define MVB_REG_EC    0xFF94
#define MVB_REG_DAOR  0xFFD8
#define MVB_REG_DAOK  0xFFDC

#ifdef __cplusplus
}
#endif
#endif /* LIBMVB_H */
