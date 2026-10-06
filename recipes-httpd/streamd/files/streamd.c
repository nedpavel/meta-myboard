/* streamd — nimmt einen dauerhaft offenen Datenstrom per TCP entgegen.
 *
 * Hintergrund: Ueber PHP-FPM laesst sich eine stehende Verbindung nicht
 * verarbeiten — FPM uebergibt einen Request erst, wenn er vollstaendig
 * vorliegt (am Geraet geprueft; nginx selbst puffert dank
 * fastcgi_request_buffering off nicht). Fuer echte Stroeme braucht es
 * deshalb einen eigenen Dienst auf einem eigenen Port.
 *
 * Sender, einmal verbinden und laufen lassen:
 *     while true; do echo "Wert $(date +%s%3N)"; sleep 0.01; done | nc <board> 9000
 *
 * Gelesen wird zeilenweise; jede Zeile ist ein Wert. Mehrere Sender
 * gleichzeitig sind moeglich (bis MAX_CLIENTS).
 *
 * SCHREIBSTRATEGIE — wichtig bei 100 Werten/s:
 *   latest.txt  liegt im tmpfs (RAM) und wird bei JEDEM Wert aktualisiert.
 *               Die Anzeige liest immer den aktuellen Stand.
 *   stream.log  wird gesammelt und nur alle FLUSH_MS auf die CFast
 *               geschrieben. Bei 100 Werten/s waeren Einzelschreibvorgaenge
 *               sonst 100 Flash-Zugriffe pro Sekunde — das kostet
 *               Lebensdauer und Leistung.
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

#define PORT_DEFAULT   9000
#define MAX_CLIENTS    8
#define LINE_MAX_LEN   1024
#define RX_BUF         8192

/* Pfade per -D ueberschreibbar, damit sich der Dienst ohne Systemrechte
   testen laesst. */
#ifndef PATH_LATEST
/* latest.txt im RAM (tmpfs): wird 100x/s geschrieben */
#define PATH_LATEST    "/run/myboard-stream/latest.txt"
#endif
#ifndef PATH_LOG
/* Verlauf auf der CFast: gesammelt, siehe FLUSH_MS */
#define PATH_LOG       "/var/www/localhost/data/stream.log"
#endif
#define FLUSH_MS       1000       /* Verlauf hoechstens 1x/s schreiben   */
#define LOG_MAX_LINES  5000       /* Verlauf begrenzen                   */
#define PENDING_MAX    (256 * 1024)

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

/* Verlauf kuerzen, damit die Datei nicht unbegrenzt waechst. */
static void log_kuerzen(void)
{
    FILE *f = fopen(PATH_LOG, "r");
    if (!f)
        return;
    long zeilen = 0;
    int c;
    while ((c = fgetc(f)) != EOF)
        if (c == '\n')
            zeilen++;
    if (zeilen <= LOG_MAX_LINES) {
        fclose(f);
        return;
    }

    rewind(f);
    long ueberspringen = zeilen - LOG_MAX_LINES;
    while (ueberspringen > 0 && (c = fgetc(f)) != EOF)
        if (c == '\n')
            ueberspringen--;

    char tmp[256];
    snprintf(tmp, sizeof(tmp), "%s.neu", PATH_LOG);
    FILE *n = fopen(tmp, "w");
    if (n) {
        char puffer[4096];
        size_t gelesen;
        while ((gelesen = fread(puffer, 1, sizeof(puffer), f)) > 0)
            fwrite(puffer, 1, gelesen, n);
        fclose(n);
        rename(tmp, PATH_LOG);
    }
    fclose(f);
}

int main(int argc, char **argv)
{
    int port = (argc > 1) ? atoi(argv[1]) : PORT_DEFAULT;

    signal(SIGINT, beenden);
    signal(SIGTERM, beenden);
    signal(SIGPIPE, SIG_IGN);

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
    fprintf(stderr, "streamd: lausche auf Port %d\n", port);

    struct pollfd fds[MAX_CLIENTS + 1];
    char rest[MAX_CLIENTS][LINE_MAX_LEN];
    size_t restlen[MAX_CLIENTS];
    memset(restlen, 0, sizeof(restlen));

    fds[0].fd = lauscher;
    fds[0].events = POLLIN;
    for (int i = 1; i <= MAX_CLIENTS; i++)
        fds[i].fd = -1;

    /* Sammelpuffer fuer den Verlauf */
    char *offen = malloc(PENDING_MAX);
    size_t offenlen = 0;
    if (!offen) {
        perror("malloc");
        return 1;
    }
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
                    /* \r von Windows-Sendern entfernen */
                    while (len > 0 && (rest[i - 1][len - 1] == '\r' ||
                                       rest[i - 1][len - 1] == ' '))
                        len--;
                    rest[i - 1][len] = '\0';

                    if (len > 0) {
                        char stempel[40];
                        zeitstempel(stempel, sizeof(stempel));
                        latest_schreiben(stempel, rest[i - 1]);
                        gesamt++;

                        /* Verlauf sammeln statt sofort zu schreiben */
                        int geschrieben = snprintf(offen + offenlen,
                                                   PENDING_MAX - offenlen,
                                                   "%s\t%s\n", stempel,
                                                   rest[i - 1]);
                        if (geschrieben > 0 &&
                            (size_t)geschrieben < PENDING_MAX - offenlen)
                            offenlen += (size_t)geschrieben;
                    }
                    restlen[i - 1] = 0;
                } else if (restlen[i - 1] < LINE_MAX_LEN - 1) {
                    rest[i - 1][restlen[i - 1]++] = z;
                }
                /* zu lange Zeilen werden am Rand abgeschnitten */
            }
        }

        /* Verlauf gebuendelt wegschreiben */
        long long jetzt = jetzt_ms();
        if (offenlen > 0 && (jetzt - letzter_flush >= FLUSH_MS ||
                             offenlen > PENDING_MAX / 2)) {
            FILE *f = fopen(PATH_LOG, "a");
            if (f) {
                fwrite(offen, 1, offenlen, f);
                fclose(f);
            }
            offenlen = 0;
            letzter_flush = jetzt;

            static int zaehler = 0;
            if (++zaehler >= 60) {     /* etwa minuetlich kuerzen */
                log_kuerzen();
                zaehler = 0;
            }
        }
    }

    /* beim Beenden den Rest noch sichern */
    if (offenlen > 0) {
        FILE *f = fopen(PATH_LOG, "a");
        if (f) {
            fwrite(offen, 1, offenlen, f);
            fclose(f);
        }
    }
    free(offen);
    for (int i = 1; i <= MAX_CLIENTS; i++)
        if (fds[i].fd >= 0)
            close(fds[i].fd);
    close(lauscher);
    fprintf(stderr, "streamd: beendet, %lu Werte empfangen\n", gesamt);
    return 0;
}
