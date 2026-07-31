/* canopen.cpp — generische CANopen-Protokoll-Interpretation */
#include "canopen.h"

QString canHex(const QByteArray &data)
{
    QString s;
    for (int i = 0; i < data.size(); ++i) {
        if (i) s += QLatin1Char(' ');
        s += QString("%1").arg((quint8)data[i], 2, 16, QLatin1Char('0')).toUpper();
    }
    return s;
}

static quint8 b(const QByteArray &d, int i)
{ return (i < d.size()) ? (quint8)d[i] : 0; }

/* Hex mit kleingeschriebenem 0x-Präfix, aber GROSSEN Hex-Ziffern. */
static QString hx(quint64 v, int width = 0)
{
    QString s = QString::number(v, 16).toUpper();
    if (width > 0) s = s.rightJustified(width, QLatin1Char('0'));
    return QStringLiteral("0x") + s;
}

/* Ein paar gaengige Objekt-Indizes benennen (Klartext fuer SDO). */
static QString objName(quint16 index)
{
    switch (index) {
    case 0x1000: return QStringLiteral(" (Device Type)");
    case 0x1001: return QStringLiteral(" (Error Register)");
    case 0x1008: return QStringLiteral(" (Device Name)");
    case 0x1009: return QStringLiteral(" (HW-Version)");
    case 0x100A: return QStringLiteral(" (SW-Version)");
    case 0x1017: return QStringLiteral(" (Heartbeat-Time)");
    case 0x1018: return QStringLiteral(" (Identity)");
    default:     return QString();
    }
}

static QString idxSub(const QByteArray &d)
{
    quint16 index = (quint16)(b(d, 1) | (b(d, 2) << 8));
    quint8  sub   = b(d, 3);
    return hx(index, 4) + QLatin1Char(':')
         + QString::number(sub, 16).toUpper().rightJustified(2, QLatin1Char('0'))
         + objName(index);
}

static QString expeditedData(const QByteArray &d, int cmd)
{
    /* Bei expedited SDO stehen die Daten in Byte 4..7; Groesse = 4 - n. */
    bool expedited = (cmd >> 1) & 0x1;
    if (!expedited) return QStringLiteral("(segmentiert)");
    int n = (cmd >> 2) & 0x3;
    int size = 4 - n;
    quint32 v = 0;
    QByteArray raw;
    for (int i = 0; i < size; ++i) {
        quint8 by = b(d, 4 + i);
        v |= (quint32)by << (8 * i);
        raw.append((char)by);
    }
    return hx(v) + QString(" (%1) [").arg(v) + canHex(raw) + QLatin1Char(']');
}

CanOpenInfo canopenDescribe(quint32 id, const QByteArray &d)
{
    CanOpenInfo r;
    const quint8 node = id & 0x7F;
    const quint8 fc   = (id >> 7) & 0xF;

    if (id == 0x000) {
        r.ok = true; r.label = QStringLiteral("NMT");
        quint8 cmd = b(d, 0), tgt = b(d, 1);
        QString c;
        switch (cmd) {
        case 0x01: c = "Start";               break;
        case 0x02: c = "Stop";                break;
        case 0x80: c = "Pre-Operational";     break;
        case 0x81: c = "Reset Node";          break;
        case 0x82: c = "Reset Communication"; break;
        default:   c = "cmd " + hx(cmd, 2);
        }
        r.value = c + " -> " + (tgt == 0 ? QStringLiteral("alle Nodes")
                                         : QString("Node %1").arg(tgt));
        return r;
    }
    if (id == 0x080) { r.ok = true; r.label = QStringLiteral("SYNC"); return r; }
    if (id == 0x100) { r.ok = true; r.label = QStringLiteral("TIME"); return r; }

    switch (fc) {
    case 0x1: /* 0x081..0x0FF: EMCY */
        r.ok = true;
        r.label = QString("EMCY Node %1").arg(node);
        r.value = "Fehlercode " + hx((quint16)(b(d,0) | (b(d,1) << 8)), 4)
                + ", Error-Reg " + hx(b(d,2), 2);
        return r;
    case 0x3: case 0x5: case 0x7: case 0x9: /* TPDO1..4 */
    case 0x4: case 0x6: case 0x8: case 0xA: /* RPDO1..4 */ {
        static const char *pdoName[] = {"","","","TPDO1","RPDO1","TPDO2","RPDO2",
                                        "TPDO3","RPDO3","TPDO4","RPDO4"};
        r.ok = true;
        r.label = QString("%1 Node %2").arg(pdoName[fc]).arg(node);
        r.value = canHex(d);
        return r;
    }
    case 0xB: /* 0x580..0x5FF: SDO-Antwort (Server->Client) */ {
        r.ok = true;
        r.label = QString("SDO-Antw Node %1").arg(node);
        quint8 cmd = b(d, 0);
        quint8 scs = cmd >> 5;
        if (scs == 0x4)        /* 0x80: Abort */
            r.value = "Abort " + idxSub(d) + " ("
                    + hx((quint32)(b(d,4)|(b(d,5)<<8)|(b(d,6)<<16)|((quint32)b(d,7)<<24)), 8)
                    + QLatin1Char(')');
        else if (scs == 0x2)   /* Upload-Antwort mit Daten */
            r.value = idxSub(d) + " = " + expeditedData(d, cmd);
        else if (scs == 0x3)   /* Download-Bestaetigung */
            r.value = "Schreib-OK " + idxSub(d);
        else
            r.value = idxSub(d) + " [" + canHex(d) + QLatin1Char(']');
        return r;
    }
    case 0xC: /* 0x600..0x67F: SDO-Request (Client->Server) */ {
        r.ok = true;
        r.label = QString("SDO-Req Node %1").arg(node);
        quint8 cmd = b(d, 0);
        quint8 ccs = cmd >> 5;
        if (ccs == 0x2)        /* 0x40: Upload = Lesen */
            r.value = "Lesen " + idxSub(d);
        else if (ccs == 0x1)   /* Download = Schreiben */
            r.value = "Schreiben " + idxSub(d) + " = " + expeditedData(d, cmd);
        else if (ccs == 0x4)   /* Abort */
            r.value = "Abort " + idxSub(d);
        else
            r.value = idxSub(d) + " [" + canHex(d) + QLatin1Char(']');
        return r;
    }
    case 0xE: /* 0x700..0x77F: Heartbeat / NMT-Error-Control */ {
        r.ok = true;
        r.label = QString("Heartbeat Node %1").arg(node);
        quint8 st = b(d, 0) & 0x7F;
        switch (st) {
        case 0x00: r.value = "Boot-up";         break;
        case 0x04: r.value = "Stopped";         break;
        case 0x05: r.value = "Operational";     break;
        case 0x7F: r.value = "Pre-Operational"; break;
        default:   r.value = "Zustand " + hx(st, 2);
        }
        return r;
    }
    default:
        r.ok = false;   /* -> Roh-Hex beim Aufrufer */
        return r;
    }
}
