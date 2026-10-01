/* main.cpp — Einstiegspunkt der Kiosk-Anwendung
 *
 * Minimale Qt-Widgets-App im Vollbild zum Testen der
 * Grafikkette: X-Server, i915, Touch, Qt.
 */

#include <QApplication>
#include <QScreen>
#include <QThread>
#include <QDebug>
#include "mainwindow.h"

/* Bildschirmgeometrie ermitteln — mit Geduld.
 *
 * Startet die Anwendung, bevor der X-Server seinen Screen fertig
 * eingerichtet hat, liefert primaryScreen() eine unbrauchbare Groesse
 * (0x0 oder ein Platzhalter). setGeometry() macht das Fenster dann
 * unsichtbar — der Bildschirm bleibt schwarz, obwohl alles laeuft.
 * Genau dieses Verhalten trat auf dem Pixy 1000 auf: ein zweiter,
 * manueller Start der Anwendung zeigte die Oberflaeche sofort.
 *
 * Deshalb hier bis zu 10 Sekunden auf eine plausible Groesse warten.
 */
static QRect wartenAufBildschirm(QApplication &app)
{
    const int MIN_KANTE = 100;   /* alles darunter ist offensichtlich falsch */

    for (int versuch = 0; versuch < 50; ++versuch) {
        QScreen *s = app.primaryScreen();
        if (s) {
            const QRect g = s->geometry();
            if (g.width() >= MIN_KANTE && g.height() >= MIN_KANTE)
                return g;
        }
        QThread::msleep(200);
    }

    /* Notnagel: lieber eine feste Groesse als ein unsichtbares Fenster. */
    qWarning("myboard-gui: keine brauchbare Bildschirmgroesse erkannt,"
             " verwende 1024x768");
    return QRect(0, 0, 1024, 768);
}

int main(int argc, char *argv[])
{
    QApplication app(argc, argv);

    /* Erst die Bildschirmgroesse abwarten, dann das Fenster bauen — die
       Oberflaeche skaliert sich danach (640x480 beim INC-100, 1024x768
       beim Pixy 1000). */
    const QRect geometrie = wartenAufBildschirm(app);
    MainWindow window(geometrie.size());

    /* Vollbild — Kiosk-Verhalten, keine Fensterdekoration.
       Wir laufen unter bare X ohne Window-Manager (xinit -> Xorg -> GUI);
       showFullScreen() setzt nur WM-Hints und wird dort NICHT umgesetzt, das
       Fenster bliebe auf seiner Size-Hint (zu klein). Darum die Bildschirm-
       geometrie explizit setzen, damit es den Schirm komplett fuellt. */
    window.setWindowFlags(Qt::FramelessWindowHint);
    window.setGeometry(geometrie);
    window.show();

    return app.exec();
}
