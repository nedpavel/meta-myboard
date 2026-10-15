/* ddsclient.cpp — DDS-Abfrage an DIA1 ueber den Dienst idu2-mvb */
#include "ddsclient.h"

#include <QLocalSocket>
#include <QTimer>
#include <QJsonDocument>
#include <QJsonParseError>

static const char *SOCKET_PATH = "/run/idu2-mvb.sock";

/* Eine Seite (16 Meldungen) dauert am Laborbus rund 1 s. Der Dienst
   bricht selbst ab, wenn DIA1 15 s lang schweigt, und meldet das. Hier nur
   die aeussere Grenze. */
static const int TIMEOUT_MS = 60000;

DdsClient::DdsClient(QObject *parent)
    : QObject(parent),
      m_sock(new QLocalSocket(this)),
      m_timer(new QTimer(this))
{
    m_timer->setSingleShot(true);
    connect(m_timer, &QTimer::timeout, this, &DdsClient::onTimeout);
    connect(m_sock, &QLocalSocket::connected, this, &DdsClient::onConnected);
    connect(m_sock, &QLocalSocket::readyRead, this, &DdsClient::onReadyRead);
    connect(m_sock, &QLocalSocket::errorOccurred, this,
            [this](QLocalSocket::LocalSocketError) {
        if (!m_busy)
            return;
        const QString why = m_sock->errorString();
        fertig();
        emit fehler(QStringLiteral("Dienst idu2-mvb nicht erreichbar (%1) — "
                                   "läuft er? systemctl status idu2-mvb")
                        .arg(why));
    });
}

bool DdsClient::abfragen(int seite, bool neu)
{
    if (m_busy)
        return false;
    m_busy = true;
    m_seite = seite;
    m_neu = neu;
    m_buf.clear();
    m_sock->abort();
    m_timer->start(TIMEOUT_MS);
    m_sock->connectToServer(QString::fromLatin1(SOCKET_PATH));
    return true;
}

void DdsClient::onConnected()
{
    m_sock->write(QStringLiteral("{\"befehl\": \"dds\", \"seite\": %1, "
                                 "\"neu\": %2}\n")
                      .arg(m_seite)
                      .arg(m_neu ? "true" : "false").toUtf8());
    m_sock->flush();
}

void DdsClient::onReadyRead()
{
    m_buf += m_sock->readAll();
    const int nl = m_buf.indexOf('\n');
    if (nl < 0)
        return;                         /* Zeile noch nicht vollstaendig */

    QJsonParseError err;
    const QJsonDocument doc = QJsonDocument::fromJson(m_buf.left(nl), &err);
    fertig();
    if (err.error != QJsonParseError::NoError || !doc.isObject()) {
        emit fehler(QStringLiteral("unlesbare Antwort vom Dienst: %1")
                        .arg(err.errorString()));
        return;
    }
    const QJsonObject obj = doc.object();
    if (!obj.value(QStringLiteral("ok")).toBool()) {
        emit fehler(obj.value(QStringLiteral("fehler")).toString(
            QStringLiteral("unbekannter Fehler")));
        return;
    }
    emit ergebnis(obj);
}

void DdsClient::onTimeout()
{
    if (!m_busy)
        return;
    fertig();
    emit fehler(QStringLiteral("keine Antwort vom Dienst idu2-mvb"));
}

void DdsClient::fertig()
{
    m_busy = false;
    m_timer->stop();
    m_sock->abort();
}
