/* mvbtool.c — Diagnose- und Testwerkzeug fuer libmvb
 *
 *   mvbtool info                          MVBC-Register anzeigen
 *   mvbtool init <devaddr>                MVBC konfigurieren (ESD, 64K)
 *   mvbtool dump <off_hex> [len_hex]      Traffic-Memory hexdumpen
 *   mvbtool watch <logaddr> <bytes> [n]   Sink-Port zyklisch lesen
 *   mvbtool put <logaddr> <bytes> <hex..> Source-Port schreiben
 */
#include "libmvb.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static void show_regs(mvb_dev *d)
{
    uint16_t scr = mvb_get_reg(d, MVB_REG_SCR);
    static const char *lvl[] = { "Reset", "Config", "Test", "Running" };
    printf("SCR  = 0x%04X  (Init-Level %d = %s)\n",
           scr, scr & 3, lvl[scr & 3]);
    printf("MCR  = 0x%04X  (MCM=%d -> %s)\n",
           mvb_get_reg(d, MVB_REG_MCR),
           mvb_get_reg(d, MVB_REG_MCR) & 7,
           (mvb_get_reg(d, MVB_REG_MCR) & 7) == 2 ? "64 KB" : "andere Groesse");
    printf("DR   = 0x%04X  (Decoder/Leitungsdiagnose)\n", mvb_get_dr(d));
    printf("STSR = 0x%04X  (Sink-Time-Supervision)\n",
           mvb_get_reg(d, MVB_REG_STSR));
    printf("FC   = 0x%04X  EC = 0x%04X  (Frame-/Fehlerzaehler)\n",
           mvb_get_reg(d, MVB_REG_FC), mvb_get_reg(d, MVB_REG_EC));
}

int main(int argc, char **argv)
{
    const char *cmd = (argc > 1) ? argv[1] : "info";
    mvb_dev *d;
    int rc;

    rc = mvb_open(NULL, &d);
    if (rc != MVB_OK) {
        fprintf(stderr, "mvb_open fehlgeschlagen (%d) — Modul geladen? "
                        "/dev/pixymvbip vorhanden? root?\n", rc);
        return 1;
    }

    if (!strcmp(cmd, "info")) {
        show_regs(d);

    } else if (!strcmp(cmd, "init")) {
        mvb_config cfg;
        memset(&cfg, 0, sizeof(cfg));
        cfg.device_addr = (argc > 2) ? (uint16_t)strtoul(argv[2], 0, 0) : 0;
        cfg.mcm       = 2;              /* 64 KB */
        cfg.phy       = MVB_PHY_ESD;    /* RS485 */
        cfg.line      = MVB_LINE_BOTH;
        cfg.sink_supv = 5;              /* 16 ms */
        rc = mvb_configure(d, &cfg);
        printf("mvb_configure -> %d\n", rc);
        show_regs(d);

    } else if (!strcmp(cmd, "dump")) {
        uint32_t off = (argc > 2) ? strtoul(argv[2], 0, 16) : 0;
        uint32_t len = (argc > 3) ? strtoul(argv[3], 0, 16) : 0x100;
        uint32_t i;
        for (i = 0; i < len; i += 16) {
            uint32_t j;
            printf("%05X: ", off + i);
            for (j = 0; j < 16; j += 2)
                printf("%04X ", mvb_tm_rd(d, off + i + j));
            printf("\n");
        }

    } else if (!strcmp(cmd, "watch")) {
        uint16_t la = (uint16_t)strtoul(argv[2], 0, 0);
        int size    = (argc > 3) ? atoi(argv[3]) : 8;
        int n       = (argc > 4) ? atoi(argv[4]) : 10;
        uint8_t buf[32];
        uint16_t st;
        int k;

        rc = mvb_add_port(d, la, MVB_SINK, size);
        printf("add_port(0x%03X, SINK, %d) -> %d\n", la, size, rc);
        mvb_set_level(d, MVB_IL_RUNNING);

        for (k = 0; k < n; k++) {
            int i;
            rc = mvb_get_port(d, la, buf, size, &st);
            printf("%2d: rc=%d st=0x%02X  ", k, rc, st);
            for (i = 0; i < size; i++)
                printf("%02X ", buf[i]);
            if (st & MVB_ST_STO)
                printf(" [veraltet]");
            printf("\n");
            usleep(200000);
        }

    } else if (!strcmp(cmd, "put")) {
        uint16_t la = (uint16_t)strtoul(argv[2], 0, 0);
        int size    = (argc > 3) ? atoi(argv[3]) : 8;
        uint8_t buf[32];
        int i;
        memset(buf, 0, sizeof(buf));
        for (i = 0; i < size && (4 + i) < argc; i++)
            buf[i] = (uint8_t)strtoul(argv[4 + i], 0, 16);
        rc = mvb_add_port(d, la, MVB_SRC, size);
        printf("add_port(0x%03X, SRC, %d) -> %d\n", la, size, rc);
        mvb_set_level(d, MVB_IL_RUNNING);
        rc = mvb_put_port(d, la, buf, size);
        printf("put_port -> %d\n", rc);

    } else {
        fprintf(stderr,
                "Kommandos: info | init <devaddr> | dump <off> [len] | "
                "watch <logaddr> <bytes> [n] | put <logaddr> <bytes> <hex..>\n");
        mvb_close(d);
        return 2;
    }

    mvb_close(d);
    return 0;
}
