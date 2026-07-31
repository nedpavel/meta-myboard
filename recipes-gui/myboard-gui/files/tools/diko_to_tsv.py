#!/usr/bin/env python3
"""diko_to_tsv.py — DIKO-XML (CAN-Matrix) -> kompakte TSV-Signaltabelle.

Die myboard-GUI bettet die erzeugte TSV als Qt-Ressource ein (can_matrix.tsv)
und dekodiert damit live empfangene CAN-Telegramme. Statt die 4.8-MB-XML auf
dem Board zu parsen, wird hier offline nur das Nötige extrahiert.

Aufruf:
    python3 diko_to_tsv.py <DB_File.xml> <can_matrix.tsv>

Signal-Modell (aus <CanSignal ...>):
    COPID       -> CAN-ID (COB-ID) des Telegramms
    BitOffset   -> Bit-Position im Payload (LSB0, little-endian / CANopen)
    SIGNAL_TYP  -> Breite: 0=1bit(bool) 1=8bit 2=16bit(int16) 3=32bit(uint32)
    Min/MaxValueBus <-> Min/MaxValueUser  -> lineare Skalierung roh->nutz
    Name, Comment, Unit -> human-readable

TSV-Spalten (Tab-getrennt, Comment als letztes Feld):
    can_id  bit_offset  width_bits  signed  factor  offset  unit  name  comment
"""
import re
import sys

# SIGNAL_TYP -> Bitbreite
TYP_WIDTH = {"0": 1, "1": 8, "2": 16, "3": 32}

ATTR_RE = re.compile(r'(\w+)="([^"]*)"')
SIGNAL_RE = re.compile(r'<CanSignal\b([^>]*?)/?>')


def parse_attrs(blob):
    return {k: v for k, v in ATTR_RE.findall(blob)}


def to_float(s, default=0.0):
    try:
        return float(s)
    except (TypeError, ValueError):
        return default


def clean(s):
    # Tabs/Zeilenumbrüche im Freitext neutralisieren, damit die TSV-Struktur hält
    return (s or "").replace("\t", " ").replace("\r", " ").replace("\n", " ").strip()


def main():
    if len(sys.argv) != 3:
        sys.stderr.write("usage: diko_to_tsv.py <in.xml> <out.tsv>\n")
        return 2
    xml_path, tsv_path = sys.argv[1], sys.argv[2]
    with open(xml_path, "r", encoding="utf-8", errors="replace") as f:
        data = f.read()

    rows = []
    skipped = 0
    for m in SIGNAL_RE.finditer(data):
        a = parse_attrs(m.group(1))
        copid = a.get("COPID")
        typ = a.get("SIGNAL_TYP")
        if copid is None or typ not in TYP_WIDTH:
            skipped += 1
            continue
        try:
            can_id = int(copid)
        except ValueError:
            skipped += 1
            continue
        if can_id <= 0:
            skipped += 1
            continue
        bit_off = int(a.get("BitOffset", "0"))
        width = TYP_WIDTH[typ]
        min_bus = to_float(a.get("MinValueBus"), 0.0)
        max_bus = to_float(a.get("MaxValueBus"), 0.0)
        min_usr = to_float(a.get("MinValueUser"), 0.0)
        max_usr = to_float(a.get("MaxValueUser"), 0.0)
        # lineare Skalierung roh->nutz: user = raw*factor + offset
        if max_bus != min_bus:
            factor = (max_usr - min_usr) / (max_bus - min_bus)
            offset = min_usr - min_bus * factor
        else:
            factor, offset = 1.0, 0.0
        signed = 1 if min_bus < 0 else 0
        name = clean(a.get("Name"))
        comment = clean(a.get("Comment"))
        unit = clean(a.get("Unit"))
        # "Unit" als Platzhalter-Einheit entwerten
        if unit == "Unit":
            unit = ""
        rows.append((can_id, bit_off, width, signed, factor, offset, unit, name, comment))

    # nach CAN-ID, dann BitOffset sortieren (stabile, lesbare Reihenfolge)
    rows.sort(key=lambda r: (r[0], r[1]))

    with open(tsv_path, "w", encoding="utf-8", newline="\n") as f:
        f.write("# can_id\tbit_offset\twidth_bits\tsigned\tfactor\toffset\tunit\tname\tcomment\n")
        for (cid, bo, w, sg, fac, off, unit, name, comment) in rows:
            # factor/offset kompakt aber verlustarm
            f.write("%d\t%d\t%d\t%d\t%.10g\t%.10g\t%s\t%s\t%s\n"
                    % (cid, bo, w, sg, fac, off, unit, name, comment))

    ids = sorted(set(r[0] for r in rows))
    sys.stderr.write("signale: %d  distinkte can-ids: %d  uebersprungen: %d\n"
                     % (len(rows), len(ids), skipped))
    return 0


if __name__ == "__main__":
    sys.exit(main())
