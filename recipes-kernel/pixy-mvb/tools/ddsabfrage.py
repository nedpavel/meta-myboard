#!/usr/bin/env python3
"""DDS-Abfrage an den Diagnoserechner DIA1 - wie das Anzeigedisplay.

Spielt die Message-Abfrage aus dem Mitschnitt des Herstellerbetriebs
nach und beantwortet alles, was DIA1 daraufhin schickt:

  (1) Anfrage an DIA1, IDU2 ist Caller (Funktion 0 -> 0xE0)
      IDU2 -> CR 0x80  Ref 0043, 28 Byte    DIA1 -> CC 0x41
      IDU2 -> DT 0x98  Rest der Anfrage     DIA1 -> AK 0x61
      DIA1 -> CR 0x00  Reply, 10 Byte       IDU2 -> CC 0xC1
  (2) Daten von DIA1, DIA1 ist Caller (Funktion 0x0D -> 0xE0)
      DIA1 -> CR 0x80  Ref von DIA1         IDU2 -> CC 0x41, Credit 1
      DIA1 -> DT 0x9n  je 22 Byte           IDU2 -> AK 0x6n nach jedem DT
      IDU2 -> CR 0x00  Reply, 10 Byte       DIA1 -> CC 0xC1

Teil 2 kommt von DIA1 von selbst. Damit er durchlaeuft, muss unser Geraet
aber CC und AK senden - das erledigt dieses Skript. Die Antwortframes
werden aus dem empfangenen Frame gebaut: Netzkopf vertauscht, Referenz
gespiegelt. Nur die Nutzdaten der eigenen Frames stammen aus dem
Mitschnitt.

DIA1 laeuft laut Busbeschreibung (MVB (trak).txt) als Teilnehmer auf
VCU1, Geraeteadresse 02EH = 46. IDU2 hat 0F0H = 240.

Ohne Lebenszeichen gilt IDU2 fuer FLG1N, FLG2N und IDU1 als nicht da.
--lebenszeichen sendet deshalb Telegramm IDU2-491 (Port 491, 4 Byte,
Zyklus 256 ms): IDU2-XMVBLifeSig als CARD16 big-endian ab Byte 0, im
Sekundentakt hochgezaehlt wie bei der Herstelleranwendung. Byte 2 traegt
Luefterbefehle an FLG1N und bleibt 0. Die Anfrage geht erst nach dem
Vorlauf hinaus, damit die FLG das Lebenszeichen vorher sehen. Port 181
(Lebenszeichen FLG1N) wird zum Vergleich mitgelesen.

Frameaufbau (32 Byte, Offsets im Frame):
  0..3  Linkkopf - setzt der Treiber (Modus|Ziel, PT 8|Quelle)
  4     SZ, gueltige Bytes ab Offset 5
  5..8  Netzkopf: Zielknoten, Zielfunktion, Quellknoten, Quellfunktion
  9     MTC: Bit 7 Caller-Seite, Bits 6..4 Typ (0 CR, 4 CC, 1 DT, 6 AK),
        bei DT Bit 3 letztes Paket und Bits 2..0 Folgenummer
  CR    10..11 Ref, 12..15 Laenge, 16 Credit (oberes Halbbyte), 18.. Daten
  CC    10..11 Ref, 12 Credit
  DT    10.. Daten

    ddsabfrage.py <Verzeichnis> [Optionen]
    ddsabfrage.py --auswerten <frames.txt> [<Ausgabe.txt>]

Die DDS-Meldungen aus den Daten von DIA1 (Saetze mit 00 80 00) landen
entschluesselt in <Verzeichnis>/dds_meldungen.txt. --auswerten macht das
nachtraeglich aus der frames.txt eines frueheren Laufs.

    --lebenszeichen  Lebenszeichen auf Port 491 senden
    --alle-quellen   (mit --lebenszeichen) auch 492 (8 Byte) und 498
                     (16 Byte) senden, nur Nullen - keine Befehle
    --vorlauf S      Wartezeit nach START bis zur Anfrage (Vorgabe 20 mit
                     --lebenszeichen, sonst 2)
    --dauer S        nach dem Vorlauf so lange zuhoeren (Vorgabe 60)
    --nur-hoeren     keine Anfrage senden, nur auf DIA1 antworten
    --keine-antwort  in Teil 2 keine Reply-Message an DIA1 senden
    --ziel N         Geraeteadresse von DIA1 (Vorgabe 0x2E = 46)
    --ref HEX        Referenz der eigenen Anfrage (Vorgabe 0043)
    --hoch           Frames mit hoher Prioritaet senden (Vorgabe niedrig)
    --single-line    Einleitungsbetrieb auf Leitung A
    --dsw-md         MD-Bit im DSW setzen, am Ende wieder loeschen
    --zeichensatz CP Zeichensatz der Textzeilen unter dem Hexdump
                     (Vorgabe cp850; auch cp437, latin1, cp1252)
    --ef1            Messages ueber EF1 statt EF0 beim Busadministrator
                     anmelden (MD_CONF mit negativer Queue-Prioritaet,
                     MR 2000 statt 1000). Ein Geraet beteiligt sich nur an
                     Event_Rounds seiner Prioritaet (IEC 61375-3-1, 8.3.2)

Geantwortet wird nur auf Frames von DIA1. Rufe anderer Geraete (etwa
WTB1, Geraet 6) erscheinen im Protokoll, bleiben aber unbeantwortet.

Nur am Laborbus. Der Controller laeuft waehrend des Tests (MVB_GO).
Gelesen wird nur unterhalb von SA+0x3C0, die Interruptzeile bleibt
unberuehrt - der Message-Empfang braucht DTI1.
"""

import codecs
import errno
import fcntl
import mmap
import os
import struct
import sys
import time

DEV = "/dev/mvblli0"
BOARD = "/dev/mvb0"
BAR_SIZE = 0x4000000
TM = 0x2000000 + 0x40000
SA_CANDIDATES = (0x03C00, 0x07C00, 0x0FC00)
SA_FOR_MCM = (0x03C00, 0x07C00, 0x0FC00, 0x0FC00, 0x0FC00)
QDT = 0x310

IOC = {
    "WRITE_DEV_ADDR": 0x40024C02,
    "READ_DSW":       0x80024C03,
    "WRITE_DSW":      0x40044C04,
    "START":          0x00004C05,
    "STOP":           0x00004C06,
    "PD_CONF":        0x40024C0C,
    "MD_CONF":        0x40024C0E,
    "WRITE_CONTROL":  0x40064C14,
    "HWINIT":         0x40104C15,
}
PD, MD_HIGH, MD_LOW = 1, 2, 3
PD_SINK, PD_SRC = 1, 2
OWN_ADDR = 240
DIA1_ADDR = 0x2E

# Prozessdaten aus der Busbeschreibung: (Port, Groesse)
LS_PORT, LS_SIZE = 491, 4          # IDU2-491, IDU2-XMVBLifeSig ab Byte 0
FLG_LS_PORT, FLG_LS_SIZE = 181, 4  # IN-XMVBLifeSFLG1, nur mitlesen
EXTRA_SRC = ((492, 8), (498, 16))  # IDU2-492, IDU2-495 (Port 498)
DSW_MD = 0x1000
CTRL_CLEAR, CTRL_LINE_A = 0x03, 0x08

KINDS = {0: "CR", 4: "CC", 1: "DT", 6: "AK"}

# Aus dem Mitschnitt, ab Offset 4 (SZ). Linkkopf setzt der Treiber.
# (1)(a) Anfrage: CR mit den ersten 14 Byte, DT mit den letzten 14
CALL_CR = bytes.fromhex(
    "1b 00e00000 80 0043 0000001c 70 00 0100000001 0501010101 00010415"
    .replace(" ", ""))
CALL_DT = bytes.fromhex(
    "13 00e00000 98 6000475200000100420101200000 0101010100010415"
    .replace(" ", ""))
# (2)(b) Reply des Displays: Nutzdaten (10 Byte) und Fuellbytes
REPLY_DATA = bytes.fromhex("0100000000010501fb01")
REPLY_PAD = bytes.fromhex("00010415")
CC_CREDIT = 0x10            # Credit 1, wie das Display es sendet


def sig(req):
    """fcntl.ioctl will die Nummer vorzeichenbehaftet."""
    return req - (1 << 32) if req >= (1 << 31) else req


def be16(b, o):
    return (b[o] << 8) | b[o + 1]


def be32(b, o):
    return struct.unpack(">I", bytes(b[o:o + 4]))[0]


# Zeichensatz fuer die Textzeilen. Vorgabe CP850 (DOS Westeuropa): dort
# liegen die Umlaute bei 84 ae, 94 oe, 81 ue, 8E Ae, 99 Oe, 9A Ue, E1 ss -
# in Latin-1 waeren das Steuerzeichen. Umschaltbar mit --zeichensatz.
TEXT_CP = "cp850"


def to_text(b):
    """Je Byte ein Zeichen im eingestellten Zeichensatz; Steuerzeichen und
    nicht darstellbare Zeichen als Punkt."""
    out = []
    for x in b:
        c = bytes((x,)).decode(TEXT_CP, errors="replace")
        out.append(c if c.isprintable() and c != "�" else ".")
    return "".join(out)


def hex_text_lines(b, width=16):
    """Hexdump mit einer Textzeile unter jeder Hexzeile. Jedes Zeichen
    steht unter der ersten Ziffer seines Bytes."""
    out = []
    for k in range(0, len(b), width):
        chunk = b[k:k + width]
        out.append("   %04X  %s" % (k, chunk.hex(" ")))
        out.append("         %s" % "  ".join(to_text(chunk)))
    return out


# ---------------------------------------------------------- DDS-Meldungen
#
# Die Daten von DIA1 (Teil 2) bestehen aus einem Kopf, den Meldungssaetzen
# und danach den Klartexten. Nach der Notiz MVB-payload_Alstom und den
# Daten vom Laborbus (2026-10-12):
#
#   Kopf     14 Byte, Byte 12 = Zahl der Saetze
#   Satz     33 Byte, beginnt mit 00 80 00; der letzte hat nur 32 (ohne Nr),
#            unmittelbar dahinter beginnen die Texte
#   Texte    je Satz eine nullterminierte Zeichenkette (CP850),
#            "<Knoten>:<Stoerungsnummer>-<Text>"
#
# Satz, Offsets; Mehrbyte-Felder byteweise umgedreht ("invertiert"):
#    0..2   00 80 00         Kennung
#    3..4   EreignisNr       Ereignisnummer des Stoerspeichers, big-endian;
#                            faellt zu den aelteren hin, parallel zur
#                            Kommt-Zeit (aus den Daten abgeleitet)
#    5..6   ?                bisher immer 0000
#    7..8   MeldNr           Meldungsnummer, big-endian; laeuft je Knoten im
#                            Gleichschritt mit dem UIC-Code, keine
#                            Millisekunden (aus den Daten abgeleitet)
#    9..12  Kommt            UNIX-Sekunden, little-endian
#   13..16  Geht             dito; FFFFFFFF = keine Gehzeit
#   17..18  Trenner          FFFF
#   19..20  @   UIC-Code/Subsystemcode
#   21..22  -   UIC-Ref-Code 7xxx (Meldung ZP + Massnahme)
#   23..24  ^   UIC-Ref-Code 8xxx (Meldung Lf + Massnahme)
#   25..26  Z.  Fehlerzaehler
#   27..28  #   Stoerungsnummer; Bit 15 gesetzt, solange die Stoerung
#               ansteht (keine Gehzeit) - die Zahl darunter steht im Text
#   29      *   Knoten (z.B. 30 FLG2N, 2C FLG1N, 31 FLG2, 81 W_BT)
#   30      *   zweites Knotenbyte
#   31      P.  Prioritaet 10/20/30 = A/B/C
#   32      Nr  laufende Nummer (fehlt beim letzten Satz)

DDS_MARK = b"\x00\x80\x00"
DDS_REC = 33
DDS_HEAD = 14
NO_TIME = "--.--.---- --:--:--.---"


def dds_time(sec):
    if sec == 0xFFFFFFFF:
        return NO_TIME
    t = time.gmtime(sec)
    return time.strftime("%d.%m.%Y %H:%M:%S", t) + ".000"


def dds_parse(msg):
    """(Saetze, Texte) aus einer Nachricht von DIA1, oder None, wenn sie
    keine DDS-Meldungen enthaelt."""
    i0 = msg.find(DDS_MARK)
    if i0 < 0:
        return None
    n = 0
    while i0 + DDS_REC * n + 32 <= len(msg) and \
            msg[i0 + DDS_REC * n:i0 + DDS_REC * n + 3] == DDS_MARK:
        n += 1
    if not n:
        return None
    recs = []
    for k in range(n):
        r = msg[i0 + DDS_REC * k:i0 + DDS_REC * k + DDS_REC]
        u16 = lambda o: struct.unpack_from("<H", r, o)[0]
        u32 = lambda o: struct.unpack_from("<I", r, o)[0]
        recs.append({
            "ereignis": struct.unpack_from(">H", r, 3)[0],
            "meldnr": struct.unpack_from(">H", r, 7)[0],
            "kommt": u32(9), "geht": u32(13),
            "uic": u16(19), "ref7": u16(21), "ref8": u16(23),
            "zaehler": u16(25), "stoer": u16(27),
            "node": r[29], "node2": r[30], "prio": r[31],
            "nr": r[32] if k < n - 1 else k + 1,
            "roh": bytes(r[:DDS_REC if k < n - 1 else 32]),
        })
    texts = msg[i0 + DDS_REC * n - 1:].split(b"\x00")
    texts = [t.decode(TEXT_CP, errors="replace") for t in texts]
    return recs, texts, (msg[i0 - 2] if i0 >= 2 else None)


def dds_lines(recs, texts, kopf_n, quelle):
    """Die Saetze als Tabelle fuer die .txt-Datei."""
    prio = {0x10: "A", 0x20: "B", 0x30: "C"}
    out = ["DDS-Meldungen %s" % quelle,
           "%d Saetze%s. Zeiten: UNIX-Sekunden als UTC, ohne Millisekunden "
           "im Satz; Geht --.-- = keine Gehzeit (FFFFFFFF)."
           % (len(recs), "" if kopf_n in (None, len(recs))
              else " (Kopf meldet %d)" % kopf_n),
           "",
           "Spalten:",
           "  Nr          laufende Nummer in der Nachricht (Byte 32)",
           "  Kommt/Geht  UNIX-Zeit, Bytes 9-12 / 13-16",
           "  Status      aktiv = keine Gehzeit",
           "  Stoer       Stoerungsnummer ohne Bit 15, wie im Text "
           "(Bytes 27-28)",
           "  Knoten      Knotenbytes (29, 30), z.B. 30h FLG2N, 2Ch FLG1N, "
           "81h W_BT",
           "  P           Prioritaet A/B/C (Byte 31)",
           "  Zaehl       Fehlerzaehler (Bytes 25-26)",
           "  UIC/Sub     UIC-Code/Subsystemcode (Bytes 19-20)",
           "  7xxx/8xxx   UIC-Ref-Code ZP bzw. Lf (Bytes 21-22 / 23-24)",
           "  EreignisNr  Ereignisnummer des Stoerspeichers (Bytes 3-4) - "
           "aus den Daten abgeleitet: faellt",
           "              zu den aelteren Meldungen hin, parallel zur "
           "Kommt-Zeit",
           "  MeldNr      Meldungsnummer (Bytes 7-8) - aus den Daten "
           "abgeleitet: laeuft je Knoten im",
           "              Gleichschritt mit dem UIC-Code",
           "",
           "Nr  Kommt                    Geht                     Status    "
           "Stoer Knoten   P  Zaehl  UIC/Sub  7xxx  8xxx  EreignisNr  "
           "MeldNr  Text",
           "--  -----------------------  -----------------------  --------  "
           "----- -------  -  -----  -------  ----  ----  ----------  "
           "------  ----"]
    for k, r in enumerate(recs):
        out.append("%2d  %s  %s  %-8s  %5d %02Xh %02Xh  %s  %5d  %04X     "
                   "%4d  %4d  %10d  %6d  %s"
                   % (r["nr"], dds_time(r["kommt"]), dds_time(r["geht"]),
                      "aktiv" if r["geht"] == 0xFFFFFFFF else "gegangen",
                      r["stoer"] & 0x7FFF, r["node"], r["node2"],
                      prio.get(r["prio"], "%02X" % r["prio"]), r["zaehler"],
                      r["uic"], r["ref7"], r["ref8"], r["ereignis"],
                      r["meldnr"], texts[k] if k < len(texts) else ""))
    out += ["", "Rohdaten (33 Byte je Satz, der letzte ohne Nr):"]
    for r in recs:
        out.append("  " + r["roh"].hex(" "))
    return out


DDS_FILE = "dds_meldungen.txt"


def dds_save(msg, path, quelle, say=print):
    """DDS-Saetze einer Nachricht an path anhaengen; True, wenn welche drin
    waren."""
    p = dds_parse(msg)
    if p is None:
        say("   keine DDS-Saetze (00 80 00) in der Nachricht")
        return False
    recs, texts, kopf_n = p
    with open(path, "a", encoding="utf-8", errors="replace") as f:
        f.write("\n".join(dds_lines(recs, texts, kopf_n, quelle)) + "\n\n")
    say("== %d DDS-Meldungen -> %s" % (len(recs), path))
    return True


def auswerten(frames_path, out_path=None):
    """Nachrichten aus einer frames.txt wieder zusammensetzen und die
    DDS-Saetze schreiben - fuer Laeufe, die schon vorbei sind."""
    out_path = out_path or os.path.join(os.path.dirname(frames_path) or ".",
                                        DDS_FILE)
    if os.path.exists(out_path):
        os.remove(out_path)
    conns, found = {}, 0
    with open(frames_path) as fh:
        for line in fh:
            parts = line.split()
            if len(parts) < 3 or parts[1] != "RX":
                continue
            # Hexteile duerfen von Hand gegliedert sein
            f = bytes.fromhex("".join(parts[2:]))
            if len(f) != 32:
                continue
            d = decode(f)
            key = (d["src"], d["net"])
            if d["kind"] == "CR":
                conns[key] = {"ref": d["ref"], "len": d["len"],
                              "data": bytearray(d["data"]), "t": parts[0]}
            elif d["kind"] == "DT" and key in conns:
                conns[key]["data"] += d["data"]
            else:
                continue
            c = conns[key]
            if len(c["data"]) >= c["len"]:
                del conns[key]
                if dds_save(bytes(c["data"][:c["len"]]), out_path,
                            "aus %s, Geraet %d, Ref %04X, CR bei %s s"
                            % (frames_path, d["src"], c["ref"], c["t"])):
                    found += 1
    print("%d Nachricht(en) mit DDS-Meldungen" % found)
    return 0 if found else 1


def frame(tail):
    """32-Byte-Frame: Linkkopf leer, dahinter tail, mit Nullen aufgefuellt."""
    f = bytes(4) + tail
    return f + bytes(32 - len(f))


def swap_net(net):
    """Netzkopf der Antwort: Ziel und Quelle vertauscht."""
    return bytes((net[2], net[3], net[0], net[1]))


def decode(f):
    """Ein Frame in ein dict; Felder je nach Typ."""
    d = {
        "dst": ((f[0] & 0x0F) << 8) | f[1],
        "src": ((f[2] & 0x0F) << 8) | f[3],
        "sz": f[4],
        "net": bytes(f[5:9]),
        "mtc": f[9],
    }
    d["caller"] = bool(d["mtc"] & 0x80)
    t = (d["mtc"] >> 4) & 7
    d["kind"] = KINDS.get(t, "Typ%d" % t)
    end = 5 + d["sz"]
    if d["kind"] == "CR":
        d["ref"] = be16(f, 10)
        d["len"] = be32(f, 12)
        d["credit"] = f[16] >> 4
        d["data"] = bytes(f[18:end])
    elif d["kind"] == "CC":
        d["ref"] = be16(f, 10)
        d["credit"] = f[12] >> 4
    elif d["kind"] == "DT":
        d["seq"] = d["mtc"] & 7
        d["last"] = bool(d["mtc"] & 0x08)
        d["data"] = bytes(f[10:end])
    elif d["kind"] == "AK":
        d["seq"] = d["mtc"] & 7
    return d


def describe(d, names):
    n = lambda a: names.get(a, "Geraet %d" % a)
    s = "%s->%s  SZ %02X  Netz %s  MTC %02X %s%s" % (
        n(d["src"]), n(d["dst"]), d["sz"], d["net"].hex(), d["mtc"],
        d["kind"], " (Caller)" if d["caller"] else " (Replier)")
    if d["kind"] == "CR":
        s += "  Ref %04X  Laenge %d  Credit %d  Daten %s" % (
            d["ref"], d["len"], d["credit"], d["data"].hex())
    elif d["kind"] == "CC":
        s += "  Ref %04X  Credit %d" % (d["ref"], d["credit"])
    elif d["kind"] == "DT":
        s += "  Nr %d%s  Daten %s" % (d["seq"], " letztes" if d["last"]
                                       else "", d["data"].hex())
    elif d["kind"] == "AK":
        s += "  Nr %d" % d["seq"]
    return s


class Log:
    def __init__(self, path):
        # UTF-8 ausdruecklich: die Textzeilen enthalten Latin-1-Zeichen
        self.f = open(path, "w", encoding="utf-8", errors="replace")
        self.t0 = time.time()

    def line(self, s=""):
        out = "%7.3f  %s" % (time.time() - self.t0, s) if s else ""
        print(out)
        self.f.write(out + "\n")
        self.f.flush()

    def close(self):
        self.f.close()


def live_sa(mm):
    for sa in SA_CANDIDATES:
        mcr = struct.unpack_from("<H", mm, TM + sa + 0x384)[0]
        mcm = mcr & 7
        if mcr >> 11 == 5 and mcm < len(SA_FOR_MCM) and SA_FOR_MCM[mcm] == sa:
            return sa
    return None


def dti1():
    try:
        with open("/sys/module/pixy_mvblli/parameters/dbg_dti1") as f:
            return int(f.read().strip())
    except (OSError, ValueError):
        return None


def parse_args(argv):
    o = {"dauer": 60.0, "nur_hoeren": False, "keine_antwort": False,
         "ziel": DIA1_ADDR, "ref": 0x0043, "hoch": False, "single": False,
         "dsw_md": False, "ls": False, "alle": False, "vorlauf": None,
         "ef1": False, "cp": TEXT_CP}
    if len(argv) < 2 or argv[1].startswith("-"):
        print(__doc__)
        sys.exit(2)
    o["dir"] = argv[1]
    i = 2
    while i < len(argv):
        a = argv[i]
        if a == "--dauer":
            i += 1
            o["dauer"] = float(argv[i])
        elif a == "--ziel":
            i += 1
            o["ziel"] = int(argv[i], 0)
        elif a == "--ref":
            i += 1
            o["ref"] = int(argv[i], 16)
        elif a == "--nur-hoeren":
            o["nur_hoeren"] = True
        elif a == "--keine-antwort":
            o["keine_antwort"] = True
        elif a == "--hoch":
            o["hoch"] = True
        elif a == "--single-line":
            o["single"] = True
        elif a == "--dsw-md":
            o["dsw_md"] = True
        elif a == "--ef1":
            o["ef1"] = True
        elif a == "--zeichensatz":
            i += 1
            try:
                o["cp"] = codecs.lookup(argv[i]).name
            except LookupError:
                print("Unbekannter Zeichensatz: %s (z.B. cp850, cp437, "
                      "latin1, cp1252)\n" % argv[i])
                sys.exit(2)
        elif a == "--lebenszeichen":
            o["ls"] = True
        elif a == "--alle-quellen":
            o["alle"] = True
        elif a == "--vorlauf":
            i += 1
            o["vorlauf"] = float(argv[i])
        else:
            print("Unbekannte Option: %s\n" % a)
            print(__doc__)
            sys.exit(2)
        i += 1
    if o["alle"] and not o["ls"]:
        print("--alle-quellen nur zusammen mit --lebenszeichen\n")
        sys.exit(2)
    if o["vorlauf"] is None:
        o["vorlauf"] = 20.0 if o["ls"] else 2.0
    return o


def main():
    global TEXT_CP
    if len(sys.argv) >= 3 and sys.argv[1] == "--auswerten":
        return auswerten(sys.argv[2], sys.argv[3] if len(sys.argv) > 3
                         else None)
    o = parse_args(sys.argv)
    TEXT_CP = o["cp"]
    try:
        codecs.lookup(TEXT_CP)
    except LookupError:
        # cp850 liegt in python3-codecs; fehlt das Paket, wenigstens Latin-1
        print("Zeichensatz %s fehlt im Python des Images - nehme latin-1"
              % TEXT_CP)
        TEXT_CP = "latin-1"
    # Umlaute in den Textzeilen duerfen eine reine ASCII-Konsole nicht
    # zum Absturz bringen
    try:
        sys.stdout.reconfigure(errors="replace")
    except AttributeError:
        pass
    os.makedirs(o["dir"], exist_ok=True)
    log = Log(os.path.join(o["dir"], "log.txt"))
    raw = open(os.path.join(o["dir"], "frames.txt"), "w")
    names = {OWN_ADDR: "IDU2", o["ziel"]: "DIA1"}
    typ = MD_HIGH if o["hoch"] else MD_LOW

    log.line("DDS-Abfrage an DIA1 (Geraet %d = 0x%03X), eigene Adresse %d, "
             "%s, %s, %s, Vorlauf %g s"
             % (o["ziel"], o["ziel"], OWN_ADDR, "nur hoeren"
                if o["nur_hoeren"] else "Anfrage Ref %04X" % o["ref"],
                "Prioritaet hoch" if o["hoch"] else "Prioritaet niedrig",
                ("Lebenszeichen 491" + (" + 492/498" if o["alle"] else ""))
                if o["ls"] else "ohne Lebenszeichen", o["vorlauf"]))

    try:
        fd = os.open(DEV, os.O_RDWR)
    except OSError as e:
        log.line("%s nicht zu oeffnen: %s%s" % (
            DEV, os.strerror(e.errno),
            " - laeuft der Dienst? systemctl stop idu2-mvb"
            if e.errno == errno.EBUSY else ""))
        raw.close()
        log.close()
        return 1

    def io(req, arg=0, mutate=False):
        if isinstance(arg, bytearray):
            return fcntl.ioctl(fd, sig(req), arg, mutate)
        return fcntl.ioctl(fd, sig(req), arg)

    def pd_write(port, data):
        """Prozessdaten schreiben. Der Treiber liest immer die vollen 38
        Byte, als Laenge sieht er die Portgroesse."""
        buf = bytearray(38)
        struct.pack_into("<HH", buf, 0, PD, port)
        buf[4:4 + len(data)] = data
        return os.writev(fd, [memoryview(buf)[:len(data)]])

    def pd_read(port, size):
        """(Daten, freshness) oder (None, Fehlertext)."""
        buf = bytearray(38)
        struct.pack_into("<HH", buf, 0, PD, port)
        try:
            os.readv(fd, [memoryview(buf)[:size]])
        except OSError as e:
            return None, errno.errorcode.get(e.errno, e.errno)
        return bytes(buf[4:4 + size]), struct.unpack_from("<H", buf, 36)[0]

    ls = {"wert": 0, "n": 0, "fehler": 0}

    def ls_send():
        """Lebenszeichen big-endian ab Byte 0, Byte 2/3 (Luefterbefehle,
        Reserve) bleiben 0."""
        try:
            pd_write(LS_PORT, struct.pack(">HH", ls["wert"], 0))
            ls["n"] += 1
        except OSError as e:
            ls["fehler"] += 1
            if ls["fehler"] == 1:
                log.line("Lebenszeichen: write FEHLER %s"
                         % errno.errorcode.get(e.errno, e.errno))

    def step(name, fn):
        try:
            fn()
            log.line("%-20s ok" % name)
            return True
        except OSError as e:
            log.line("%-20s FEHLER %s" % (name, errno.errorcode.get(
                e.errno, e.errno)))
            return False

    # Erst nach open() ist das TM-Fenster eingerichtet
    fb = os.open(BOARD, os.O_RDWR)
    try:
        mm = mmap.mmap(fb, BAR_SIZE, mmap.MAP_SHARED, mmap.PROT_READ)
    finally:
        os.close(fb)

    started = False
    try:
        step("WRITE_DEV_ADDR", lambda: io(IOC["WRITE_DEV_ADDR"], OWN_ADDR))
        step("HWINIT", lambda: io(IOC["HWINIT"], bytearray(struct.pack(
            "<QBBHHB x", 0, 1, 1, 0xFFF, 0xFFF, 0)), True))
        # q_tq_priority: negativ -> Anmeldung ueber EF1 (MR 2000), sonst
        # ueber EF0 (MR 1000). Laut IEC 61375-3-1 nimmt ein Geraet nur an
        # Event_Rounds der passenden Prioritaet teil (ET 0 hoch, 1 niedrig).
        prio = 0xFFFF if o["ef1"] else 0
        step("MD_CONF (%s)" % ("EF1" if o["ef1"] else "EF0"),
             lambda: io(IOC["MD_CONF"],
                        bytearray(struct.pack("<H", prio)), True))
        if o["ls"]:
            ports = [(LS_PORT, LS_SIZE, PD_SRC),
                     (FLG_LS_PORT, FLG_LS_SIZE, PD_SINK)]
            if o["alle"]:
                ports += [(p, s, PD_SRC) for p, s in EXTRA_SRC]
            cfg = struct.pack("<H", len(ports))
            for p, s, t in ports:
                cfg += struct.pack("<HHH", p, s, t)
            step("PD_CONF %s" % " ".join(
                "%d%s" % (p, "Q" if t == PD_SRC else "S")
                for p, s, t in ports),
                lambda: io(IOC["PD_CONF"], bytearray(cfg), True))
            # Quellen werden erst mit dem ersten write() aktiv
            ls_send()
            log.line("Lebenszeichen 491: erster Wert %04X geschrieben"
                     % ls["wert"])
            if o["alle"]:
                for p, s in EXTRA_SRC:
                    step("write %d (%d Byte Nullen)" % (p, s),
                         lambda p=p, s=s: pd_write(p, bytes(s)))
        cmd = CTRL_CLEAR | (CTRL_LINE_A if o["single"] else 0)
        step("WRITE_CONTROL", lambda: io(IOC["WRITE_CONTROL"], bytearray(
            struct.pack("<HHBx", OWN_ADDR, 43, cmd)), True))
        if o["dsw_md"]:
            step("WRITE_DSW MD", lambda: io(IOC["WRITE_DSW"],
                                            (DSW_MD << 16) | DSW_MD))
        started = step("START", lambda: io(IOC["START"]))
        if not started:
            return 1

        sa = live_sa(mm)
        if sa is None:
            log.line("Kein Registerblock gefunden - Abbruch")
            return 1

        def qdt():
            return struct.unpack_from("<3H", mm, TM + sa + QDT)

        def mr():
            """MR: 1000/2000 = Ereignis ueber EF0/EF1 angemeldet, das der
            Busmaster mit einer Ereignisabfrage abholen soll; 0200 Busy.
            Erste Cachezeile, unbedenklich."""
            return struct.unpack_from("<H", mm, TM + sa + 0x3A0)[0]

        # Vergleichsstand der Queues. Wird nach jedem Einhaengen neu
        # gesetzt: beim ersten Senden traegt der Treiber die Sendequeues
        # erst in die QDT ein (0000 -> 2000/2050) - das ist kein Senden.
        qs = {"q": qdt(), "sent": False}

        def dsw():
            b = bytearray(2)
            try:
                io(IOC["READ_DSW"], b, True)
                return "%04X" % struct.unpack("<H", b)[0]
            except OSError:
                return "?"

        def tx(tail, note):
            # Hat sich die Sendequeue seit dem letzten Stand bewegt, zaehlt
            # das, bevor das neue Einhaengen den Vergleichsstand verschiebt
            q0 = qdt()
            if q0[:2] != qs["q"][:2]:
                qs["sent"] = True
                log.line("QDT  xmit0 %04X xmit1 %04X rcve %04X  "
                         "(Sendequeue bewegt - gesendet)" % q0)
            f = frame(tail)
            buf = bytearray(38)
            struct.pack_into("<HH", buf, 0, typ, o["ziel"])
            buf[4:36] = f
            try:
                os.writev(fd, [memoryview(buf)[:32]])
                ok = "eingehaengt"
            except OSError as e:
                ok = "FEHLER %s" % errno.errorcode.get(e.errno, e.errno)
            # Linkkopf so, wie der Treiber ihn setzt - fuer die Anzeige
            shown = bytes((0x10 | (o["ziel"] >> 8) & 0xF, o["ziel"] & 0xFF,
                           0x80 | (OWN_ADDR >> 8) & 0xF, OWN_ADDR & 0xFF)) \
                + f[4:]
            log.line("TX %s  %s  [%s]" % (describe(decode(shown), names),
                                         note, ok))
            raw.write("%.3f TX %s\n" % (time.time() - log.t0, shown.hex()))
            raw.flush()
            qs["q"] = qdt()
            log.line("   nach dem Einhaengen: QDT xmit0 %04X xmit1 %04X "
                     "rcve %04X  MR %04X - gesendet ist erst, wenn sich "
                     "xmit danach bewegt" % (qs["q"] + (mr(),)))

        def rx():
            buf = bytearray(38)
            struct.pack_into("<HH", buf, 0, typ, o["ziel"])
            os.readv(fd, [memoryview(buf)[:32]])
            return bytes(buf[4:36])

        # Eingehende Messages je Verbindung (Quelle + Netzkopf)
        conns = {}
        got = {"rx": 0, "tx": 0, "antwort1": None, "daten2": None,
               "cc_anfrage": False, "cc_reply": False, "fremd": {}}
        reply_ref = (o["ref"] + 1) & 0xFFFF

        def handle(f):
            d = decode(f)
            got["rx"] += 1
            log.line("RX %s" % describe(d, names))
            if d.get("data"):
                log.line("   Text   %s" % to_text(d["data"]))
            raw.write("%.3f RX %s\n" % (time.time() - log.t0, f.hex()))
            raw.flush()
            # Kommuniziert wird nur mit DIA1. Rufe anderer Geraete (z.B.
            # WTB1, Geraet 6) werden protokolliert, aber nicht beantwortet -
            # sonst stuenden deren CC/AK in der Sendequeue vor der Anfrage.
            if d["src"] != o["ziel"]:
                got["fremd"][d["src"]] = got["fremd"].get(d["src"], 0) + 1
                log.line("   nicht von DIA1 - keine Antwort")
                return
            back = swap_net(d["net"])
            side = 0x00 if d["caller"] else 0x80   # wir: Gegenseite
            key = (d["src"], d["net"])

            if d["kind"] == "CR":
                # Verbindungswunsch von DIA1: Call (0x80) oder Reply (0x00)
                tx(bytes((0x08,)) + back + bytes((side | 0x41,))
                   + struct.pack(">H", d["ref"]) + bytes((CC_CREDIT,)),
                   "CC auf %s Ref %04X" % ("Call" if d["caller"] else
                                           "Reply", d["ref"]))
                conns[key] = {"ref": d["ref"], "len": d["len"],
                              "data": bytearray(d["data"]),
                              "call": d["caller"], "net": d["net"]}
                check_done(key)
            elif d["kind"] == "DT":
                tx(bytes((0x05,)) + back
                   + bytes((side | 0x60 | ((d["seq"] + 1) & 7),)), "AK")
                c = conns.get(key)
                if c is None:
                    log.line("   DT ohne vorheriges CR - nicht zuzuordnen")
                    return
                c["data"] += d["data"]
                check_done(key)
            elif d["kind"] == "CC":
                if d["ref"] == o["ref"] and not got["cc_anfrage"]:
                    got["cc_anfrage"] = True
                    log.line("   DIA1 nimmt die Anfrage an - sende den Rest")
                    tx(CALL_DT, "DT Rest der Anfrage")
                elif d["ref"] == reply_ref:
                    got["cc_reply"] = True
                    log.line("   DIA1 hat unsere Reply-Message bestaetigt")
            elif d["kind"] == "AK":
                log.line("   quittiert")

        def check_done(key):
            c = conns[key]
            if len(c["data"]) < c["len"]:
                return
            msg = bytes(c["data"][:c["len"]])
            del conns[key]
            if c["call"]:
                got["daten2"] = msg
                log.line("== Daten von DIA1 vollstaendig: %d Byte, Ref %04X"
                         % (len(msg), c["ref"]))
            else:
                got["antwort1"] = msg
                log.line("== Antwort auf die Anfrage: %d Byte, Ref %04X"
                         % (len(msg), c["ref"]))
            for ln in hex_text_lines(msg):
                log.line(ln)
            if c["call"] and not o["keine_antwort"]:
                net = swap_net(c["net"])
                tx(bytes((0x17,)) + net + bytes((0x00,))
                   + struct.pack(">HI", reply_ref, len(REPLY_DATA))
                   + bytes((0x70, 0x00)) + REPLY_DATA + REPLY_PAD,
                   "Reply an DIA1 Ref %04X" % reply_ref)
            # Erst nach der Reply: das Schreiben soll DIA1 nicht aufhalten
            if c["call"]:
                dds_save(msg, os.path.join(o["dir"], DDS_FILE),
                         "von DIA1, Ref %04X, empfangen %s"
                         % (c["ref"], time.strftime("%d.%m.%Y %H:%M:%S")),
                         log.line)

        def ls_text():
            """Eigenes und FLG1N-Lebenszeichen fuer die Statuszeile."""
            if not o["ls"]:
                return ""
            data, fresh = pd_read(FLG_LS_PORT, FLG_LS_SIZE)
            flg = ("%04X fresh %d" % (be16(data, 0), fresh) if data
                   else str(fresh))
            return "  LS491 %04X  FLG181 %s" % (ls["wert"], flg)

        d_last = dti1()
        log.line("Status  QDT xmit0 %04X xmit1 %04X rcve %04X  MR %04X  "
                 "DSW %s  dbg_dti1 %s%s"
                 % (qs["q"] + (mr(), dsw(), d_last, ls_text())))

        t0 = time.time()
        call_sent = o["nur_hoeren"]
        next_status = t0 + 5
        next_ls = t0 + 1
        while time.time() - t0 < o["vorlauf"] + o["dauer"]:
            # Lebenszeichen im Sekundentakt, wie die Herstelleranwendung
            if o["ls"] and time.time() >= next_ls:
                ls["wert"] = (ls["wert"] + 1) & 0xFFFF
                ls_send()
                next_ls += 1
            # Erst nach dem Vorlauf: der Busmaster und die FLG sollen das
            # Geraet (und sein Lebenszeichen) vorher sehen
            if not call_sent and time.time() - t0 >= o["vorlauf"]:
                tx(CALL_CR[:5] + CALL_CR[5:6] + struct.pack(">H", o["ref"])
                   + CALL_CR[8:], "Anfrage an DIA1")
                call_sent = True
            while True:
                try:
                    handle(rx())
                except OSError as e:
                    if e.errno not in (errno.ENOBUFS, errno.EAGAIN):
                        log.line("read: %s" % errno.errorcode.get(e.errno,
                                                                  e.errno))
                    break
            q = qdt()
            if q != qs["q"]:
                what = []
                if q[:2] != qs["q"][:2]:
                    what.append("Sendequeue bewegt - gesendet")
                    qs["sent"] = True
                if q[2] != qs["q"][2]:
                    what.append("Empfangsqueue bewegt")
                log.line("QDT  xmit0 %04X xmit1 %04X rcve %04X  (%s)"
                         % (q + (", ".join(what),)))
                qs["q"] = q
            if time.time() >= next_status:
                d_now = dti1()
                log.line("Status  QDT xmit0 %04X xmit1 %04X rcve %04X  "
                         "MR %04X  DSW %s  dbg_dti1 %s%s"
                         % (q + (mr(), dsw(), d_now, ls_text())))
                next_status += 5
            time.sleep(0.01)

        # Zusammenfassung
        log.line()
        log.line("Zusammenfassung")
        d_end = dti1()
        log.line("   Frames empfangen %d, dbg_dti1 %s -> %s"
                 % (got["rx"], d_last, d_end))
        if o["ls"]:
            log.line("   Lebenszeichen 491: %d mal geschrieben, letzter Wert "
                     "%04X, Fehler %d" % (ls["n"], ls["wert"], ls["fehler"]))
        if not o["nur_hoeren"]:
            log.line("   Anfrage hat das Geraet verlassen: %s"
                     % ("ja" if qs["sent"] else
                        "NEIN - Sendequeue nach dem Einhaengen unveraendert, "
                        "der Busmaster hat nicht abgeholt (MR %04X)" % mr()))
            log.line("   Anfrage angenommen (CC):      %s"
                     % ("ja" if got["cc_anfrage"] else "NEIN"))
            log.line("   Antwort auf die Anfrage:      %s"
                     % (got["antwort1"].hex() if got["antwort1"] else "keine"))
        log.line("   Daten von DIA1 (Teil 2):      %s"
                 % ("%d Byte" % len(got["daten2"]) if got["daten2"]
                    else "keine"))
        if got["daten2"] and not o["keine_antwort"]:
            log.line("   Reply bestaetigt (CC):        %s"
                     % ("ja" if got["cc_reply"] else "NEIN"))
        for key, c in conns.items():
            log.line("   unvollstaendig: Ref %04X, %d von %d Byte"
                     % (c["ref"], len(c["data"]), c["len"]))
        for src, n in sorted(got["fremd"].items()):
            log.line("   Frames anderer Geraete (unbeantwortet): Geraet %d "
                     "(0x%03X): %d" % (src, src, n))
        return 0
    finally:
        if started:
            try:
                io(IOC["STOP"])
            except OSError:
                pass
        if o["dsw_md"]:
            try:
                io(IOC["WRITE_DSW"], DSW_MD << 16)
            except OSError:
                pass
        os.close(fd)
        raw.close()
        log.close()


sys.exit(main())
