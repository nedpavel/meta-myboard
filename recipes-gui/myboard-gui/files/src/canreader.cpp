/* canreader.cpp — SocketCAN-Empfaenger (Linux) */
#include "canreader.h"

#include <QSocketNotifier>

#include <sys/socket.h>
#include <sys/ioctl.h>
#include <sys/types.h>
#include <net/if.h>
#include <unistd.h>
#include <fcntl.h>
#include <cstring>
#include <cerrno>

#include <linux/can.h>
#include <linux/can/raw.h>

CanReader::CanReader(QObject *parent) : QObject(parent) {}

CanReader::~CanReader() { close(); }

bool CanReader::open(const QString &ifname)
{
    close();

    int fd = ::socket(PF_CAN, SOCK_RAW, CAN_RAW);
    if (fd < 0) {
        emit errorOccurred(QStringLiteral("socket(PF_CAN) fehlgeschlagen: %1")
                               .arg(strerror(errno)));
        return false;
    }

    struct ifreq ifr;
    std::memset(&ifr, 0, sizeof(ifr));
    const QByteArray name = ifname.toLatin1();
    std::strncpy(ifr.ifr_name, name.constData(), IFNAMSIZ - 1);
    if (::ioctl(fd, SIOCGIFINDEX, &ifr) < 0) {
        emit errorOccurred(QStringLiteral("Interface %1 nicht gefunden: %2")
                               .arg(ifname, strerror(errno)));
        ::close(fd);
        return false;
    }

    struct sockaddr_can addr;
    std::memset(&addr, 0, sizeof(addr));
    addr.can_family  = AF_CAN;
    addr.can_ifindex = ifr.ifr_ifindex;
    if (::bind(fd, reinterpret_cast<struct sockaddr *>(&addr), sizeof(addr)) < 0) {
        emit errorOccurred(QStringLiteral("bind %1 fehlgeschlagen: %2")
                               .arg(ifname, strerror(errno)));
        ::close(fd);
        return false;
    }

    /* nicht-blockierend, damit onActivated() sauber leer laufen kann */
    int flags = ::fcntl(fd, F_GETFL, 0);
    ::fcntl(fd, F_SETFL, flags | O_NONBLOCK);

    m_fd = fd;
    m_notifier = new QSocketNotifier(m_fd, QSocketNotifier::Read, this);
    connect(m_notifier, &QSocketNotifier::activated, this, &CanReader::onActivated);
    return true;
}

void CanReader::close()
{
    if (m_notifier) {
        m_notifier->setEnabled(false);
        m_notifier->deleteLater();
        m_notifier = nullptr;
    }
    if (m_fd >= 0) {
        ::close(m_fd);
        m_fd = -1;
    }
}

void CanReader::onActivated()
{
    if (m_fd < 0)
        return;

    struct can_frame frame;
    /* alle anstehenden Frames abholen, bis EAGAIN */
    for (;;) {
        ssize_t n = ::read(m_fd, &frame, sizeof(frame));
        if (n < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK)
                break;
            if (errno == EINTR)
                continue;
            emit errorOccurred(QStringLiteral("read can fehlgeschlagen: %1")
                                   .arg(strerror(errno)));
            break;
        }
        if (n < (ssize_t)sizeof(struct can_frame))
            break;

        /* Error-Frames ignorieren */
        if (frame.can_id & CAN_ERR_FLAG)
            continue;

        quint32 id = (frame.can_id & CAN_EFF_FLAG)
                         ? (frame.can_id & CAN_EFF_MASK)
                         : (frame.can_id & CAN_SFF_MASK);
        int dlc = frame.can_dlc;
        if (dlc > 8) dlc = 8;
        QByteArray data(reinterpret_cast<const char *>(frame.data), dlc);
        emit frameReceived(id, data);
    }
}
