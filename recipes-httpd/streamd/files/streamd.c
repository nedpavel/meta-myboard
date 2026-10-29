/* streamd — nimmt einen dauerhaft offenen Datenstrom per TCP entgegen.
 *
 * Hintergrund: Ueber PHP-FPM laesst sich eine stehende Verbindung nicht
 * verarbeiten — FPM uebergibt einen Request erst, wenn er vollstaendig
 * vorliegt. Fuer echte Stroeme braucht es deshalb einen eigenen Dienst auf
 * einem eigenen Port.
 *
 * Sender, einmal verbinden und laufen lassen:
 *     while true; do echo "Wert $(date +%s%3N)"; sleep 0.01; done | nc <board> 9100
 *
 * Gelesen wird zeilenweise; jede Zeile ist ein Wert. Mehrere Sender
 * gleichzeitig sind moeglich (bis MAX_CLIENTS).
 *
 *     streamd [port] [transaktionen_pro_sekunde]     Vorgabe: 9100 60
 *
 * SCHREIBSTRATEGIE — bei bis zu 100 Werten/s:
 *   latest.txt  liegt im tmpfs (RAM) und wird bei JEDEM Wert aktualisiert.
 *               Die Anzeige liest immer den aktuellen Stand.
 *   werte       Tabelle in MariaDB (Datenbank icn). Werte werden
 *               gesammelt und 60-mal pro Sekunde in EINER Transaktion
 *               (mehrzeiliges INSERT) geschrieben; InnoDB schreibt jede
 *               Transaktion fest auf die CFast. Bei Stromausfall fehlen
 *               hoechstens die Werte der letzten ~17 ms.
 *
 * Anmeldung ohne Passwort ueber den Unix-Socket (unix_socket): der Dienst
 * laeuft als Linux-Benutzer streamd, der Datenbank-Benutzer heisst gleich.
 * Socket und Port liest die Client-Bibliothek aus /etc/my.cnf ([client]).
 * Ist die Datenbank (noch) nicht erreichbar, bleiben die Werte gepuffert
 * und der Dienst verbindet sich jede Sekunde neu.
 *
 * Datenbank abfragen:
 *     mariadb icn -e "SELECT zeit, wert FROM werte ORDER BY id DESC LIMIT 20;"
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <signal.h>
#include <time.h>
#include <fcntl.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <mysql.h>

/* NICHT 9000 verwenden — darauf lauscht php-fpm (siehe php-fpm.conf:
   listen = 127.0.0.1:9000). streamd startete zuerst, belegte den Port und
   php-fpm scheiterte mit "Address already in use". */
#define PORT_DEFAULT   9100
#define RATE_DEFAULT   60         /* Transaktionen pro Sekunde             */
#define MAX_CLIENTS    8
#define LINE_MAX_LEN   1024
#define RX_BUF         8192
#define STEMPEL_LEN    40

/* Pfade und Namen per -D ueberschreibbar, damit sich der Dienst ohne
   Systemrechte testen laesst. */
#ifndef PATH_LATEST
/* latest.txt im RAM (tmpfs): wird bei jedem Wert geschrieben */
#define PATH_LATEST    "/run/myboard-stream/latest.txt"
#endif
#ifndef DB_NAME
#define DB_NAME        "icn"
#endif
#define DB_MAX_ZEILEN  100000     /* Verlauf begrenzen (Ringpuffer)       */
#define KUERZEN_MS     60000      /* etwa minuetlich kuerzen              */
#define PENDING_MAX    2000       /* gesammelte Werte, hoechstens         */
#define NEU_VERBINDEN_MS 1000     /* nach Fehlschlag hoechstens 1x/s      */

static volatile sig_atomic_t laeuft = 1;
static void beenden(int sig) { (void)sig; laeuft = 0; }

/* Mikrosekunden seit Systemstart (monoton, springt nicht bei Zeitumstellung) */
static long long jetzt_us(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long)ts.tv_sec * 1000000 + ts.tv_nsec / 1000;
}

static void zeitstempel(char *out, size_t n)
{
    struct timespec ts;
    struct tm tm;
    clock_gettime(CLOCK_REALTIME, &ts);
    localtime_r(&ts.tv_sec, &tm);
    /* mit Millisekunden — bei 100 Werten/s sonst nicht unterscheidbar.
       Der Bereich wird ausdruecklich begrenzt (0..999): sonst kann der
       Compiler die Feldbreite nicht garantieren und warnt zu Recht vor
       einem moeglichen Abschneiden. */
    int ms = (int)(ts.tv_nsec / 1000000);
    if (ms < 0)   ms = 0;
    if (ms > 999) ms = 999;
    snprintf(out, n, "%04d-%02d-%02d %02d:%02d:%02d.%03d",
             tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
             tm.tm_hour, tm.tm_min, tm.tm_sec, ms);
}

/* latest.txt atomar ersetzen: erst schreiben, dann umbenennen. So sieht
   ein gleichzeitig lesendes PHP nie eine halb geschriebene Datei. */
static void latest_schreiben(const char *stempel, const char *zeile)
{
    char tmp[256];
    snprintf(tmp, sizeof(tmp), "%s.neu", PATH_LATEST);
    FILE *f = fopen(tmp, "w");
    if (!f)
        return;
    fprintf(f, "%s\t%s\n", stempel, zeile);
    fclose(f);
    rename(tmp, PATH_LATEST);
}

/* --- MariaDB --------------------------------------------------------- */

static MYSQL     *db;
static char      *sql;            /* Puffer fuer das mehrzeilige INSERT */
static long long  naechster_versuch;
static int        fehler_gemeldet;

static int db_verbinden(void)
{
    if (db)
        return 0;
    if (jetzt_us() < naechster_versuch)
        return -1;

    MYSQL *m = mysql_init(NULL);
    if (!m)
        return -1;
    /* Socket, Port und Zeichensatz aus /etc/my.cnf, Gruppen [client] und
       [streamd] */
    mysql_options(m, MYSQL_READ_DEFAULT_GROUP, "streamd");
    mysql_options(m, MYSQL_SET_CHARSET_NAME, "utf8mb4");
    unsigned int wartezeit = 5;
    mysql_options(m, MYSQL_OPT_CONNECT_TIMEOUT, &wartezeit);

    /* Benutzer NULL = Name des Linux-Benutzers (unix_socket) */
    if (!mysql_real_connect(m, NULL, NULL, NULL, DB_NAME, 0, NULL, 0)) {
        if (!fehler_gemeldet)
            fprintf(stderr, "streamd: Datenbank nicht erreichbar: %s - "
                    "Werte bleiben gepuffert, neuer Versuch jede Sekunde\n",
                    mysql_error(m));
        fehler_gemeldet = 1;
        mysql_close(m);
        naechster_versuch = jetzt_us() + NEU_VERBINDEN_MS * 1000LL;
        return -1;
    }
    fprintf(stderr, "streamd: mit Datenbank %s verbunden (MariaDB %s)\n",
            DB_NAME, mysql_get_server_info(m));
    fehler_gemeldet = 0;
    db = m;
    return 0;
}

static void db_trennen(void)
{
    if (db)
        mysql_close(db);
    db = NULL;
    naechster_versuch = jetzt_us() + NEU_VERBINDEN_MS * 1000LL;
}

/* Gesammelte Werte als EIN INSERT (eine Transaktion) schreiben.
   0 = geschrieben, -1 = Werte behalten und spaeter erneut versuchen. */
static int db_schreiben(char zeit[][STEMPEL_LEN],
                        char wert[][LINE_MAX_LEN], size_t n)
{
    if (n == 0)
        return 0;
    if (db_verbinden() != 0)
        return -1;

    char *p = sql;
    p += sprintf(p, "INSERT INTO werte (zeit, wert) VALUES ");
    for (size_t i = 0; i < n; i++) {
        p += sprintf(p, "%s('%s','", i ? "," : "", zeit[i]);
        p += mysql_real_escape_string(db, p, wert[i], strlen(wert[i]));
        *p++ = '\'';
        *p++ = ')';
    }
    if (mysql_real_query(db, sql, (unsigned long)(p - sql)) != 0) {
        fprintf(stderr, "streamd: INSERT: %s\n", mysql_error(db));
        db_trennen();
        return -1;
    }
    return 0;
}

/* Aelteste Eintraege entfernen, damit die Tabelle nicht unbegrenzt
   waechst. Bei 100 Werten/s ist DB_MAX_ZEILEN nach gut 16 Minuten
   erreicht — danach laeuft sie als Ringpuffer. */
static void db_kuerzen(void)
{
    if (!db || mysql_query(db, "SELECT MAX(id) FROM werte") != 0)
        return;
    MYSQL_RES *r = mysql_store_result(db);
    if (!r)
        return;
    MYSQL_ROW z = mysql_fetch_row(r);
    unsigned long long max_id = (z && z[0]) ? strtoull(z[0], NULL, 10) : 0;
    mysql_free_result(r);
    if (max_id <= DB_MAX_ZEILEN)
        return;
    char abfrage[128];
    snprintf(abfrage, sizeof(abfrage), "DELETE FROM werte WHERE id <= %llu",
             max_id - DB_MAX_ZEILEN);
    if (mysql_query(db, abfrage) != 0)
        fprintf(stderr, "streamd: kuerzen: %s\n", mysql_error(db));
}

int main(int argc, char **argv)
{
    int port = (argc > 1) ? atoi(argv[1]) : PORT_DEFAULT;
    int rate = (argc > 2) ? atoi(argv[2]) : RATE_DEFAULT;
    if (rate < 1 || rate > 1000) {
        fprintf(stderr, "streamd: Transaktionen pro Sekunde 1..1000\n");
        return 2;
    }
    const long long intervall = 1000000LL / rate;

    signal(SIGINT, beenden);
    signal(SIGTERM, beenden);
    signal(SIGPIPE, SIG_IGN);

    /* je Wert hoechstens: Zeitstempel, maskierter Text (doppelte Laenge)
       und Klammern/Anfuehrungszeichen */
    sql = malloc((size_t)PENDING_MAX * (STEMPEL_LEN + 2 * LINE_MAX_LEN + 16) + 64);
    if (!sql || mysql_library_init(0, NULL, NULL) != 0) {
        fprintf(stderr, "streamd: Initialisierung fehlgeschlagen\n");
        return 1;
    }
    db_verbinden();

    int lauscher = socket(AF_INET, SOCK_STREAM, 0);
    if (lauscher < 0) {
        perror("socket");
        return 1;
    }
    int eins = 1;
    setsockopt(lauscher, SOL_SOCKET, SO_REUSEADDR, &eins, sizeof(eins));

    struct sockaddr_in adr;
    memset(&adr, 0, sizeof(adr));
    adr.sin_family = AF_INET;
    adr.sin_addr.s_addr = htonl(INADDR_ANY);
    adr.sin_port = htons(port);
    if (bind(lauscher, (struct sockaddr *)&adr, sizeof(adr)) < 0) {
        perror("bind");
        return 1;
    }
    if (listen(lauscher, MAX_CLIENTS) < 0) {
        perror("listen");
        return 1;
    }
    fprintf(stderr, "streamd: lausche auf Port %d, Datenbank %s, %d "
            "Transaktionen/s\n", port, DB_NAME, rate);

    struct pollfd fds[MAX_CLIENTS + 1];
    char rest[MAX_CLIENTS][LINE_MAX_LEN];
    size_t restlen[MAX_CLIENTS];
    memset(restlen, 0, sizeof(restlen));

    fds[0].fd = lauscher;
    fds[0].events = POLLIN;
    for (int i = 1; i <= MAX_CLIENTS; i++)
        fds[i].fd = -1;

    /* Sammelpuffer: zwischen zwei Transaktionen gepufferte Werte */
    static char puf_zeit[PENDING_MAX][STEMPEL_LEN];
    static char puf_wert[PENDING_MAX][LINE_MAX_LEN];
    size_t offen = 0;

    long long letzter_flush = jetzt_us();
    long long letztes_kuerzen = letzter_flush;
    unsigned long gesamt = 0, verworfen = 0;

    while (laeuft) {
        /* Warten bis zum naechsten Schreibzeitpunkt, ohne offene Werte
           hoechstens 100 ms */
        int warte_ms = 100;
        if (offen > 0) {
            long long rest_us = letzter_flush + intervall - jetzt_us();
            warte_ms = rest_us > 0 ? (int)((rest_us + 999) / 1000) : 0;
        }
        int n = poll(fds, MAX_CLIENTS + 1, warte_ms);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            perror("poll");
            break;
        }

        /* neue Verbindung */
        if (fds[0].revents & POLLIN) {
            int c = accept(lauscher, NULL, NULL);
            if (c >= 0) {
                int platz = -1;
                for (int i = 1; i <= MAX_CLIENTS; i++)
                    if (fds[i].fd < 0) { platz = i; break; }
                if (platz < 0) {
                    close(c);          /* zu viele Sender */
                } else {
                    setsockopt(c, IPPROTO_TCP, TCP_NODELAY, &eins, sizeof(eins));
                    fds[platz].fd = c;
                    fds[platz].events = POLLIN;
                    restlen[platz - 1] = 0;
                }
            }
        }

        /* Daten der Sender */
        for (int i = 1; i <= MAX_CLIENTS; i++) {
            if (fds[i].fd < 0 || !(fds[i].revents & (POLLIN | POLLHUP | POLLERR)))
                continue;

            char puffer[RX_BUF];
            ssize_t gelesen = read(fds[i].fd, puffer, sizeof(puffer));
            if (gelesen <= 0) {
                close(fds[i].fd);
                fds[i].fd = -1;
                restlen[i - 1] = 0;
                continue;
            }

            for (ssize_t k = 0; k < gelesen; k++) {
                char z = puffer[k];
                if (z == '\n') {
                    size_t len = restlen[i - 1];
                    /* \r von Windows-Sendern und Leerzeichen entfernen */
                    while (len > 0 && (rest[i - 1][len - 1] == '\r' ||
                                       rest[i - 1][len - 1] == ' '))
                        len--;
                    rest[i - 1][len] = '\0';

                    if (len > 0) {
                        char stempel[STEMPEL_LEN];
                        zeitstempel(stempel, sizeof(stempel));
                        latest_schreiben(stempel, rest[i - 1]);
                        gesamt++;

                        /* fuer die Datenbank sammeln statt sofort schreiben.
                           memcpy statt snprintf: die Laenge ist bekannt
                           (len < LINE_MAX_LEN, oben begrenzt), und der
                           Compiler kann das Abschneiden sonst nicht
                           ausschliessen. Ist der Puffer voll (Datenbank
                           laenger weg), gehen neue Werte nur in latest.txt. */
                        if (offen < PENDING_MAX) {
                            size_t kopie = (len < LINE_MAX_LEN - 1)
                                           ? len : LINE_MAX_LEN - 1;
                            memcpy(puf_zeit[offen], stempel, STEMPEL_LEN);
                            puf_zeit[offen][STEMPEL_LEN - 1] = '\0';
                            memcpy(puf_wert[offen], rest[i - 1], kopie);
                            puf_wert[offen][kopie] = '\0';
                            offen++;
                        } else if (verworfen++ % 1000 == 0) {
                            fprintf(stderr, "streamd: Puffer voll, %lu Werte "
                                    "nicht in der Datenbank\n", verworfen);
                        }
                    }
                    restlen[i - 1] = 0;
                } else if (restlen[i - 1] < LINE_MAX_LEN - 1) {
                    rest[i - 1][restlen[i - 1]++] = z;
                }
                /* zu lange Zeilen werden am Rand abgeschnitten */
            }
        }

        /* gesammelte Werte gebuendelt in die Datenbank */
        long long jetzt = jetzt_us();
        if (offen > 0 && (jetzt - letzter_flush >= intervall ||
                          offen >= PENDING_MAX / 2)) {
            if (db_schreiben(puf_zeit, puf_wert, offen) == 0)
                offen = 0;
            letzter_flush = jetzt;
        }
        if (jetzt - letztes_kuerzen >= KUERZEN_MS * 1000LL) {
            db_kuerzen();
            letztes_kuerzen = jetzt;
        }
    }

    /* beim Beenden den Rest noch sichern */
    naechster_versuch = 0;
    db_schreiben(puf_zeit, puf_wert, offen);

    if (db)
        mysql_close(db);
    mysql_library_end();
    for (int i = 1; i <= MAX_CLIENTS; i++)
        if (fds[i].fd >= 0)
            close(fds[i].fd);
    close(lauscher);
    fprintf(stderr, "streamd: beendet, %lu Werte empfangen\n", gesamt);
    return 0;
}
