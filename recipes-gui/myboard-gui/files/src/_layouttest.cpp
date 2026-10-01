/* Offscreen-Layouttest: prueft, ob die Oberflaeche bei 640x480 (INC-100)
   und 1024x768 (Pixy 1000) in den Bildschirm passt. */
#include "mainwindow.h"
#include <QApplication>
#include <QDebug>
#include <QLabel>

static int fails;

static void pruefe(QApplication &app, int w, int h)
{
    MainWindow win(QSize(w, h));
    win.setGeometry(0, 0, w, h);
    win.show();
    app.processEvents();
    app.processEvents();

    const QSize ist = win.size();
    const QSize mind = win.minimumSizeHint();

    qInfo().noquote() << QString("=== %1x%2 ===").arg(w).arg(h);
    qInfo().noquote() << "  tatsaechliche Groesse :" << ist;
    qInfo().noquote() << "  minimumSizeHint       :" << mind;

    if (ist.width() != w || ist.height() != h) {
        qInfo().noquote() << "  FEHLER: Fenster fuellt den Schirm nicht";
        fails++;
    }
    if (mind.width() > w || mind.height() > h) {
        qInfo().noquote() << "  FEHLER: Layout braucht mehr Platz als vorhanden"
                          << "-> Inhalte wuerden abgeschnitten";
        fails++;

        /* Verursacher suchen: Kinder mit zu grosser Mindestbreite */
        qInfo().noquote() << "  --- Elemente mit minimumSizeHint().width() >"
                          << w << "---";
        const auto kinder = win.findChildren<QWidget *>();
        for (QWidget *k : kinder) {
            const int mw = k->minimumSizeHint().width();
            if (mw > w) {
                QString txt;
                if (auto *l = qobject_cast<QLabel *>(k))
                    txt = l->text().left(45).replace('\n', ' ');
                qInfo().noquote()
                    << QString("    %1 (%2) minW=%3  %4")
                           .arg(k->metaObject()->className())
                           .arg(k->objectName().isEmpty() ? "-" : k->objectName())
                           .arg(mw).arg(txt);
            }
        }
    } else {
        qInfo().noquote() << "  OK: passt in den Bildschirm";
    }
}

int main(int argc, char **argv)
{
    QApplication app(argc, argv);
    pruefe(app, 640, 480);     /* INC-100 */
    pruefe(app, 1024, 768);    /* Pixy 1000 */
    qInfo().noquote() << (fails ? QString("FEHLGESCHLAGEN (%1)").arg(fails)
                                : QString("ALLE TESTS OK"));
    return fails ? 1 : 0;
}
