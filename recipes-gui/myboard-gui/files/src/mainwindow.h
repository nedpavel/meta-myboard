/* mainwindow.h — Bahn-Bediendisplay mit Unterseiten
 *
 * Hauptmenü plus umschaltbare Unterseiten (Zugsübersicht,
 * Prozesswert, Zustandsdaten, DDS-Speicher) via QStackedWidget.
 *
 * Bedienung: Touch/Maus (Buttons antippen) ODER Hardware-Tasten
 * (1-4 öffnen die Seiten, "C" führt zurück zum Hauptmenü).
 */

#ifndef MAINWINDOW_H
#define MAINWINDOW_H

#include <QWidget>
#include <QStringList>

class QLabel;
class QFrame;
class QTimer;
class QStackedWidget;
class QKeyEvent;

class MainWindow : public QWidget
{
    Q_OBJECT

public:
    explicit MainWindow(QWidget *parent = nullptr);

protected:
    void keyPressEvent(QKeyEvent *event) override;   /* Hardware-Tasten */

private slots:
    void updateClock();          /* Uhr in der Kopfzeile aktualisieren */
    void openPage();             /* Unterseite öffnen (von Button)      */
    void backToMenu();           /* zurück zum Hauptmenü                */

private:
    QWidget *buildHeader();      /* Kopfzeile: Datum | Titel | Zuginfo  */
    QWidget *buildMainMenu();    /* Seite 0: Hauptmenü                  */
    QWidget *buildContent();     /* leere Arbeitsfläche im Hauptmenü    */
    QWidget *buildCarRow();      /* Wagen-Symbole                       */
    QWidget *buildButtonRow();   /* Funktionsleiste unten               */
    QWidget *createSubPage(const QString &title,
                           const QString &prosa);  /* Unterseite bauen  */
    QWidget *buildUpdatePage();  /* Seite 5: App-Update ab USB-Stick    */
    void     showPage(int page); /* zentral: Seite + Titel umschalten   */
    void     updateCarSelection(); /* blauer Rand + "X" auf gewählten Wagen */

    /* --- USB-Update --- */
    void     updateUsbState();       /* 1x/s: USB-Stick da? Ja/Nein aktiv/grau */
    QString  findUsbBlockDevice();   /* /dev/sdX des USB-Sticks (leer = keiner) */
    bool     usbStickPresent();      /* USB-Stick angeschlossen?               */
    void     doUpdate();             /* .tar.gz vom Stick -> /usr/bin/myboard-gui */

    QLabel         *m_dateLabel;
    QLabel         *m_timeLabel;
    QLabel         *m_titleLabel;   /* Titel in der Kopfzeile (wechselt) */
    QLabel         *m_halbzugLabel;    /* "1. Halbzug" / "2. Halbzug"     */
    QLabel         *m_zugNumberLabel;  /* Fahrzeugnummer oben rechts      */
    QTimer         *m_clockTimer;
    QStackedWidget *m_stack;        /* schaltet zwischen den Seiten um   */
    QStringList     m_pageTitles;   /* Titel je Seitenindex (0 = Menü)   */

    /* Wagen-Auswahl in der Zugsübersicht (Pfeiltasten hoch/runter) */
    static const int CAR_COUNT = 2;
    QFrame  *m_carFrames[CAR_COUNT];   /* die zwei Wagen-Rahmen         */
    QLabel  *m_carLabels[CAR_COUNT];   /* Beschriftung je Wagen         */
    QString  m_carNumbers[CAR_COUNT];  /* Fahrzeugnummern               */
    int      m_selectedCar;            /* 0 = links, 1 = rechts         */

    /* USB-Update-Seite */
    static const int UPDATE_PAGE = 5;  /* Seitenindex der Update-Seite  */
    QTimer  *m_usbTimer;               /* prüft 1x/s auf USB-Stick       */
    QLabel  *m_jaLabel;                /* "Ja"  (über Taste 1)           */
    QLabel  *m_neinLabel;              /* "Nein" (über Taste 2)          */
    QLabel  *m_usbHint;                /* Status: USB erkannt / kein USB */
    bool     m_usbPresent;             /* aktueller USB-Zustand          */
};

#endif /* MAINWINDOW_H */
