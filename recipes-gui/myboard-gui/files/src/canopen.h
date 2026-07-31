/* canopen.h — generische CANopen-Protokoll-Interpretation
 *
 * Ergaenzt die Matrix (die nur PDO-Prozessdaten kennt): interpretiert
 * jedes Telegramm anhand des CANopen-Function-Codes (obere Bits der
 * COB-ID) — NMT, SYNC, EMCY, PDO, SDO, Heartbeat. Braucht keine Matrix.
 *
 * canopenDescribe() gibt .ok=false zurueck, wenn die ID in keine
 * CANopen-Klasse passt (dann greift die Roh-Hex-Anzeige des Aufrufers).
 */
#ifndef CANOPEN_H
#define CANOPEN_H

#include <QString>
#include <QByteArray>

struct CanOpenInfo {
    bool    ok = false;   /* konnte klassifiziert werden?          */
    QString label;        /* z.B. "Heartbeat Node 11" (Spalte Signal) */
    QString value;        /* z.B. "Operational"        (Spalte Wert)  */
};

/* Bytes als Hex-String, z.B. "40 00 10 00". */
QString canHex(const QByteArray &data);

/* Ein Telegramm generisch nach CANopen interpretieren. */
CanOpenInfo canopenDescribe(quint32 canId, const QByteArray &data);

#endif /* CANOPEN_H */
