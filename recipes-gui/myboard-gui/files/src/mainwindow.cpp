/* mainwindow.cpp — Bahn-Bediendisplay mit Unterseiten
 *
 * Hauptmenü mit Funktionstasten, die jeweils eine eigene
 * Unterseite öffnen (Titel, Beschreibungstext, Zurück-Knopf).
 *
 * Bedienung:
 *   - Touch/Maus: Buttons direkt antippen
 *   - Hardware:   1=Zugsübersicht, 2=Prozesswert,
 *                 3=Zustandsdaten, 4=DDS-Speicher,
 *                 "C" = zurück zum Hauptmenü
 */

#include "mainwindow.h"

#include <QLabel>
#include <QPixmap>
#include <QFrame>
#include <QSizePolicy>
#include <QPushButton>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QStackedWidget>
#include <QTimer>
#include <QDateTime>
#include <QPainter>
#include <QFont>
#include <QKeyEvent>
#include <QProcess>
#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QTableWidget>
#include <QHeaderView>
#include <QAbstractItemView>
#include "canreader.h"

/* ------------------------------------------------------------------ */
/* Farbschema — an das Original-Display angelehnt                      */
/* ------------------------------------------------------------------ */

static const char *COL_BG      = "#EDEDE6";   /* warmes Off-White      */
static const char *COL_BORDER  = "#9AA0A6";   /* graue Zelltrennlinien */
static const char *COL_TEXT    = "#2B2B2B";   /* dunkler Text          */
static const char *COL_BLUE    = "#23457E";   /* Blau für Zuginfo      */
static const char *COL_CELL    = "#F6F6F1";   /* Zellhintergrund       */

/* ------------------------------------------------------------------ */
/* Kleines Wagen-Symbol — stilisierter Triebzug-Wagen via QPainter    */
/* ------------------------------------------------------------------ */

class TrainCar : public QWidget
{
public:
    explicit TrainCar(bool selected, QWidget *parent = nullptr)
        : QWidget(parent), m_selected(selected)
    {
        setMinimumSize(80, 14);
    }

protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing, true);

        int w = width();
        int h = height();
        int margin = 6;

        QRectF body(margin, h * 0.30, w - 2 * margin, h * 0.40);
        p.setPen(QPen(QColor("#888888"), 1));
        p.setBrush(QColor("#E8E8E8"));
        p.drawRoundedRect(body, 4, 4);

        QRectF roof(margin, h * 0.26, w - 2 * margin, h * 0.06);
        p.setBrush(QColor("#C0392B"));
        p.setPen(Qt::NoPen);
        p.drawRect(roof);

        p.setPen(QPen(QColor("#5A7A9A"), 1));
        int winCount = 14;
        double step = (w - 2 * margin - 16) / (double)winCount;
        for (int i = 0; i < winCount; i++) {
            double x = margin + 12 + i * step;
            p.drawLine(QPointF(x, h * 0.40), QPointF(x, h * 0.52));
        }

        p.setBrush(QColor("#555555"));
        p.setPen(Qt::NoPen);
        double wheelY = h * 0.70;
        p.drawEllipse(QPointF(w * 0.25, wheelY), 4, 4);
        p.drawEllipse(QPointF(w * 0.75, wheelY), 4, 4);
    }

private:
    bool m_selected;
};

/* ------------------------------------------------------------------ */
/* Konstruktor                                                          */
/* ------------------------------------------------------------------ */

MainWindow::MainWindow(QWidget *parent)
    : QWidget(parent), m_selectedCar(0), m_usbPresent(false)
{
    setStyleSheet(QString("background-color: %1;").arg(COL_BG));

    /* Fenster nimmt Tastatur-Eingaben entgegen */
    setFocusPolicy(Qt::StrongFocus);

    /* CAN-Matrix (Signaltabelle) aus der eingebetteten Ressource laden —
       liefert die Dekodierung fuer die Prozesswert-Seite. */
    m_canMatrix.loadFromResource(QStringLiteral(":/can/can_matrix.tsv"));

    QVBoxLayout *root = new QVBoxLayout(this);
    root->setContentsMargins(2, 2, 2, 2);
    root->setSpacing(0);

    /* Kopfzeile bleibt immer sichtbar */
    root->addWidget(buildHeader());

    /* Stapel: Seite 0 = Hauptmenü, danach die Unterseiten */
    m_stack = new QStackedWidget();
    root->addWidget(m_stack, 1);

    /* Seite 0 — das Hauptmenü */
    m_stack->addWidget(buildMainMenu());
    m_pageTitles << "Hauptmenü";   /* Index 0 */

    /* Unterseiten — Reihenfolge bestimmt den Index (1..4)
       und muss zu den Buttons und F-Tasten passen. */
    struct { const char *title; const char *prosa; } pages[] = {
        { "Zugsübersicht",
          "Auf dieser Seite wird später die Zusammensetzung des Zuges "
          "dargestellt: alle Wagen des Halbzugs mit ihrer Fahrzeugnummer, "
          "Position im Verband, Fahrtrichtung sowie dem aktuellen "
          "Betriebszustand jedes Fahrzeugs." },
        { "Prozesswert",
          "Hier werden später die Live-Prozesswerte aus dem CAN-Bus "
          "angezeigt: Geschwindigkeit, Drehzahl, Drücke, Temperaturen und "
          "Statusbits der Geräte (ATESS, ULG, SR1A). Die Signale stammen "
          "aus der CAN-Matrix und werden laufend aktualisiert." },
        { "Zustandsdaten",
          "Diese Seite zeigt später die Diagnose- und Zustandsdaten der "
          "Fahrzeuge: Warnungen, Fehlermeldungen, Übertemperaturen und "
          "den Status der einzelnen Stromrichter und Baugruppen." },
        { "DDS-Speicher",
          "Hier wird später der Inhalt des DDS-Speichers (Diagnose-Daten-"
          "Speicher) aufgelistet: gespeicherte Ereignisse, Zeitstempel und "
          "abrufbare Aufzeichnungen für die Wartung." },
    };

    for (const auto &pg : pages) {
        if (QString::fromUtf8(pg.title) == QStringLiteral("Prozesswert"))
            m_stack->addWidget(buildProzesswertPage());   /* Live-CAN-Werte */
        else
            m_stack->addWidget(createSubPage(pg.title, pg.prosa));
        m_pageTitles << pg.title;
    }

    /* Seite 5 — App-Update ab USB-Stick (Index UPDATE_PAGE) */
    m_stack->addWidget(buildUpdatePage());
    m_pageTitles << "Update App";   /* Index 5 */

    setLayout(root);

    /* Uhr jede Sekunde aktualisieren */
    m_clockTimer = new QTimer(this);
    connect(m_clockTimer, &QTimer::timeout, this, &MainWindow::updateClock);
    m_clockTimer->start(1000);
    updateClock();

    /* USB-Timer (läuft nur, solange die Update-Seite offen ist) */
    m_usbTimer = new QTimer(this);
    connect(m_usbTimer, &QTimer::timeout, this, &MainWindow::updateUsbState);

    /* CAN-Empfang auf can0 starten — dekodierte Werte landen live auf
       der Prozesswert-Seite (Seite 2). */
    m_canReader = new CanReader(this);
    connect(m_canReader, &CanReader::frameReceived,
            this, &MainWindow::onCanFrame);
    bool canOk = m_canReader->open(QStringLiteral("can0"));
    if (m_pwStatus) {
        if (canOk)
            m_pwStatus->setText(
                QString("CAN: can0 offen · %1 Signale in der Matrix · warte auf Telegramme …")
                    .arg(m_canMatrix.signalCount()));
        else
            m_pwStatus->setText(
                "CAN: can0 nicht verfügbar — Interface hochfahren: "
                "'ip link set can0 up type can bitrate 250000'");
    }
}

/* ------------------------------------------------------------------ */
/* Kopfzeile                                                            */
/* ------------------------------------------------------------------ */

QWidget *MainWindow::buildHeader()
{
    QFrame *header = new QFrame();
    header->setStyleSheet(
        QString("QFrame { background-color: %1; border: 1px solid %2; }")
            .arg(COL_CELL).arg(COL_BORDER));
    header->setFixedHeight(47);          /* ~2/3 der vorherigen Höhe (70) */

    QHBoxLayout *lay = new QHBoxLayout(header);
    lay->setContentsMargins(0, 0, 0, 0);
    lay->setSpacing(0);

    QFrame *dateBox = new QFrame();
    dateBox->setStyleSheet(
        QString("border-right: 1px solid %1;").arg(COL_BORDER));
    dateBox->setFixedWidth(180);
    QVBoxLayout *dateLay = new QVBoxLayout(dateBox);
    dateLay->setContentsMargins(12, 3, 12, 3);
    dateLay->setSpacing(0);

    m_dateLabel = new QLabel("01.02.86");
    m_timeLabel = new QLabel("01:49:30");
    QFont dtFont; dtFont.setPointSize(10);
    m_dateLabel->setFont(dtFont);
    m_timeLabel->setFont(dtFont);
    m_dateLabel->setStyleSheet(
        QString("color: %1; border: none;").arg(COL_TEXT));
    m_timeLabel->setStyleSheet(
        QString("color: %1; border: none;").arg(COL_TEXT));
    m_dateLabel->setAlignment(Qt::AlignCenter);
    m_timeLabel->setAlignment(Qt::AlignCenter);
    dateLay->addWidget(m_dateLabel);
    dateLay->addWidget(m_timeLabel);

    /* Titel — wird beim Seitenwechsel aktualisiert */
    m_titleLabel = new QLabel("Hauptmenü");
    QFont titleFont; titleFont.setPointSize(13);
    m_titleLabel->setFont(titleFont);
    m_titleLabel->setAlignment(Qt::AlignCenter);
    m_titleLabel->setStyleSheet(
        QString("color: %1; border: none; border-right: 1px solid %2;")
            .arg(COL_TEXT).arg(COL_BORDER));

    QFrame *infoBox = new QFrame();
    infoBox->setStyleSheet("border: none;");
    infoBox->setFixedWidth(220);
    QVBoxLayout *infoLay = new QVBoxLayout(infoBox);
    infoLay->setContentsMargins(12, 2, 12, 2);
    infoLay->setSpacing(0);

    m_halbzugLabel   = new QLabel("1. Halbzug");
    m_zugNumberLabel = new QLabel("1 500 012-3");
    QLabel *l3 = new QLabel("mm:ss");
    QFont infoFont; infoFont.setPointSize(9); infoFont.setBold(true);
    for (QLabel *l : { m_halbzugLabel, m_zugNumberLabel, l3 }) {
        l->setFont(infoFont);
        l->setAlignment(Qt::AlignCenter);
        l->setStyleSheet(
            QString("color: %1; border: none;").arg(COL_BLUE));
    }
    infoLay->addWidget(m_halbzugLabel);
    infoLay->addWidget(m_zugNumberLabel);
    infoLay->addWidget(l3);

    lay->addWidget(dateBox);
    lay->addWidget(m_titleLabel, 1);
    lay->addWidget(infoBox);

    return header;
}

/* ------------------------------------------------------------------ */
/* Seite 0 — das Hauptmenü                                             */
/* ------------------------------------------------------------------ */

QWidget *MainWindow::buildMainMenu()
{
    QWidget *page = new QWidget();
    QVBoxLayout *lay = new QVBoxLayout(page);
    lay->setContentsMargins(0, 0, 0, 0);
    lay->setSpacing(0);

    lay->addWidget(buildContent(), 1);
    lay->addWidget(buildCarRow());
    lay->addWidget(buildButtonRow());

    return page;
}

/* ------------------------------------------------------------------ */
/* Arbeitsfläche — leer                                                */
/* ------------------------------------------------------------------ */

QWidget *MainWindow::buildContent()
{
    QFrame *content = new QFrame();
    content->setStyleSheet(
        QString("QFrame { background-color: %1; "
                "border-left: 1px solid %2; "
                "border-right: 1px solid %2; }")
            .arg(COL_BG).arg(COL_BORDER));
    return content;
}

/* ------------------------------------------------------------------ */
/* Wagen-Reihe                                                          */
/* ------------------------------------------------------------------ */

QWidget *MainWindow::buildCarRow()
{
    QFrame *row = new QFrame();
    row->setStyleSheet(
        QString("QFrame { border-left: 1px solid %1; "
                "border-right: 1px solid %1; }").arg(COL_BORDER));
    row->setFixedHeight(32);          /* ~halbe Höhe wie vorher (64) */

    QHBoxLayout *lay = new QHBoxLayout(row);
    lay->setContentsMargins(0, 1, 0, 1);   /* linksbündig zur Buttonleiste */
    lay->setSpacing(0);

    m_carNumbers[0] = "1 500 012-3";
    m_carNumbers[1] = "2 500 012-1";

    /* Fotos der Halbzüge (als Qt-Ressource ins Binary eingebettet, siehe
       images.qrc) statt der früheren schematischen TrainCar-Zeichnung. */
    static const char *carImages[CAR_COUNT] = {
        ":/img/ICN_Office.jpg",
        ":/img/ICN_Office_2.jpg",
    };

    QFont carFont; carFont.setPointSize(7);   /* passt in die buttonbündige Zelle */

    for (int i = 0; i < CAR_COUNT; i++) {
        QFrame *car = new QFrame();
        QVBoxLayout *c = new QVBoxLayout(car);
        c->setContentsMargins(1, 0, 1, 0);
        c->setSpacing(0);

        QLabel *img = new QLabel();
        img->setPixmap(QPixmap(carImages[i]));
        img->setScaledContents(true);   /* füllt die (breite, flache) Zelle */
        img->setStyleSheet("border: none;");
        /* Ignored: das Bild-Label soll NICHT seine 1024px-Bildbreite als
           Wunschgröße erzwingen (das würde das GUI horizontal aufblähen).
           Es skaliert stattdessen in den per Stretch vorgegebenen Platz. */
        img->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Ignored);
        img->setMinimumSize(0, 0);
        c->addWidget(img, 1);

        QLabel *lab = new QLabel();
        lab->setAlignment(Qt::AlignCenter);
        lab->setFont(carFont);
        lab->setStyleSheet(
            QString("color: %1; border: none;").arg(COL_TEXT));
        /* Der Nummerntext darf die Zellbreite NICHT erzwingen — sonst wird
           die selektierte Zelle breiter und die Bilder sitzen nicht mehr
           bündig über den Buttonpaaren. Ignored -> Zelle = Stretch-Anteil. */
        lab->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
        lab->setMinimumWidth(0);
        c->addWidget(lab);

        m_carFrames[i] = car;
        m_carLabels[i] = lab;
        /* Stretch 2 = zwei Button-Spalten breit; so liegt Halbzug 1 über
           "Zugsübersicht"+"Prozesswert", Halbzug 2 über "Zustandsdaten"+
           "DDS-Speicher" (die Buttonleiste hat 10 gleiche Spalten). */
        lay->addWidget(car, 2);
    }

    /* Die restlichen 6 Button-Spalten bleiben frei. */
    lay->addStretch(6);

    /* Anfangszustand: blauer Rand + "X" gemäß m_selectedCar setzen */
    updateCarSelection();

    return row;
}

/* ------------------------------------------------------------------ */
/* Wagen-Auswahl aktualisieren — blauer Rand + "X" wandern mit         */
/* ------------------------------------------------------------------ */

void MainWindow::updateCarSelection()
{
    for (int i = 0; i < CAR_COUNT; i++) {
        bool sel = (i == m_selectedCar);
        m_carFrames[i]->setStyleSheet(
            QString("border: %1px solid %2;")
                .arg(sel ? 2 : 1)
                .arg(sel ? COL_BLUE : COL_BORDER));
        /* Auswahl-Anzeige: blauer Rahmen (wandert mit Pfeiltasten) + Fettschrift.
           Kein "X"-Textpräfix mehr — das würde die Zelle über die Buttonbreite
           hinaus dehnen und die Ausrichtung zerstören. */
        m_carLabels[i]->setStyleSheet(
            QString("color: %1; border: none; font-weight: %2;")
                .arg(COL_TEXT).arg(sel ? "bold" : "normal"));
        m_carLabels[i]->setText(m_carNumbers[i]);
    }

    /* Zuginfo oben rechts mitführen: Halbzug-Nummer + Fahrzeugnummer */
    m_halbzugLabel->setText(QString("%1. Halbzug").arg(m_selectedCar + 1));
    m_zugNumberLabel->setText(m_carNumbers[m_selectedCar]);
}

/* ------------------------------------------------------------------ */
/* Funktionsleiste unten                                                */
/* ------------------------------------------------------------------ */

QWidget *MainWindow::buildButtonRow()
{
    QFrame *row = new QFrame();
    row->setStyleSheet(
        QString("QFrame { border: 1px solid %1; }").arg(COL_BORDER));
    row->setFixedHeight(43);          /* ~2/3 der vorherigen Höhe (64) */

    QHBoxLayout *lay = new QHBoxLayout(row);
    lay->setContentsMargins(0, 0, 0, 0);
    lay->setSpacing(0);

    QString btnStyle = QString(
        "QPushButton {"
        "  background-color: %1;"
        "  color: %2;"
        "  border: 1px solid %3;"
        "  font-size: 10px;"
        "  padding: 0px;"
        "}"
        "QPushButton:pressed {"
        "  background-color: %4;"
        "  color: white;"
        "}")
        .arg(COL_CELL).arg(COL_TEXT).arg(COL_BORDER).arg(COL_BLUE);

    /* Diese vier Tasten öffnen Unterseiten.
       pageIndex = Seitenindex im Stack (1..4), passend zu den Hardware-
       Tasten 1-4 (die Ziffer steht bewusst nicht mehr in der Beschriftung). */
    struct { QString text; int page; } funcs[] = {
        { "Zugs-\nübersicht", 1 },
        { "Prozess-\nwert",   2 },
        { "Zustands-\ndaten", 3 },
        { "DDS -\nSpeicher",  4 },
        { "Update\nApp",      UPDATE_PAGE },  /* Taste 5 */
    };

    for (const auto &f : funcs) {
        QPushButton *btn = new QPushButton(f.text);
        btn->setStyleSheet(btnStyle);
        btn->setMinimumWidth(0);
        btn->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Expanding);
        btn->setProperty("pageIndex", f.page);
        /* Buttons sollen den Tastatur-Fokus NICHT abfangen,
           damit F-Tasten/C immer beim Hauptfenster ankommen. */
        btn->setFocusPolicy(Qt::NoFocus);
        connect(btn, &QPushButton::clicked, this, &MainWindow::openPage);
        lay->addWidget(btn, 1);
    }

    /* Es gibt jetzt 5 belegte Funktionstasten (1..5), daher nur noch 3 leere. */
    for (int i = 0; i < 3; i++) {
        QPushButton *btn = new QPushButton("");
        btn->setStyleSheet(btnStyle);
        btn->setMinimumWidth(0);
        btn->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Expanding);
        btn->setFocusPolicy(Qt::NoFocus);
        lay->addWidget(btn, 1);
    }

    QPushButton *sBtn = new QPushButton("S");
    sBtn->setStyleSheet(btnStyle);
    sBtn->setMinimumWidth(0);
    sBtn->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Expanding);
    sBtn->setFocusPolicy(Qt::NoFocus);
    lay->addWidget(sBtn, 1);

    QPushButton *driver = new QPushButton("LF");
    driver->setStyleSheet(btnStyle);
    driver->setMinimumWidth(0);
    driver->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Expanding);
    driver->setFocusPolicy(Qt::NoFocus);
    lay->addWidget(driver, 1);

    return row;
}

/* ------------------------------------------------------------------ */
/* Unterseite bauen — Titel, Prosa, Zurück-Knopf                       */
/* ------------------------------------------------------------------ */

QWidget *MainWindow::createSubPage(const QString &title,
                                   const QString &prosa)
{
    QFrame *page = new QFrame();
    page->setStyleSheet(
        QString("QFrame { background-color: %1; border: 1px solid %2; }")
            .arg(COL_BG).arg(COL_BORDER));

    QVBoxLayout *lay = new QVBoxLayout(page);
    lay->setContentsMargins(24, 24, 24, 24);
    lay->setSpacing(20);

    QLabel *titleLabel = new QLabel(title);
    QFont tf; tf.setPointSize(22); tf.setBold(true);
    titleLabel->setFont(tf);
    titleLabel->setStyleSheet(
        QString("color: %1; border: none;").arg(COL_BLUE));
    lay->addWidget(titleLabel);

    QFrame *line = new QFrame();
    line->setFrameShape(QFrame::HLine);
    line->setStyleSheet(QString("color: %1;").arg(COL_BORDER));
    lay->addWidget(line);

    QLabel *prose = new QLabel(prosa);
    prose->setWordWrap(true);
    QFont pf; pf.setPointSize(14);
    prose->setFont(pf);
    prose->setStyleSheet(
        QString("color: %1; border: none;").arg(COL_TEXT));
    prose->setAlignment(Qt::AlignTop | Qt::AlignLeft);
    lay->addWidget(prose);

    QLabel *hint = new QLabel("(Diese Seite ist noch leer — Inhalte folgen.)");
    hint->setWordWrap(true);   /* sonst erzwingt der Text >640px Fensterbreite */
    QFont hf; hf.setPointSize(12); hf.setItalic(true);
    hint->setFont(hf);
    hint->setStyleSheet("color: #888888; border: none;");
    lay->addWidget(hint);

    lay->addStretch();

    /* Zurück-Knopf — Beschriftung weist auf die "C"-Taste hin */
    QPushButton *back = new QPushButton("◄  Zurück  (C)");
    back->setStyleSheet(QString(
        "QPushButton {"
        "  background-color: %1;"
        "  color: %2;"
        "  border: 1px solid %3;"
        "  font-size: 16px;"
        "  padding: 10px 24px;"
        "}"
        "QPushButton:pressed {"
        "  background-color: %4;"
        "  color: white;"
        "}")
        .arg(COL_CELL).arg(COL_TEXT).arg(COL_BORDER).arg(COL_BLUE));
    back->setFixedWidth(220);
    back->setFocusPolicy(Qt::NoFocus);
    connect(back, &QPushButton::clicked, this, &MainWindow::backToMenu);

    QHBoxLayout *backRow = new QHBoxLayout();
    backRow->addWidget(back);
    backRow->addStretch();
    lay->addLayout(backRow);

    return page;
}

/* ------------------------------------------------------------------ */
/* Prozesswert-Seite — Live-CAN-Werte, dekodiert via CanMatrix          */
/* ------------------------------------------------------------------ */

QWidget *MainWindow::buildProzesswertPage()
{
    QFrame *page = new QFrame();
    page->setStyleSheet(
        QString("QFrame { background-color: %1; border: 1px solid %2; }")
            .arg(COL_BG).arg(COL_BORDER));

    QVBoxLayout *lay = new QVBoxLayout(page);
    lay->setContentsMargins(8, 6, 8, 6);
    lay->setSpacing(5);

    QLabel *titleLabel = new QLabel("Prozesswerte (CAN live)");
    QFont tf; tf.setPointSize(14); tf.setBold(true);
    titleLabel->setFont(tf);
    titleLabel->setStyleSheet(QString("color: %1; border: none;").arg(COL_BLUE));
    lay->addWidget(titleLabel);

    m_pwStatus = new QLabel("CAN: initialisiere …");
    m_pwStatus->setWordWrap(true);
    QFont sf; sf.setPointSize(9);
    m_pwStatus->setFont(sf);
    m_pwStatus->setStyleSheet("color: #888888; border: none;");
    lay->addWidget(m_pwStatus);

    /* Live-Tabelle: eine Zeile je Signal, Wert wird in-place aktualisiert. */
    m_pwTable = new QTableWidget(0, 4);
    QStringList headers;
    headers << "CAN-ID" << "Signal" << "Wert" << "Einh.";
    m_pwTable->setHorizontalHeaderLabels(headers);
    m_pwTable->verticalHeader()->setVisible(false);
    m_pwTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_pwTable->setSelectionMode(QAbstractItemView::NoSelection);
    m_pwTable->setFocusPolicy(Qt::NoFocus);
    QFont tbf; tbf.setPointSize(9);
    m_pwTable->setFont(tbf);
    m_pwTable->horizontalHeader()->setFont(tbf);
    m_pwTable->horizontalHeader()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    m_pwTable->horizontalHeader()->setSectionResizeMode(1, QHeaderView::Stretch);
    m_pwTable->horizontalHeader()->setSectionResizeMode(2, QHeaderView::ResizeToContents);
    m_pwTable->horizontalHeader()->setSectionResizeMode(3, QHeaderView::ResizeToContents);
    m_pwTable->setStyleSheet(
        "QTableWidget { background-color: white; color: #202020; border: 1px solid #999; }"
        "QHeaderView::section { background-color: #E0E0E0; color: #202020; padding: 1px; }");
    lay->addWidget(m_pwTable, 1);

    QPushButton *back = new QPushButton("◄  Zurück  (C)");
    back->setStyleSheet(QString(
        "QPushButton {"
        "  background-color: %1;"
        "  color: %2;"
        "  border: 1px solid %3;"
        "  font-size: 13px;"
        "  padding: 5px 18px;"
        "}"
        "QPushButton:pressed { background-color: %4; color: white; }")
        .arg(COL_CELL).arg(COL_TEXT).arg(COL_BORDER).arg(COL_BLUE));
    back->setFocusPolicy(Qt::NoFocus);
    connect(back, &QPushButton::clicked, this, &MainWindow::backToMenu);

    QHBoxLayout *backRow = new QHBoxLayout();
    backRow->addWidget(back);
    backRow->addStretch();
    lay->addLayout(backRow);

    return page;
}

/* Ein empfangenes CAN-Telegramm dekodieren und die Tabelle aktualisieren.
   Jedes Signal bekommt eine feste Zeile (per Name); der Wert wird in-place
   ueberschrieben, damit die Anzeige ruhig bleibt und nicht waechst. */
void MainWindow::onCanFrame(quint32 canId, const QByteArray &data)
{
    ++m_pwFrames;
    if (!m_pwTable)
        return;

    const QVector<CanDecoded> sigs = m_canMatrix.decode(canId, data);

    for (const CanDecoded &d : sigs) {
        QString valStr;
        if (d.isBool) {
            valStr = d.raw ? QStringLiteral("1") : QStringLiteral("0");
        } else if (qAbs(d.value - qRound64(d.value)) < 1e-9) {
            valStr = QString::number((qlonglong)qRound64(d.value));
        } else {
            valStr = QString::number(d.value, 'f', 2);
        }

        auto it = m_pwRows.constFind(d.name);
        if (it == m_pwRows.constEnd()) {
            int row = m_pwTable->rowCount();
            m_pwTable->insertRow(row);
            m_pwRows.insert(d.name, row);

            const QString idStr =
                QStringLiteral("0x") + QString::number(canId, 16).toUpper();
            const QString label = d.comment.isEmpty() ? d.name : d.comment;

            m_pwTable->setItem(row, 0, new QTableWidgetItem(idStr));
            QTableWidgetItem *sig = new QTableWidgetItem(label);
            sig->setToolTip(d.name);
            m_pwTable->setItem(row, 1, sig);
            m_pwTable->setItem(row, 2, new QTableWidgetItem(valStr));
            m_pwTable->setItem(row, 3, new QTableWidgetItem(d.unit));
        } else {
            m_pwTable->item(it.value(), 2)->setText(valStr);
        }
    }

    /* Status nicht bei jedem Frame neu setzen (nur alle 25). */
    if (m_pwStatus && (m_pwFrames % 25 == 0)) {
        m_pwStatus->setText(
            QString("CAN: can0 · %1 Telegramme · %2 Signale live")
                .arg(m_pwFrames).arg(m_pwRows.size()));
    }
}

/* ------------------------------------------------------------------ */
/* Zentrale Umschaltung — von Button UND Taste genutzt                 */
/* ------------------------------------------------------------------ */

void MainWindow::showPage(int page)
{
    if (page < 0 || page >= m_stack->count())
        return;

    m_stack->setCurrentIndex(page);
    m_titleLabel->setText(m_pageTitles.value(page, "Hauptmenü"));

    /* USB-Prüfung nur laufen lassen, während die Update-Seite offen ist */
    if (page == UPDATE_PAGE) {
        updateUsbState();          /* sofort einmal prüfen  */
        m_usbTimer->start(1000);   /* danach jede Sekunde   */
    } else {
        m_usbTimer->stop();
    }
}

/* ------------------------------------------------------------------ */
/* Slots                                                                */
/* ------------------------------------------------------------------ */

void MainWindow::updateClock()
{
    QDateTime now = QDateTime::currentDateTime();
    m_dateLabel->setText(now.toString("dd.MM.yy"));
    m_timeLabel->setText(now.toString("HH:mm:ss"));
}

void MainWindow::openPage()
{
    QPushButton *btn = qobject_cast<QPushButton *>(sender());
    if (!btn)
        return;
    showPage(btn->property("pageIndex").toInt());
}

void MainWindow::backToMenu()
{
    showPage(0);
}

/* ------------------------------------------------------------------ */
/* Hardware-Tasten                                                      */
/* ------------------------------------------------------------------ */

void MainWindow::keyPressEvent(QKeyEvent *event)
{
    const int key = event->key();

    /* "C"/Escape führen IMMER zurück zum Hauptmenü */
    if (key == Qt::Key_C || key == Qt::Key_Escape) {
        showPage(0);
        return;
    }

    /* --- Sonderfall Update-Seite: 1=Ja, 2=Nein (nur bei USB-Stick) --- */
    if (m_stack->currentIndex() == UPDATE_PAGE) {
        switch (key) {
        case Qt::Key_1:                 /* "Ja"  */
            if (m_usbPresent)
                doUpdate();
            break;
        case Qt::Key_2:                 /* "Nein" */
            if (m_usbPresent)
                showPage(0);
            break;
        default:
            break;                      /* sonst nichts (nur C zurück) */
        }
        return;
    }

    /* --- Hauptmenü / Unterseiten --- */
    switch (key) {
    case Qt::Key_1:
        showPage(1);
        break;
    case Qt::Key_2:
        showPage(2);
        break;
    case Qt::Key_3:
        showPage(3);
        break;
    case Qt::Key_4:
        showPage(4);
        break;
    case Qt::Key_5:                     /* Update-Seite öffnen */
        showPage(UPDATE_PAGE);
        break;
    case Qt::Key_Up:       /* Pfeil hoch = Wagen-Auswahl nach RECHTS */
        if (m_selectedCar < CAR_COUNT - 1) {
            m_selectedCar++;
            updateCarSelection();
        }
        break;
    case Qt::Key_Down:     /* Pfeil runter = Wagen-Auswahl nach LINKS */
        if (m_selectedCar > 0) {
            m_selectedCar--;
            updateCarSelection();
        }
        break;
    default:
        QWidget::keyPressEvent(event);   /* alles andere weiterreichen */
        break;
    }
}

/* ------------------------------------------------------------------ */
/* Update-Seite (Index UPDATE_PAGE) — App ab USB-Stick aktualisieren   */
/* ------------------------------------------------------------------ */

QWidget *MainWindow::buildUpdatePage()
{
    QFrame *page = new QFrame();
    page->setStyleSheet(
        QString("QFrame { background-color: %1; border: 1px solid %2; }")
            .arg(COL_BG).arg(COL_BORDER));

    QVBoxLayout *lay = new QVBoxLayout(page);
    lay->setContentsMargins(0, 0, 0, 0);
    lay->setSpacing(0);

    /* Frage — füllt den oberen Bereich */
    QLabel *q = new QLabel(
        "Soll die Applikation wirklich\nab USB-Stick upgedatet werden?");
    q->setWordWrap(true);   /* sonst erzwingt der Text >640px Fensterbreite */
    QFont qf; qf.setPointSize(16); qf.setBold(true);
    q->setFont(qf);
    q->setAlignment(Qt::AlignCenter);
    q->setStyleSheet(QString("color: %1; border: none;").arg(COL_TEXT));
    lay->addWidget(q, 1);

    /* Status-Hinweis (USB erkannt / kein USB / Update-Status) */
    m_usbHint = new QLabel();
    m_usbHint->setWordWrap(true);   /* Fehlermeldungen koennen lang sein */
    QFont hf; hf.setPointSize(11);
    m_usbHint->setFont(hf);
    m_usbHint->setAlignment(Qt::AlignCenter);
    m_usbHint->setStyleSheet("border: none;");
    lay->addWidget(m_usbHint);

    lay->addSpacing(8);

    /* Untere Leiste: "Ja" über Taste 1, "Nein" über Taste 2 — gleiche
       10-Spalten-Aufteilung wie die Funktionsleiste, damit sie über den
       Hardware-Tasten 1 und 2 liegen. */
    QFrame *row = new QFrame();
    row->setStyleSheet(
        QString("QFrame { border: 1px solid %1; }").arg(COL_BORDER));
    row->setFixedHeight(43);
    QHBoxLayout *rl = new QHBoxLayout(row);
    rl->setContentsMargins(0, 0, 0, 0);
    rl->setSpacing(0);

    m_jaLabel   = new QLabel("Ja");
    m_neinLabel = new QLabel("Nein");
    QFont bf; bf.setPointSize(13); bf.setBold(true);
    for (QLabel *l : { m_jaLabel, m_neinLabel }) {
        l->setFont(bf);
        l->setAlignment(Qt::AlignCenter);
        l->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Expanding);
    }
    rl->addWidget(m_jaLabel, 1);
    rl->addWidget(m_neinLabel, 1);

    /* die restlichen 8 Spalten bleiben leer */
    for (int i = 0; i < 8; i++) {
        QLabel *e = new QLabel();
        e->setStyleSheet(QString("border: 1px solid %1;").arg(COL_BORDER));
        e->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Expanding);
        rl->addWidget(e, 1);
    }

    lay->addWidget(row);

    return page;
}

/* Sucht das /dev/sdX eines per USB angeschlossenen Block-Geräts
   (die CFast hängt an SATA -> Pfad enthält "ata", nicht "usb"). */
QString MainWindow::findUsbBlockDevice()
{
    QDir sysBlock("/sys/block");
    const QStringList devs = sysBlock.entryList(
        QStringList() << "sd*", QDir::Dirs | QDir::NoDotAndDotDot);
    for (const QString &d : devs) {
        QFileInfo fi("/sys/block/" + d);
        if (fi.canonicalFilePath().contains("/usb"))
            return "/dev/" + d;
    }
    return QString();
}

bool MainWindow::usbStickPresent()
{
    return !findUsbBlockDevice().isEmpty();
}

/* 1x/s aufgerufen, solange die Update-Seite offen ist */
void MainWindow::updateUsbState()
{
    m_usbPresent = usbStickPresent();

    const QString col = m_usbPresent ? "#000000" : "#B0B0B0"; /* schwarz / hellgrau */
    const QString labStyle =
        QString("color: %1; border: 1px solid %2;").arg(col).arg(COL_BORDER);
    m_jaLabel->setStyleSheet(labStyle);
    m_neinLabel->setStyleSheet(labStyle);

    if (m_usbPresent) {
        m_usbHint->setText("USB-Stick erkannt — Taste 1 = Ja, Taste 2 = Nein.");
        m_usbHint->setStyleSheet("color: #2E7D32; border: none;");
    } else {
        m_usbHint->setText("Kein USB-Stick angeschlossen — nur C (zurück).");
        m_usbHint->setStyleSheet("color: #B0B0B0; border: none;");
    }
}

/* "Ja": .tar.gz im Stick-Hauptverzeichnis auspacken, darin enthaltene
   myboard-gui auf die CFast (/usr/bin) kopieren, dann Service neu starten. */
void MainWindow::doUpdate()
{
    m_usbTimer->stop();   /* während des Updates nicht weiterprüfen */
    m_usbHint->setText("Update läuft … bitte warten.");
    m_usbHint->setStyleSheet("color: #000000; border: none;");
    QCoreApplication::processEvents();

    const QString dev = findUsbBlockDevice();
    bool ok = false;
    QString err;

    if (!dev.isEmpty()) {
        const QString script = QString(
            "set -e\n"
            "DEV='%1'\n"
            "PART=\"${DEV}1\"; [ -b \"$PART\" ] || PART=\"$DEV\"\n"
            "MNT=/mnt/usb-update\n"
            "TMP=$(mktemp -d)\n"
            "mkdir -p \"$MNT\"\n"
            "umount \"$MNT\" 2>/dev/null || true\n"
            "mount \"$PART\" \"$MNT\"\n"
            "TGZ=$(ls \"$MNT\"/*.tar.gz 2>/dev/null | head -n1)\n"
            "if [ -z \"$TGZ\" ]; then umount \"$MNT\"; echo 'kein .tar.gz gefunden' >&2; exit 2; fi\n"
            "tar xzf \"$TGZ\" -C \"$TMP\"\n"
            "BIN=$(find \"$TMP\" -name myboard-gui -type f | head -n1)\n"
            "if [ -z \"$BIN\" ]; then umount \"$MNT\"; echo 'myboard-gui nicht im Archiv' >&2; exit 3; fi\n"
            /* Laufende Binary kann nicht in-place ueberschrieben werden
             * (ETXTBSY). Daher erst .new schreiben, dann atomar per rename()
             * drueberschieben — der laufende Prozess laeuft an der alten
             * Inode weiter, systemctl restart startet danach die neue. */
            "cp \"$BIN\" /usr/bin/myboard-gui.new\n"
            "chmod 0755 /usr/bin/myboard-gui.new\n"
            "mv -f /usr/bin/myboard-gui.new /usr/bin/myboard-gui\n"
            "sync\n"
            "umount \"$MNT\"\n"
        ).arg(dev);

        QProcess proc;
        proc.start("sh", QStringList() << "-c" << script);
        proc.waitForFinished(60000);
        ok = (proc.exitStatus() == QProcess::NormalExit && proc.exitCode() == 0);
        err = QString::fromLocal8Bit(proc.readAllStandardError()).trimmed();
    } else {
        err = "kein USB-Gerät gefunden";
    }

    if (ok) {
        m_usbHint->setText("Update erfolgreich — Neustart der Anwendung …");
        m_usbHint->setStyleSheet("color: #2E7D32; border: none;");
        QCoreApplication::processEvents();
        /* Neue Binary starten: Service neu starten löst die laufende App ab. */
        QProcess::startDetached("systemctl",
                                QStringList() << "restart" << "myboard-gui.service");
    } else {
        m_usbHint->setText("Update fehlgeschlagen: " +
                           (err.isEmpty() ? QStringLiteral("Fehler") : err));
        m_usbHint->setStyleSheet("color: #C0392B; border: none;");
        m_usbTimer->start(1000);   /* USB-Prüfung wieder aufnehmen */
    }
}
