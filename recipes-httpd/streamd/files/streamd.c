/* streamd — nimmt einen dauerhaft offenen Datenstrom per TCP entgegen.
 *
 * Hintergrund: Ueber PHP-FPM laesst sich eine stehende Verbindung nicht
 * verarbeiten — FPM uebergibt einen Request erst, wenn er vollstaendig
 * vorliegt (am Geraet geprueft; nginx selbst puffert dank
 * fastcgi_request_buffering off nicht). Fuer echte Stroeme braucht es
 * deshalb einen eigenen Dienst auf einem eigenen Port.
 *
 * Sender, einmal verbinden und laufen lassen:
 *     while true; do echo "Wert $(date +%s%3N)"; sleep 0.01; done | nc <board> 9100
 *
 * Gelesen wird zeilenweise; jede Zeile ist ein Wert. Mehrere Sender
 * gleichzeitig sind moeglich (bis MAX_CLIENTS).
 *
 * SCHREIBSTRATEGIE — wichtig bei 100 Werten/s:
 *   latest.txt  liegt im tmpfs (RAM) und wird bei JEDEM Wert aktualisiert.
 *               Die Anzeige liest immer den aktuellen Stand.
 *   werte.db    SQLite auf der CFast. Werte werden gesammelt und nur alle
 *               FLUSH_MS in EINER Transaktion geschrieben. Einzeln waeren
 *               es 100 Flash-Zugriffe pro Sekunde — das kostet Lebensdauer
 *               und Leistung. Gebuendelt ist es einer pro Sekunde.
 *
 * Datenbank abfragen:
 *     sqlite3 /var/www/localhost/data/werte.db \
 *         "SELECT zeit, wert FROM werte ORDER BY id DESC LIMIT 20;"
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
#include <sqlite3.h>

/* NICHT 9000 verwenden — darauf lauscht php-fpm (siehe php-fpm.conf:
   listen = 127.0.0.1:9000). streamd startete zuerst, belegte den Port und
   php-fpm scheiterte mit "Address already in use". */
#define PORT_DEFAULT   9100
#define MAX_CLIENTS    8
#define LINE_MAX_LEN   1024
#define RX_BUF         8192
#define STEMPEL_LEN    40

/* Pfade per -D ueberschreibbar, damit sich der Dienst ohne Systemrechte
   testen laesst. */
#ifndef PATH_LATEST
/* latest.txt im RAM (tmpfs): wird 100x/s geschrieben */
#define PATH_LATEST    "/run/myboard-stream/latest.txt"
#endif
#ifndef PATH_DB
/* Verlauf auf der CFast: gesammelt, siehe FLUSH_MS */
#define PATH_DB        "/var/www/localhost/data/werte.db"
#endif
#define FLUSH_MS       1000       /* hoechstens 1 Transaktion pro Sekunde */
#define DB_MAX_ZEILEN  100000     /* Verlauf begrenzen (Ringpuffer)       */
#define PENDING_MAX    2000       /* gesammelte Werte je Transaktion      */

static volatile sig_atomic_t laeuft = 1;
static void beenden(int sig) { (void)sig; laeuft = 0; }

/* Millisekunden seit Systemstart (monoton, springt nicht bei Zeitumstellung) */
static long long jetzt_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
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

/* --- SQLite ---------------------------------------------------------- */

static sqlite3      *db;
static sqlite3_stmt *stmt_einfuegen;

static int db_oeffnen(void)
{
    if (sqlite3_open(PATH_DB, &db) != SQLITE_OK) {
        fprintf(stderr, "streamd: %s: %s\n", PATH_DB, sqlite3_errmsg(db));
        return -1;
    }

    /* WAL schont die Karte zusaetzlich: Schreibvorgaenge gehen zuerst in
       ein fortlaufendes Journal, statt die Datenbankdatei umzuschreiben.
       synchronous=NORMAL spart je Transaktion ein fsync — bei Stromausfall
       koennen die letzten Werte fehlen, die Datenbank bleibt aber heil. */
    char *fehler = NULL;
    const char *init =
        "PRAGMA journal_mode=WAL;"
        "PRAGMA synchronous=NORMAL;"
        "CREATE TABLE IF NOT EXISTS werte ("
        "  id   INTEGER PRIMARY KEY AUTOINCREMENT,"
        "  zeit TEXT NOT NULL,"
        "  wert TEXT NOT NULL);"
        "CREATE INDEX IF NOT EXISTS werte_zeit ON werte(zeit);";
    if (sqlite3_exec(db, init, NULL, NULL, &fehler) != SQLITE_OK) {
        fprintf(stderr, "streamd: Tabelle anlegen: %s\n", fehler);
        sqlite3_free(fehler);
        return -1;
    }

    if (sqlite3_prepare_v2(db, "INSERT INTO werte (zeit, wert) VALUES (?, ?);",
                           -1, &stmt_einfuegen, NULL) != SQLITE_OK) {
        fprintf(stderr, "streamd: prepare: %s\n", sqlite3_errmsg(db));
        return -1;
    }
    return 0;
}

/* Gesammelte Werte in EINER Transaktion schreiben. */
static void db_schreiben(char zeit[][STEMPEL_LEN],
                         char wert[][LINE_MAX_LEN], size_t n)
{
    if (n == 0 || !db)
        return;

    sqlite3_exec(db, "BEGIN;", NULL, NULL, NULL);
    for (size_t i = 0; i < n; i++) {
        sqlite3_bind_text(stmt_einfuegen, 1, zeit[i], -1, SQLITE_STATIC);
        sqlite3_bind_text(stmt_einfuegen, 2, wert[i], -1, SQLITE_STATIC);
        if (sqlite3_step(stmt_einfuegen) != SQLITE_DONE)
            fprintf(stderr, "streamd: insert: %s\n", sqlite3_errmsg(db));
        sqlite3_reset(stmt_einfuegen);
    }
    sqlite3_exec(db, "COMMIT;", NULL, NULL, NULL);
}

/* Aelteste Eintraege entfernen, damit die Datenbank nicht unbegrenzt
   waechst. Bei 100 Werten/s ist DB_MAX_ZEILEN nach gut 16 Minuten
   erreicht — danach laeuft sie als Ringpuffer. */
static void db_kuerzen(void)
{
    if (!db)
        return;
    char sql[256];
    snprintf(sql, sizeof(sql),
             "DELETE FROM werte WHERE id <= "
             "(SELECT MAX(id) - %d FROM werte);", DB_MAX_ZEILEN);
    sqlite3_exec(db, sql, NULL, NULL, NULL);
}

int main(int argc, char **argv)
{
    int port = (argc > 1) ? atoi(argv[1]) : PORT_DEFAULT;

    signal(SIGINT, beenden);
    signal(SIGTERM, beenden);
    signal(SIGPIPE, SIG_IGN);

    if (db_oeffnen() != 0)
        return 1;

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
    fprintf(stderr, "streamd: lausche auf Port %d, Datenbank %s\n",
            port, PATH_DB);

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

    long long letzter_flush = jetzt_ms();
    unsigned long gesamt = 0;

    while (laeuft) {
        int n = poll(fds, MAX_CLIENTS + 1, 100);
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
                           ausschliessen. */
                        if (offen < PENDING_MAX) {
                            size_t kopie = (len < LINE_MAX_LEN - 1)
                                           ? len : LINE_MAX_LEN - 1;
                            memcpy(puf_zeit[offen], stempel, STEMPEL_LEN);
                            puf_zeit[offen][STEMPEL_LEN - 1] = '\0';
                            memcpy(puf_wert[offen], rest[i - 1], kopie);
                            puf_wert[offen][kopie] = '\0';
                            offen++;
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
        long long jetzt = jetzt_ms();
        if (offen > 0 && (jetzt - letzter_flush >= FLUSH_MS ||
                          offen >= PENDING_MAX / 2)) {
            db_schreiben(puf_zeit, puf_wert, offen);
            offen = 0;
            letzter_flush = jetzt;

            static int zaehler = 0;
            if (++zaehler >= 60) {     /* etwa minuetlich kuerzen */
                db_kuerzen();
                zaehler = 0;
            }
        }
    }

    /* beim Beenden den Rest noch sichern */
    db_schreiben(puf_zeit, puf_wert, offen);

    if (stmt_einfuegen)
        sqlite3_finalize(stmt_einfuegen);
    if (db)
        sqlite3_close(db);
    for (int i = 1; i <= MAX_CLIENTS; i++)
        if (fds[i].fd >= 0)
            close(fds[i].fd);
    close(lauscher);
    fprintf(stderr, "streamd: beendet, %lu Werte empfangen\n", gesamt);
    return 0;
}
