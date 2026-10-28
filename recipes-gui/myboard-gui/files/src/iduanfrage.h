/* iduanfrage.h — einzelne Anfrage an den Dienst idu2-mvb
 *
 * Fuer kurze Befehle wie "softkeys" und "softkey": je Anfrage eine eigene
 * Verbindung zu /run/idu2-mvb.sock, eine JSON-Zeile hin, eine zurueck.
 * Nicht blockierend (QLocalSocket in der Qt-Eventloop); die Antwort kommt
 * als Rueckruf, bei Fehlern als {"ok": false, "fehler": "..."}.
 *
 * Die DDS-Abfrage bleibt bei DdsClient (eine Abfrage zur Zeit, lange
 * Laufzeit). Zum Testen ohne Geraet lenkt die Umgebungsvariable
 * IDU2_MVB_SOCK auf einen anderen Socket.
 */
#ifndef IDUANFRAGE_H
#define IDUANFRAGE_H

#include <QJsonObject>
#include <functional>

class QObject;

/* besitzer: Elternobjekt der Verbindung; wird er zerstoert, entfaellt
   auch der Rueckruf. */
void iduAnfrage(QObject *besitzer, const QJsonObject &anfrage,
                std::function<void(const QJsonObject &)> antwort,
                int timeoutMs = 3000);

#endif /* IDUANFRAGE_H */
