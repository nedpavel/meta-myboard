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

    /* Neue DDS-Abfrage an DIA1 anstossen. false, wenn schon eine laeuft. */
    bool abfragen();
    bool busy() const { return m_busy; }

signals:
    /* Antwort des Dienstes: {"ok": true, "anzahl": n, "meldungen": [...]} */
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
};

#endif /* DDSCLIENT_H */
