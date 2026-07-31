/* canmatrix.h — CAN-Matrix-Decoder
 *
 * Laedt die kompakte Signaltabelle (can_matrix.tsv, als Qt-Ressource
 * eingebettet) und dekodiert empfangene CAN-Telegramme in benannte,
 * skalierte, human-readable Signale.
 *
 * Erzeugt aus der DIKO-XML via tools/diko_to_tsv.py.
 */
#ifndef CANMATRIX_H
#define CANMATRIX_H

#include <QHash>
#include <QString>
#include <QVector>
#include <QByteArray>

/* Eine Signaldefinition (eine Zeile der TSV). */
struct CanSignalDef {
    int     bitOffset = 0;   /* Bit-Position im Payload (LSB0)     */
    int     width     = 1;   /* Bitbreite: 1 / 8 / 16 / 32         */
    bool    isSigned  = false;
    double  factor    = 1.0; /* user = raw*factor + offset         */
    double  offset    = 0.0;
    QString unit;
    QString name;
    QString comment;         /* menschenlesbare Beschreibung       */
};

/* Ein dekodiertes Signal (Ergebnis von decode()). */
struct CanDecoded {
    QString name;
    QString comment;
    QString unit;
    double  value = 0.0;     /* skalierter Nutzwert                */
    quint64 raw   = 0;       /* Rohwert (extrahierte Bits)         */
    bool    isBool = false;  /* Breite 1 -> als an/aus darstellbar */
};

class CanMatrix
{
public:
    CanMatrix() = default;

    /* Laedt die TSV (z.B. ":/can/can_matrix.tsv"). true bei Erfolg. */
    bool loadFromResource(const QString &resourcePath);

    /* Anzahl geladener Signale / distinkter CAN-IDs. */
    int  signalCount() const { return m_count; }
    int  idCount() const     { return m_byId.size(); }

    /* Ist zu dieser CAN-ID ueberhaupt etwas bekannt? */
    bool knows(quint32 canId) const { return m_byId.contains(canId); }

    /* Dekodiert alle Signale des Telegramms (canId, data[0..len-1]). */
    QVector<CanDecoded> decode(quint32 canId, const QByteArray &data) const;

private:
    QHash<quint32, QVector<CanSignalDef>> m_byId;
    int m_count = 0;
};

#endif /* CANMATRIX_H */
