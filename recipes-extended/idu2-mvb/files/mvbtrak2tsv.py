#!/usr/bin/env python3
"""Signalliste fuer idu2-mvb aus der MVB-Busbeschreibung ("MVB (trak).txt").

    mvbtrak2tsv.py BUSBESCHREIBUNG > mvb-signale.tsv

Uebernommen werden alle Signale, die IDU1 oder IDU2 lesen ($SINKMEMB), und
alle Signale der Telegramme, die IDU1 oder IDU2 senden. Welche davon gelten,
entscheidet idu2-mvb nach seiner Rolle (--rolle).

Aufbau der Busbeschreibung:

    $TELEGRAM  <name>  <quelle>  <port>  <zykluszeit_ms>
     $SRCSIG   <signal>  <typ>  <byte_ofs>  <bit_ofs>   [; Kommentar]
      $SINKMEMB  <empfaenger>
    ; Beschreibung (eine oder mehrere Zeilen)
    ;----------

Die Laenge eines Ports steht nur selten in der Datei, in dieser Reihenfolge
gilt:
  1. ein fuenftes Feld der Telegrammzeile, B<n> = n Byte
     ("$TELEGRAM ETCS-862 ETCS 862 1024 B16");
  2. das letzte belegte Byte aller Signale plus freie Bytes, die ein
     Kommentar im Telegramm nennt ("4 bytes free", "Noch 4 Bits & 28 Bytes
     frei");
  3. sonst das letzte belegte Byte allein;
jeweils aufgerundet auf eine MVB-Portgroesse (2, 4, 8, 16, 32 Byte) - fuer
491/492/498 wie am Bus bestaetigt 4/8/16. Wo die Datei nichts sagt, kann
ein Port laenger sein als abgeleitet; am Bus bleibt er dann auf frisch = 0.

Bits: bit_ofs zaehlt ab byte_ofs durch, Byte fuer Byte, im Byte ab dem
niederwertigsten Bit. Das Bit liegt also in Byte byte_ofs + bit_ofs/8, dort
an Stelle bit_ofs % 8. So steht es bei den I/O-Geraeten der Datei
("BIT 0 7 ;(Bit 15)", "BIT 0 8 ;(Bit 0)").

Datentypen (Spalte datentyp), Werte big-endian wie am MVB:

    MVB-Typ               datentyp   Python    Umrechnung
    BIT BOOLEAN1          bool       bool
    CARD8                 uint8      int
    CARD16 WORD GANZOHNE  uint16     int       (GANZOHNE: ganzzahlig ohne
                                                Vorzeichen)
    CARD32                uint32     int
    INT16                 int16      int
    ANALOG BIFRACT800     float      float     int16 * 25/1024 in %
                                               ("1000 Hex = 100 %"); in
                                               32 Bit exakt, 25/1024 ist
                                               ein Binaerbruch
    TIMEDATE48            datetime   datetime  Sekunden seit 1970 (uint32)
                                               + Ticks/65536 (uint16), UTC;
                                               als double exakt (48 Bit)
    AR_CHAR8_8 _16 _32    string     str       8/16/32 Zeichen, Nullbytes am
                                               Ende weg
    AR8 AR16 AR32         string     str       ebenso (Zugnummer, Fahrzeug-ID
                                               usw.); die Bytes stehen
                                               zusaetzlich in roh

Die Datei mischt Zeichensaetze (CP850 und Windows-1252); dekodiert wird
deshalb zeilenweise.

Ausgabe: Tabulator-getrennt, eine Zeile je Signal, Kopfzeile mit '#'.
"""

import re
import sys

ROLLEN = ("IDU1", "IDU2")

ANALOG_FAKTOR = 25 / 1024               # 100 % / 0x1000

# MVB-Typ -> (Datentyp, Bytes, Faktor, Einheit); Bytes 0 = ein Bit
TYPEN = {
    "BIT":        ("bool",      0, 1, ""),
    "BOOLEAN1":   ("bool",      0, 1, ""),
    "CARD8":      ("uint8",     1, 1, ""),
    "CARD16":     ("uint16",    2, 1, ""),
    "WORD":       ("uint16",    2, 1, ""),
    "GANZOHNE":   ("uint16",    2, 1, ""),
    "CARD32":     ("uint32",    4, 1, ""),
    "INT16":      ("int16",     2, 1, ""),
    "ANALOG":     ("float",     2, ANALOG_FAKTOR, "%"),
    "BIFRACT800": ("float",     2, ANALOG_FAKTOR, "%"),
    "TIMEDATE48": ("datetime",  6, 1, "UTC"),
    "AR_CHAR8_8":  ("string",   8, 1, ""),
    "AR_CHAR8_16": ("string",  16, 1, ""),
    "AR_CHAR8_32": ("string",  32, 1, ""),
    "AR8":         ("string",   8, 1, ""),
    "AR16":        ("string",  16, 1, ""),
    "AR32":        ("string",  32, 1, ""),
}
PORTGROESSEN = (2, 4, 8, 16, 32)
SPALTEN = ("port", "telegramm", "quelle", "zyklus_ms", "port_bytes", "signal",
           "typ", "datentyp", "byte", "bit", "bytes", "faktor", "einheit",
           "senken", "beschreibung")

CP850_UMLAUTE = set(b"\x81\x84\x94\x8e\x99\x9a")         # ue ae oe Ae Oe Ue
CP1252_UMLAUTE = set(b"\xe4\xf6\xfc\xc4\xd6\xdc\xdf")    # ae oe ue Ae Oe Ue ss


def zeile_dekodieren(roh):
    """Eine Zeile als Text, Zeichensatz je Zeile geraten."""
    if set(roh) & CP1252_UMLAUTE and not set(roh) & CP850_UMLAUTE:
        return roh.decode("cp1252", errors="replace")
    return roh.decode("cp850", errors="replace")


def typ_info(typ):
    """(Datentyp, Bytes, Faktor, Einheit); unbekannte Typen als 2 Byte roh."""
    if typ in TYPEN:
        return TYPEN[typ]
    m = re.match(r"AR(?:_CHAR8_)?(\d+)$", typ)      # andere Laengen
    if m:
        return "string", int(m.group(1)), 1, ""
    print("unbekannter Typ %s - als bytes (2) uebernommen" % typ,
          file=sys.stderr)
    return "bytes", 2, 1, ""


def belegt_bis(sig):
    """Erstes Byte hinter dem Signal."""
    datentyp, n, _, _ = typ_info(sig["typ"])
    if datentyp == "bool":
        return sig["byte"] + sig["bit"] // 8 + 1
    return sig["byte"] + n


def einlesen(pfad):
    telegramme = []
    tgm = sig = None
    sammeln = False             # Beschreibungszeilen hinter dem Signal
    with open(pfad, "rb") as f:
        for roh in f:
            s = zeile_dekodieren(roh.rstrip(b"\r\n")).strip()
            if s.startswith(";"):
                text = s.lstrip(";").strip()
                frei = re.search(r"(\d+)\s*Bytes?\s+(?:free|frei)\b", text,
                                 re.I)
                if frei and tgm is not None:
                    tgm["frei"] += int(frei.group(1))
                if text[:1] in ("-", "=", "*", "$"):
                    sammeln = False     # Trennlinie, auskommentierte Zeile
                elif text and sammeln and sig is not None:
                    sig["text"].append(text)
                continue
            code, _, kommentar = s.partition(";")
            w = code.split()
            if not w:
                continue
            sammeln = False
            if w[0] == "$TELEGRAM" and len(w) >= 5:
                # Port dezimal oder hexadezimal mit H (I/O-Geraete: D38H)
                port = re.fullmatch(r"(\d+)|([0-9A-Fa-f]+)[Hh]", w[3])
                zyklus = re.match(r"\d+", w[4])
                sig = None
                if not (port and zyklus):
                    print("Telegramm ohne Port/Zyklus: %s" % s, file=sys.stderr)
                    tgm = None
                    continue
                laenge = (re.fullmatch(r"B(\d+)", w[5]) if len(w) > 5
                          else None)
                tgm = {"name": w[1], "quelle": w[2],
                       "port": (int(port.group(1)) if port.group(1)
                                else int(port.group(2), 16)),
                       "zyklus": int(zyklus.group()), "signale": [],
                       "laenge": int(laenge.group(1)) if laenge else None,
                       "frei": 0}
                telegramme.append(tgm)
            elif w[0] == "$SRCSIG" and len(w) >= 5 and tgm is not None:
                k = kommentar.strip()
                sig = {"name": w[1], "typ": w[2], "byte": int(w[3]),
                       "bit": int(w[4]), "senken": [], "text": [k] if k else []}
                tgm["signale"].append(sig)
                sammeln = True
            elif w[0] == "$SINKMEMB" and len(w) >= 2 and sig is not None:
                sig["senken"].append(w[1])
                sammeln = True
            elif w[0].startswith("$"):
                sig = None
                if w[0] in ("$DEVICE", "$EXTDEV", "$BUS", "$PARA"):
                    tgm = None
    return telegramme


def portgroesse(tgm):
    belegt = max((belegt_bis(s) for s in tgm["signale"]), default=2)
    if tgm["laenge"] in PORTGROESSEN:
        if belegt > tgm["laenge"]:
            print("Telegramm %s: B%d, aber Signale bis Byte %d"
                  % (tgm["name"], tgm["laenge"], belegt), file=sys.stderr)
        return tgm["laenge"]
    ende = belegt + tgm["frei"]
    for g in PORTGROESSEN:
        if ende <= g:
            return g
    # "2 bytes free" bei schon vollen 32 Byte (FLG2-282): Kommentar veraltet
    if belegt > 32:
        print("Telegramm %s: %d Byte belegt, mehr als 32"
              % (tgm["name"], belegt), file=sys.stderr)
    return 32


def main():
    if len(sys.argv) != 2:
        print("Aufruf: mvbtrak2tsv.py BUSBESCHREIBUNG > mvb-signale.tsv",
              file=sys.stderr)
        return 2
    tgms = einlesen(sys.argv[1])
    out = sys.stdout
    out.write("# " + "\t".join(SPALTEN) + "\n")
    n = 0
    for t in tgms:
        eigen = t["quelle"] in ROLLEN
        groesse = portgroesse(t)
        for s in t["signale"]:
            if not eigen and not set(s["senken"]) & set(ROLLEN):
                continue
            datentyp, nbytes, faktor, einheit = typ_info(s["typ"])
            text = " / ".join(s["text"]).replace("\t", " ")
            out.write("\t".join(str(x) for x in (
                t["port"], t["name"], t["quelle"], t["zyklus"], groesse,
                s["name"], s["typ"], datentyp, s["byte"], s["bit"], nbytes,
                faktor, einheit, ",".join(s["senken"]), text)) + "\n")
            n += 1
    print("%d Telegramme gelesen, %d Signale fuer IDU1/IDU2" % (len(tgms), n),
          file=sys.stderr)
    return 0


if __name__ == "__main__":
    sys.exit(main())
