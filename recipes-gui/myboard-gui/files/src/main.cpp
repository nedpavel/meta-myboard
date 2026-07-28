/* main.cpp — Einstiegspunkt der Kiosk-Anwendung
 *
 * Minimale Qt-Widgets-App im Vollbild zum Testen der
 * Grafikkette: X-Server, i915, Touch, Qt.
 */

#include <QApplication>
#include <QScreen>
#include "mainwindow.h"

int main(int argc, char *argv[])
{
    QApplication app(argc, argv);

    MainWindow window;

    /* Vollbild — Kiosk-Verhalten, keine Fensterdekoration.
       Wir laufen unter bare X ohne Window-Manager (xinit -> Xorg -> GUI);
       showFullScreen() setzt nur WM-Hints und wird dort NICHT umgesetzt, das
       Fenster bliebe auf seiner Size-Hint (zu klein). Darum die Bildschirm-
       geometrie explizit setzen, damit es 640x480 komplett fuellt. */
    window.setWindowFlags(Qt::FramelessWindowHint);
    window.setGeometry(app.primaryScreen()->geometry());
    window.show();

    return app.exec();
}
