/* iduanfrage.cpp — einzelne Anfrage an den Dienst idu2-mvb */
#include "iduanfrage.h"

#include <QByteArray>
#include <QJsonDocument>
#include <QJsonParseError>
#include <QLocalSocket>
#include <QString>
#include <QTimer>
#include <memory>

static const char *SOCKET_PATH = "/run/idu2-mvb.sock";

void iduAnfrage(QObject *besitzer, const QJsonObject &anfrage,
                std::function<void(const QJsonObject &)> antwort,
                int timeoutMs)
{
    QLocalSocket *sock = new QLocalSocket(besitzer);
    QTimer *timer = new QTimer(sock);
    auto erledigt = std::make_shared<bool>(false);
    auto puffer = std::make_shared<QByteArray>();

    /* genau ein Rueckruf, danach Verbindung weg */
    auto ende = [sock, erledigt, antwort](const QJsonObject &obj) {
        if (*erledigt)
            return;
        *erledigt = true;
        sock->abort();
        sock->deleteLater();
        antwort(obj);
    };
    auto fehler = [ende](const QString &text) {
        ende(QJsonObject{{"ok", false}, {"fehler", text}});
    };

    timer->setSingleShot(true);
    QObject::connect(timer, &QTimer::timeout, sock, [fehler]() {
        fehler(QStringLiteral("keine Antwort vom Dienst idu2-mvb"));
    });
    QObject::connect(sock, &QLocalSocket::connected, sock, [sock, anfrage]() {
        sock->write(QJsonDocument(anfrage).toJson(QJsonDocument::Compact)
                    + '\n');
        sock->flush();
    });
    QObject::connect(sock, &QLocalSocket::readyRead, sock,
                     [sock, puffer, ende, fehler]() {
        *puffer += sock->readAll();
        const int nl = puffer->indexOf('\n');
        if (nl < 0)
            return;                     /* Zeile noch nicht vollstaendig */
        QJsonParseError err;
        const QJsonDocument doc = QJsonDocument::fromJson(puffer->left(nl), &err);
        if (err.error != QJsonParseError::NoError || !doc.isObject())
            fehler(QStringLiteral("unlesbare Antwort vom Dienst idu2-mvb"));
        else
            ende(doc.object());
    });
    QObject::connect(sock, &QLocalSocket::errorOccurred, sock,
                     [sock, fehler](QLocalSocket::LocalSocketError) {
        fehler(QStringLiteral("Dienst idu2-mvb nicht erreichbar (%1) — "
                              "läuft er? systemctl status idu2-mvb")
                   .arg(sock->errorString()));
    });

    timer->start(timeoutMs);
    sock->connectToServer(qEnvironmentVariable("IDU2_MVB_SOCK",
                                               QString::fromLatin1(SOCKET_PATH)));
}
