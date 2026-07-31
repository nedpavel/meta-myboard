/* canmatrix.cpp — Implementierung des CAN-Matrix-Decoders */
#include "canmatrix.h"

#include <QFile>
#include <QIODevice>

/* Bits little-endian (CANopen/Intel) aus dem Payload extrahieren:
   Bit 0 = LSB von Byte 0. Fehlende Bytes zaehlen als 0. */
static quint64 extractBits(const uchar *data, int len, int bitOffset, int width)
{
    quint64 v = 0;
    for (int i = 0; i < width; ++i) {
        int bit  = bitOffset + i;
        int byte = bit >> 3;
        int b    = bit & 7;
        if (byte < 0 || byte >= len)
            continue;                 /* ausserhalb -> 0-Bit */
        quint64 bitVal = (quint64)((data[byte] >> b) & 0x1);
        v |= (bitVal << i);
    }
    return v;
}

bool CanMatrix::loadFromResource(const QString &resourcePath)
{
    m_byId.clear();
    m_count = 0;

    QFile f(resourcePath);
    if (!f.open(QIODevice::ReadOnly | QIODevice::Text))
        return false;

    while (!f.atEnd()) {
        const QByteArray raw = f.readLine();
        if (raw.isEmpty() || raw.startsWith('#'))
            continue;
        const QString line = QString::fromUtf8(raw).trimmed();
        if (line.isEmpty())
            continue;

        /* Spalten: can_id bit_offset width signed factor offset unit name comment
           Comment ist das letzte Feld (kann Leerzeichen enthalten). */
        const QStringList c = line.split('\t');
        if (c.size() < 8)
            continue;

        bool ok = false;
        quint32 canId = c[0].toUInt(&ok);
        if (!ok)
            continue;

        CanSignalDef s;
        s.bitOffset = c[1].toInt();
        s.width     = c[2].toInt();
        s.isSigned  = (c[3].toInt() != 0);
        s.factor    = c[4].toDouble();
        s.offset    = c[5].toDouble();
        s.unit      = c[6];
        s.name      = c[7];
        s.comment   = (c.size() >= 9) ? c[8] : QString();

        if (s.width <= 0 || s.width > 32)
            continue;

        m_byId[canId].append(s);
        ++m_count;
    }
    return m_count > 0;
}

QVector<CanDecoded> CanMatrix::decode(quint32 canId, const QByteArray &data) const
{
    QVector<CanDecoded> out;
    const auto it = m_byId.constFind(canId);
    if (it == m_byId.constEnd())
        return out;

    const uchar *bytes = reinterpret_cast<const uchar *>(data.constData());
    const int len = data.size();

    out.reserve(it->size());
    for (const CanSignalDef &s : *it) {
        quint64 rawU = extractBits(bytes, len, s.bitOffset, s.width);

        double num;
        if (s.isSigned && s.width < 64) {
            /* Vorzeichen-Erweiterung auf 64 Bit */
            qint64 sraw = (qint64)rawU;
            if (rawU & (quint64(1) << (s.width - 1)))
                sraw |= (~quint64(0) << s.width);
            num = (double)sraw;
        } else {
            num = (double)rawU;
        }

        CanDecoded d;
        d.name    = s.name;
        d.comment = s.comment;
        d.unit    = s.unit;
        d.raw     = rawU;
        d.value   = num * s.factor + s.offset;
        d.isBool  = (s.width == 1);
        out.append(d);
    }
    return out;
}
