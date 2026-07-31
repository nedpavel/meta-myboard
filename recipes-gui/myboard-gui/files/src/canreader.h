/* canreader.h — nicht-blockierender SocketCAN-Empfaenger
 *
 * Oeffnet ein SocketCAN-Interface (z.B. "can0") und liefert jeden
 * empfangenen Frame per Signal frameReceived(canId, data) — integriert
 * ueber QSocketNotifier in die Qt-Eventloop (blockiert die GUI nicht).
 */
#ifndef CANREADER_H
#define CANREADER_H

#include <QObject>
#include <QByteArray>
#include <QString>

class QSocketNotifier;

class CanReader : public QObject
{
    Q_OBJECT
public:
    explicit CanReader(QObject *parent = nullptr);
    ~CanReader() override;

    /* Interface oeffnen (z.B. "can0"). true bei Erfolg. */
    bool open(const QString &ifname);
    void close();
    bool isOpen() const { return m_fd >= 0; }

signals:
    void frameReceived(quint32 canId, const QByteArray &data);
    void errorOccurred(const QString &msg);

private slots:
    void onActivated();

private:
    int              m_fd = -1;
    QSocketNotifier *m_notifier = nullptr;
};

#endif /* CANREADER_H */
