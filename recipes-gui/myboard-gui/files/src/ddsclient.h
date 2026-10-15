/* ddsclient.h — DDS-Abfrage an DIA1 ueber den Dienst idu2-mvb
 *
 * /dev/mvblli0 gehoert dem Dienst idu2-mvb (exklusiv). Er haelt das
 * Display als IDU2 am MVB und fuehrt die DDS-Abfrage auf Anfrage aus —
 * ohne Initialisierungszeit, der Controller laeuft bereits.
 *
 * Verbindung ueber den Unix-Socket /run/idu2-mvb.sock, je Anfrage und
 * Antwort eine JSON-Zeile. Nicht blockierend (QLocalSocket in der
 * Qt-Eventloop); das Ergebnis kommt per Signal.
 */
#ifndef DDSCLIENT_H
#define DDSCLIENT_H

#include <QObject>
#include <QByteArray>
#include <QJsonObject>
#include <QString>

class QLocalSocket;
class QTimer;

class DdsClient : public QObject
{
    Q_OBJECT
public:
    explicit DdsClient(QObject *parent = nullptr);

    /* Seite 1, 2, ... der DDS-Meldungen von DIA1 abfragen (16 je Seite,
       --anzahl des Dienstes).
       Schon geholte Seiten liefert der Dienst aus seinem Zwischenspeicher;
       neu = true holt Seite 1 auf jeden Fall neu von DIA1.
       false, wenn schon eine Abfrage laeuft. */
    bool abfragen(int seite = 1, bool neu = false);
    bool busy() const { return m_busy; }

signals:
    /* Antwort des Dienstes: {"ok": true, "seite": n, "letzte": bool,
       "meldungen": [...], evtl. "hinweis": "..."} */
    void ergebnis(const QJsonObject &antwort);
    void fehler(const QString &meldung);

private slots:
    void onConnected();
    void onReadyRead();
    void onTimeout();

private:
    void fertig();

    QLocalSocket *m_sock;
    QTimer       *m_timer;
    QByteArray    m_buf;
    bool          m_busy = false;
    int           m_seite = 1;
    bool          m_neu = false;
};

#endif /* DDSCLIENT_H */
