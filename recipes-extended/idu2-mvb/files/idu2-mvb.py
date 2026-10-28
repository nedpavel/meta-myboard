#!/usr/bin/env python3
"""IDU am MVB: Lebenszeichen ab dem Start, Prozessdaten in MariaDB,
DDS-Abfrage an DIA1 auf Anfrage.

Der Dienst haelt das Display dauernd am Bus, als IDU2 ("IDU Wagen",
Geraet 240 = F0H, Vorgabe) oder mit --rolle IDU1 als IDU1 ("IDU
Traktion", Geraet 238 = EEH). Weil /dev/mvblli0 exklusiv ist, laeuft alles
ueber ihn - auch die DDS-Abfrage der Bedienoberflaeche. Die kommt so ohne
Initialisierungszeit aus: der Controller laeuft schon, das Lebenszeichen
auch. Als IDU1 nur, wenn kein echtes IDU1 am Bus haengt: sonst senden zwei
Geraete auf denselben Ports.

Lebenszeichen laut Busbeschreibung (MVB (trak).txt): IDU2-XMVBLifeSig auf
Port 491 (4 Byte, Zyklus 256 ms) bzw. IDU1-XMVBLifeSig auf Port 481
(8 Byte, 256 ms), CARD16 big-endian ab Byte 0, gelesen von FLG1N, FLG2N
und dem anderen IDU, im Sekundentakt hochgezaehlt. Ohne laufendes
Lebenszeichen gilt das IDU fuer FLG und DIA1 als nicht da.

Source-Ports: alle drei der Rolle (IDU2: 491, 492, 498; IDU1: 481, 488,
489; mit --nur-lebenszeichen nur den ersten), gesendet im Takt ihrer
Zykluszeit und sofort nach jeder Aenderung. Jedes Signal darin hat einen
Wert - Vorgabe 0 bzw. leer, fuer Befehle und Softkeys "nicht gedrueckt" -,
fest gesetzt mit --wert NAME=WERT (z. B. Software-Version, Fahrzeugname)
oder zur Laufzeit ueber den Socket (Bildnummer, Softkeys als Puls).

Einrichtung wie im erfolgreichen Lauf stufe8/ef1 (2026-10-12): Adresse
240, Einleitungsbetrieb auf Leitung A, MD-Bit im DSW, Anmeldung von
Messages ueber EF1 (MD_CONF mit negativer Prioritaet).

Prozessdaten: Die Sink-Ports der Rolle (wo das IDU in der Busbeschreibung
als $SINKMEMB steht; IDU2 69 Ports, IDU1 70) richtet der Dienst als Senken
ein und liest sie im Takt ihrer Zykluszeit (64..1024 ms). Fuer
Prozessdaten gibt es keinen Interrupt - der Controller schreibt sie selbst
in den Traffic Store, read() liefert den Stand oder ETIMEDOUT, wenn er
aelter als die Sink-Time-Schwelle ist. Jedes Signal steht mit Beschreibung
in myboard.mvb_prozessdaten, dazu die gesendeten Signale der Rolle. Jede
Lesung wird geschrieben (Wert, frisch, gelesen; geaendert bei neuem Wert),
in einem eigenen Thread; die Tabelle ist eine MEMORY-Tabelle, damit rund
300 Schreibvorgaenge pro Sekunde nicht auf die CFast gehen. Die
Signalliste (/usr/share/idu2-mvb/mvb-signale.tsv) erzeugt mvbtrak2tsv.py
beim Bauen aus der Busbeschreibung; fehlt sie, laeuft der Dienst ohne
Prozessdaten.

DDS-Abfrage: dieselben Frames wie ddsabfrage.py (Mitschnitt des Displays).
IDU2 schickt die Anfrage an DIA1 (Geraet 46), beantwortet CC/AK und die
Reply-Message von DIA1, nimmt die Daten (Teil 2) entgegen und quittiert
sie mit der Reply des Displays. Geantwortet wird nur auf Frames von
DIA1. Ruft DIA1 von sich aus an, beantwortet der Dienst das ebenfalls und
haelt das Ergebnis bereit.

Steuerung ueber den Unix-Socket /run/idu2-mvb.sock, je Anfrage und
Antwort eine JSON-Zeile:

    {"befehl": "dds", "seite": N}  Seite N der DDS-Meldungen, schon
                                   geholte aus dem Zwischenspeicher
    {"befehl": "dds", "seite": 1, "neu": true}
                           Seite 1 neu von DIA1 (auch ohne "seite")
    {"befehl": "letzte"}   letztes Ergebnis, ohne neue Abfrage
    {"befehl": "status"}   Rolle, Lebenszeichen, Frames, laufende Abfrage,
                           Prozessdaten-Zaehler
    {"befehl": "setzen", "signal": "IDU2-XScreenNum", "wert": 3}
                           Source-Signal setzen; mit "dauer": S als Puls,
                           danach wieder der vorige Wert
    {"befehl": "quellen"}  aktuelle Werte aller Source-Signale
    {"befehl": "softkeys"} Softkeys der Rolle in Gruppen, mit Zustand, und
                           ihre Rueckmeldungen (Port 290) - fuer die Seite
                           "Softkeys" der Oberflaechen
    {"befehl": "softkey", "signal": "IDU2-BTestSound"}
                           Softkey druecken: Puls von --softkey-dauer s;
                           nur Signale aus der Softkey-Liste

Aufbau der Anfrage laut "MVB Dokumentation IC2000 Payload Structure"
(Abschnitt 4.2.2.1, dort IDU1 -> VCU1): Datenbytes 0-4 Header, 5 Source,
6 Dest, 8 Command, 10-11 Cache_index (Mitschnitt 00 01), 13 Nr FC
(Anzahl), 14-17 Startzeit, 18-21 Endzeit. Commands: 01 REQ_FC erste Seite,
03 INV_CACHE, 05 REQ_NEXT_PAGE_FC naechste Seite; DIA1 antwortet mit FB
ANS_FC, Cache_index und Anzahl im Kopf. Je Anfrage
liefert DIA1 hoechstens 25 Meldungen; Seiten zu 16 (--anzahl; das
Original-Display fragt 21 an).

Blaettern: Seite 1 = REQ_FC genau wie im Mitschnitt. Jede weitere Seite =
REQ_NEXT_PAGE_FC, aufgebaut laut REQ_NEXT_PAGE_FC.txt (22 Byte): Header mit
Command 05, Cache_index (aus der Antwort auf die Seite davor), Anzahl, dann
00 80 00, Address und 4 Byte Unk - die ersten 9 Byte des Satzes der
letzten gezeigten Meldung (der letzten der Seite davor). DIA1 ueberspringt
diese Meldung und liefert die naechsten. Cache_index ist die Version des
DDS-Caches bei DIA1; er aendert sich nur, wenn neue Meldungen dazukommen,
und wird beim Blaettern unveraendert zurueckgeschickt, nie hochgezaehlt.
Fuer die vorige Seite gibt es kein Command: schon geholte Seiten kommen aus
dem Zwischenspeicher des Dienstes, bis Seite 1 neu abgefragt wird.

MariaDB: Jede DDS-Antwort von DIA1 landet in myboard.dds_meldungen (Stand
in myboard.dds_stand). Hat die Antwort einen anderen Cache_index als die
Tabelle, wird ihr Inhalt ersetzt - DIA1 hat neuere Meldungen, oder Seite 1
wurde neu abgefragt. Beim Blaettern (gleicher Cache_index) kommen die
Seiten dazu. Geschrieben wird in einem eigenen Thread ueber PyMySQL, als
root ueber den Unix-Socket: fehlt MariaDB oder ist sie langsam, laeuft das
Lebenszeichen ungestoert weiter, und die Seiten werden nachgetragen,
sobald die Datenbank da ist.

/dev/mvblli0 ist exklusiv. Fuer ddsabfrage.py und mvbdiff.py:

    systemctl stop idu2-mvb     ...     systemctl start idu2-mvb

Optionen (auch ueber OPTIONEN in /etc/default/idu2-mvb):
    --zwei-leitungen   Zweileitungsbetrieb statt Leitung A allein
    --ohne-dsw-md      MD-Bit im DSW nicht setzen
    --ef0              Messages ueber EF0 statt EF1 anmelden
    --takt S           Lebenszeichen alle S Sekunden erhoehen (Vorgabe 1)
    --rolle IDU1|IDU2  Rolle am Bus (Vorgabe IDU2)
    --nur-lebenszeichen  nur den Port des Lebenszeichens senden, nicht die
                       weiteren Source-Ports der Rolle
    --wert NAME=WERT   Wert eines Source-Signals (mehrfach), z. B.
                       --wert IDU2-XSWVersion=0x0102; nach den Werten aus
    --werte-datei PFAD (Vorgabe /etc/idu2-mvb/quellen.conf, eine Zeile
                       NAME=WERT je Signal, Leerzeichen im Wert erlaubt)
    --socket PFAD      Steuersocket (Vorgabe /run/idu2-mvb.sock)
    --anzahl N         DDS-Meldungen je Seite, 1..255 (Vorgabe 16; das
                       Original-Display fragt 21 an, DIA1 liefert
                       hoechstens 25)
    --softkey-dauer S  Dauer eines Softkey-Pulses (Vorgabe 1 s)
    --ohne-pd          Sink-Ports nicht einrichten, keine Prozessdaten
    --signale PFAD     Signalliste (Vorgabe /usr/share/idu2-mvb/
                       mvb-signale.tsv)
    --ohne-db          nichts in MariaDB speichern (DDS und Prozessdaten)
    --db-socket PFAD   Socket von MariaDB (Vorgabe /var/lib/mysql/mysql.sock)

Eine Anfrage ueber den Socket kann die Anzahl mit "anzahl" ueberschreiben.
"""

import datetime
import errno
import fcntl
import json
import os
import pwd
import queue
import select
import signal
import socket
import struct
import sys
import threading
import time

DEV = "/dev/mvblli0"
SOCK = "/run/idu2-mvb.sock"
DB_SOCK = "/var/lib/mysql/mysql.sock"
SIGNALE = "/usr/share/idu2-mvb/mvb-signale.tsv"
WERTE = "/etc/idu2-mvb/quellen.conf"
DIA1_ADDR = 0x2E
ABFRAGE_TIMEOUT = 15.0

IOC = {
    "WRITE_DEV_ADDR": 0x40024C02,
    "WRITE_DSW":      0x40044C04,
    "START":          0x00004C05,
    "STOP":           0x00004C06,
    "PD_CONF":        0x40024C0C,
    "MD_CONF":        0x40024C0E,
    "WRITE_CONTROL":  0x40064C14,
    "HWINIT":         0x40104C15,
}
PD, MD_LOW = 1, 3
PD_SINK, PD_SRC = 1, 2
# Rolle laut Busbeschreibung: Geraeteadresse, Port des Lebenszeichens
# (XMVBLifeSig ab Byte 0), weitere Source-Ports - je (Port, Laenge in Byte,
# Zykluszeit in ms)
ROLLEN = {
    "IDU1": (0xEE, (481, 8, 256), ((488, 8, 1024), (489, 16, 1024))),
    "IDU2": (0xF0, (491, 4, 256), ((492, 8, 256), (498, 16, 1024))),
}

# Softkeys der Bedienoberflaeche (Seite "Softkeys" in Qt und Web): Gruppe,
# dann je Taste das Source-Signal ohne Rollen-Praefix und die Beschriftung
# ("\n" bricht um). Die Oberflaeche zeigt nur, was die Rolle sendet - IDU2
# alle Gruppen, IDU1 nur die Luefter. Jeder Druck ist ein Puls
# (--softkey-dauer, Vorgabe 1 s wie "Puls 1s" in der Busbeschreibung).
SOFTKEYS = (
    ("Türen-Test", (
        ("BDrTstStart", "Testmodus\nStart"),
        ("BDrTstStop", "Testmodus\nEnde"),
        ("BDrTestCls1", "Testschliessung\nprimär"),
        ("BDrTestCls2", "Testschliessung\nredundant"),
        ("BDrTestOpen", "Testöffnung"))),
    ("Visiteur", (
        ("BTestSound", "Testton"),
        ("BTestPict", "Testbild"),
        ("BTestLamps", "Test\nLeselampen"),
        ("BTrainRes", "Zug-\nreservation"),
        ("LSumWinMode", "Sommer /\nWinter"),
        ("LBlkRelCool", "Kälte sperren /\nfreigeben"),
        ("BResNrToggl", "Reserv.-Nr.\n1–7 ↔ 11–17"))),
    ("Eigener Zug", (
        ("BTrLightOn", "Licht ein"),
        ("BTrLightOff", "Licht aus"),
        ("BTrLightHlf", "Licht halb"),
        ("BTrHLKOn", "HLK ein"),
        ("BTrHLKOff", "HLK aus"),
        ("BTrTempUp", "Temperatur +"),
        ("BTrTempDn", "Temperatur −"))),
    ("Ganzer Zug", (
        ("BDTrLightOn", "Licht ein"),
        ("BDTrLighOff", "Licht aus"),
        ("BDTrLighHlf", "Licht halb"),
        ("BDTrHLKOn", "HLK ein"),
        ("BDTrHLKOff", "HLK aus"),
        ("BDTrTempUp", "Temperatur +"),
        ("BDTrTempDn", "Temperatur −"))),
    ("Klima-Prüflauf", tuple(
        ("BTstCool%d#%d" % (g, w), "Wagen %d\nGerät %d" % (w, g))
        for w in (1, 2, 3, 4) for g in (1, 2))),
    ("Lüfter", (
        ("BSrVent1Up", "Stromrichter 1\nauf"),
        ("BSrVent1Dn", "Stromrichter 1\nab"),
        ("BSrVent2Up", "Stromrichter 2\nauf"),
        ("BSrVent2Dn", "Stromrichter 2\nab"),
        ("BTrVent1Up", "Trafo 1\nauf"),
        ("BTrVent1Dn", "Trafo 1\nab"),
        ("BTrVent2Up", "Trafo 2\nauf"),
        ("BTrVent2Dn", "Trafo 2\nab"))),
)
# Rueckmeldungen zu den Softkeys: Sink-Signale aus Port 290 (FLG2N)
RUECKMELDUNGEN = (
    ("TT-MTrLightOn", "Licht ein"),
    ("TT-MTrLightHlf", "Licht halb"),
    ("TT-MKomfHLKOn", "Komfort-HLK"),
    ("TT-MSummerMode", "Sommer"),
    ("TT-MWinterMode", "Winter"),
    ("TT-MTrReserved", "reserviert"),
    ("TT-MReadLTstOn", "Test Leselampen"),
    ("TT-MPISSoundTst", "Testton"),
    ("TT-MPISDispTst", "Testbild"),
    ("TT-MCoolBlocked", "Kälte gesperrt"),
    ("TT-MCoolReleased", "Kälte frei"),
    ("TT-MResNrHigh", "Res.-Nr. 11–17"),
)
DSW_MD = 0x1000
CTRL_CLEAR, CTRL_LINE_A = 0x03, 0x08

# ---------------------------------------------------------- Message-Frames
# Aufbau wie in ddsabfrage.py: 0..3 Linkkopf (setzt der Treiber), 4 SZ,
# 5..8 Netzkopf, 9 MTC (Bit 7 Caller-Seite, Bits 6..4 Typ 0 CR / 4 CC /
# 1 DT / 6 AK, bei DT Bit 3 letztes Paket und Bits 2..0 Folgenummer);
# CR: 10..11 Ref, 12..15 Laenge, 16 Credit, 18.. Daten; CC: 10..11 Ref,
# 12 Credit; DT: 10.. Daten.

KINDS = {0: "CR", 4: "CC", 1: "DT", 6: "AK"}

# Aus dem Mitschnitt des Displays, ab Offset 4 (SZ). Ref wird ersetzt.
CALL_CR = bytes.fromhex(
    "1b 00e00000 80 0043 0000001c 70 00 0100000001 0501010101 00010415"
    .replace(" ", ""))
CALL_DT = bytes.fromhex(
    "13 00e00000 98 6000475200000100420101200000 0101010100010415"
    .replace(" ", ""))
REPLY_DATA = bytes.fromhex("0100000000010501fb01")
REPLY_PAD = bytes.fromhex("00010415")
CC_CREDIT = 0x10

# Commands der DDS-Abfrage (Datenbyte 8), laut IC2000-Payload-Doku
REQ_FC, INV_CACHE, REQ_NEXT_PAGE_FC = 0x01, 0x03, 0x05


def next_page_data(cache, anzahl, satz):
    """Daten von REQ_NEXT_PAGE_FC (REQ_NEXT_PAGE_FC.txt): Header wie REQ_FC
    mit Command 05, dann Cache_index (16 bit BE), Nb_fault_codes (8 bit),
    00 80 00, Address (16 bit BE), Unk (4 Byte) - die letzten drei sind
    die ersten 9 Byte des Satzes der letzten gezeigten Meldung. DIA1 macht
    hinter dieser Meldung weiter."""
    return (CALL_CR[14:22] + bytes((REQ_NEXT_PAGE_FC,)) + CALL_CR[23:24]
            + struct.pack(">HB", cache, anzahl) + satz)


def request_frames(ref, daten):
    """CR mit den ersten 14 Datenbytes und ein DT (letztes Paket) mit dem
    Rest; reicht fuer Anfragen bis 36 Byte."""
    assert 14 < len(daten) <= 36
    cr = (CALL_CR[:6] + struct.pack(">HI", ref, len(daten)) + CALL_CR[12:14]
          + daten[:14])
    rest = daten[14:]
    return cr, bytes((5 + len(rest),)) + CALL_DT[1:6] + rest


def be16(b, o):
    return (b[o] << 8) | b[o + 1]


def frame(tail):
    f = bytes(4) + tail
    return f + bytes(32 - len(f))


def swap_net(net):
    return bytes((net[2], net[3], net[0], net[1]))


def decode(f):
    d = {"src": ((f[2] & 0x0F) << 8) | f[3], "sz": f[4],
         "net": bytes(f[5:9]), "mtc": f[9]}
    d["caller"] = bool(d["mtc"] & 0x80)
    d["kind"] = KINDS.get((d["mtc"] >> 4) & 7, "?")
    end = 5 + d["sz"]
    if d["kind"] == "CR":
        d["ref"] = be16(f, 10)
        d["len"] = struct.unpack(">I", bytes(f[12:16]))[0]
        d["data"] = bytes(f[18:end])
    elif d["kind"] == "CC":
        d["ref"] = be16(f, 10)
    elif d["kind"] == "DT":
        d["seq"] = d["mtc"] & 7
        d["data"] = bytes(f[10:end])
    return d


# ---------------------------------------------------------- DDS-Meldungen
# Wie ddsabfrage.py: Kopf 13 Byte, dann die Saetze (Index und 32 Byte ab
# 00 80 00, auf Folgeseiten noch 2 Byte davor), danach je Satz ein Klartext
# (CP850, nullterminiert).

DDS_MARK = b"\x00\x80\x00"
DDS_KOPF = 13
# Auf Sekunden genau: der Satz enthaelt nur UNIX-Sekunden, Millisekunden
# gaebe es ohnehin nur als ".000"
NO_TIME = "--.--.---- --:--:--"
PRIO = {0x10: "A", 0x20: "B", 0x30: "C"}


def dds_time(sec):
    if sec == 0xFFFFFFFF:
        return NO_TIME
    return time.strftime("%d.%m.%Y %H:%M:%S", time.gmtime(sec))


def dds_saetze(msg):
    """Saetze einer ANS_FC (Seite 1) oder ANS_NEXT_PAGE_FC (Folgeseiten):
    ([(Index, 32 Byte ab 00 80 00), ...], [Texte als Bytes], FC_in_payload)
    oder None, wenn msg keine solche Antwort ist.

    Kopf 13 Byte: 10 Byte Header, Cache_index (Int16ub), FC_in_payload
    (Int8ub). Dann FC_in_payload Saetze, dann ebenso viele Texte (CP850,
    nullterminiert). Ein Satz ist bei ANS_FC Index (1 Byte) + 32 Byte ab
    00 80 00 (FC_TYPE_1, 33 Byte), bei ANS_NEXT_PAGE_FC steht davor noch
    Req_nb_fc als Int16ul (FC_TYPE_2, 35 Byte; ANS_NEXT_PAGE_FC.txt). Mit
    FC_in_payload = 0 endet die Antwort nach dem Kopf."""
    if len(msg) < DDS_KOPF:
        return None
    n = msg[DDS_KOPF - 1]
    if n == 0:
        return [], [], 0
    for vor in (0, 2):                  # FC_TYPE_1, FC_TYPE_2
        a = DDS_KOPF + vor + 1
        if msg[a:a + 3] == DDS_MARK:
            break
    else:
        return None
    groesse = vor + 33
    saetze = []
    for k in range(n):
        a = DDS_KOPF + groesse * k + vor
        if a + 33 > len(msg) or msg[a + 1:a + 4] != DDS_MARK:
            break
        saetze.append((msg[a], bytes(msg[a + 1:a + 33])))
    texte = msg[DDS_KOPF + groesse * len(saetze):].split(b"\x00")
    return saetze, texte, n


def ereignis_bereich(meld):
    if not meld:
        return "-"
    return "%d..%d" % (meld[0]["ereignisnr"], meld[-1]["ereignisnr"])


def dds_meldungen(msg):
    """Liste der Meldungen als dicts, oder None ohne DDS-Antwort."""
    p = dds_saetze(msg)
    if p is None:
        return None
    saetze, texte, _ = p
    out = []
    for k, (index, r) in enumerate(saetze):
        u16 = lambda o: struct.unpack_from("<H", r, o)[0]
        u32 = lambda o: struct.unpack_from("<I", r, o)[0]
        geht = u32(13)
        out.append({
            "nr": index + 1,
            "kommt": dds_time(u32(9)),
            "geht": dds_time(geht),
            "status": "aktiv" if geht == 0xFFFFFFFF else "gegangen",
            "stoer": u16(27) & 0x7FFF,
            "knoten": "%02X" % r[29],
            "knoten2": "%02X" % r[30],
            "prio": PRIO.get(r[31], "%02X" % r[31]),
            "zaehler": u16(25),
            "uic": "%04X" % u16(19),
            "ref7": u16(21),
            "ref8": u16(23),
            "ereignisnr": struct.unpack_from(">H", r, 3)[0],
            "meldnr": struct.unpack_from(">H", r, 7)[0],
            "text": (texte[k].decode("cp850", errors="replace")
                     if k < len(texte) else ""),
            "roh": r.hex(" "),
        })
    return out

# ------------------------------------------------------------------ Dienst


def log(msg):
    # stdout landet im Journal: journalctl -u idu2-mvb
    print(msg, flush=True)


def parse_args(argv):
    o = {"single": True, "dsw_md": True, "ef1": True, "takt": 1.0,
         "alle": True, "socket": SOCK, "anzahl": 16, "db": True,
         "db_socket": DB_SOCK, "rolle": "IDU2", "pd": True,
         "signale": SIGNALE, "werte": [], "werte_datei": WERTE,
         "softkey_dauer": 1.0}
    i = 1
    while i < len(argv):
        a = argv[i]
        if a == "--zwei-leitungen":
            o["single"] = False
        elif a == "--ohne-dsw-md":
            o["dsw_md"] = False
        elif a == "--ef0":
            o["ef1"] = False
        elif a == "--alle-quellen":     # inzwischen Vorgabe
            o["alle"] = True
        elif a == "--nur-lebenszeichen":
            o["alle"] = False
        elif a == "--wert":
            i += 1
            name, gleich, wert = argv[i].partition("=")
            if not gleich:
                log("--wert erwartet NAME=WERT")
                sys.exit(2)
            o["werte"].append((name.strip(), wert))
        elif a == "--werte-datei":
            i += 1
            o["werte_datei"] = argv[i]
        elif a == "--softkey-dauer":
            i += 1
            o["softkey_dauer"] = float(argv[i])
            if not 0.1 <= o["softkey_dauer"] <= 10:
                log("--softkey-dauer muss zwischen 0.1 und 10 s liegen")
                sys.exit(2)
        elif a == "--takt":
            i += 1
            o["takt"] = float(argv[i])
        elif a == "--socket":
            i += 1
            o["socket"] = argv[i]
        elif a == "--anzahl":
            i += 1
            o["anzahl"] = int(argv[i], 0)
            if not 1 <= o["anzahl"] <= 255:
                log("--anzahl muss zwischen 1 und 255 liegen")
                sys.exit(2)
        elif a == "--ohne-db":
            o["db"] = False
        elif a == "--db-socket":
            i += 1
            o["db_socket"] = argv[i]
        elif a == "--rolle":
            i += 1
            o["rolle"] = argv[i].upper()
            if o["rolle"] not in ROLLEN:
                log("--rolle muss IDU1 oder IDU2 sein")
                sys.exit(2)
        elif a == "--ohne-pd":
            o["pd"] = False
        elif a == "--signale":
            i += 1
            o["signale"] = argv[i]
        else:
            log("Unbekannte Option: %s" % a)
            sys.exit(2)
        i += 1
    return o


# ---------------------------------------------------------------- MariaDB

DDS_SPALTEN = ("ereignisnr", "pos", "seite", "uic", "prio", "zaehler",
               "kommt", "geht", "status", "knoten", "knoten2", "meldung",
               "meldnr", "stoer", "ref7", "ref8", "nr", "roh",
               "cache_index", "abgerufen")
# Schluessel ist die EreignisNr (wie in Dia1.auswerten). pos und seite
# fehlen, wenn DIA1 von sich aus schickt - dann bleiben die alten stehen.
SQL_MELDUNG = (
    "INSERT INTO dds_meldungen (%s) VALUES (%s) ON DUPLICATE KEY UPDATE %s"
    % (", ".join(DDS_SPALTEN), ", ".join(["%s"] * len(DDS_SPALTEN)),
       ", ".join("%s = COALESCE(VALUES(%s), %s)" % (s, s, s)
                 if s in ("pos", "seite") else "%s = VALUES(%s)" % (s, s)
                 for s in DDS_SPALTEN[1:])))


def db_verbinden(pymysql, sock, autocommit=False):
    # unix_socket: Datenbank-Benutzer = Linux-Benutzer (root)
    return pymysql.connect(
        unix_socket=sock, database="myboard", charset="utf8mb4",
        user=pwd.getpwuid(os.geteuid()).pw_name, autocommit=autocommit,
        connect_timeout=5, read_timeout=10, write_timeout=10)


def db_zeit(sec):
    """Zeit eines DDS-Satzes fuer DATETIME, wie dds_time; None = keine."""
    if sec == 0xFFFFFFFF:
        return None
    return time.strftime("%Y-%m-%d %H:%M:%S", time.gmtime(sec))


def dds_zeile(m, seite, cache, jetzt):
    """Eine Meldung aus dds_meldungen() als Werte fuer SQL_MELDUNG."""
    kommt, geht = struct.unpack_from("<II", bytes.fromhex(m["roh"]), 9)
    return (m["ereignisnr"], m.get("pos"), seite, m["uic"], m["prio"],
            m["zaehler"], db_zeit(kommt), db_zeit(geht), m["status"],
            m["knoten"], m["knoten2"], m["text"], m["meldnr"], m["stoer"],
            m["ref7"], m["ref8"], m["nr"], m["roh"], cache, jetzt)


class DdsDb:
    """Schreibt die DDS-Antworten von DIA1 nach myboard.dds_meldungen.

    Eigener Thread mit Warteschlange: die Hauptschleife bedient das MVB
    (Lebenszeichen, Ring des Treibers) und wartet nie auf die Datenbank.
    Ist MariaDB nicht da, bleiben die Seiten in der Warteschlange und
    werden geschrieben, sobald sie erreichbar ist."""

    WARTEN = 5              # s bis zum naechsten Versuch
    VERSUCHE = 12           # bei SQL-Fehlern (nicht Verbindung): 1 min,
                            # reicht fuer myboard-db-setup beim Erststart

    def __init__(self, sock):
        self.sock = sock
        self.q = queue.Queue(maxsize=200)
        self.verworfen = 0
        self.db = None
        try:
            import pymysql
        except ImportError:
            self.pymysql = None
            log("PyMySQL fehlt - DDS-Meldungen werden nicht in MariaDB "
                "gespeichert")
            return
        self.pymysql = pymysql
        threading.Thread(target=self.laufen, name="dds-db",
                         daemon=True).start()

    def speichern(self, erg, seite):
        """Antwort von DIA1 zum Schreiben vormerken (Hauptschleife)."""
        if self.pymysql is None or "cache_index" not in erg:
            return
        try:
            cache = erg["cache_index"]
            jetzt = time.strftime("%Y-%m-%d %H:%M:%S")
            zeilen = [dds_zeile(m, seite, cache, jetzt)
                      for m in erg["meldungen"]]
            self.q.put_nowait((cache, seite, bool(erg.get("letzte")), jetzt,
                               zeilen))
        except queue.Full:
            self.verworfen += 1
            if self.verworfen % 100 == 1:
                log("MariaDB: Warteschlange voll, %d DDS-Seite(n) verworfen"
                    % self.verworfen)
        except Exception as e:      # nie die MVB-Schleife anhalten
            log("MariaDB: DDS-Seite nicht vorgemerkt: %r" % e)

    def laufen(self):
        gestoert = False
        while True:
            job = self.q.get()
            versuche = 0
            while True:
                try:
                    self.schreiben(*job)
                    if gestoert:
                        log("MariaDB wieder erreichbar")
                        gestoert = False
                    break
                except Exception as e:
                    self.trennen()
                    versuche += 1
                    verbindung = isinstance(e, (
                        OSError, self.pymysql.err.OperationalError,
                        self.pymysql.err.InterfaceError))
                    if not gestoert:
                        log("MariaDB: %s - neuer Versuch alle %d s"
                            % (e, self.WARTEN))
                        gestoert = True
                    if not verbindung and versuche >= self.VERSUCHE:
                        log("MariaDB: DDS-Seite nach %d Versuchen verworfen: "
                            "%s" % (versuche, e))
                        break
                    time.sleep(self.WARTEN)

    def trennen(self):
        if self.db is not None:
            try:
                self.db.close()
            except Exception:
                pass
            self.db = None

    def schreiben(self, cache, seite, letzte, jetzt, zeilen):
        if self.db is None:
            self.db = db_verbinden(self.pymysql, self.sock)
        with self.db.cursor() as c:
            c.execute("SELECT cache_index FROM dds_stand WHERE id = 1 "
                      "FOR UPDATE")
            r = c.fetchone()
            # Anderer Cache_index: DIA1 hat neuere Meldungen oder Seite 1
            # wurde neu abgefragt - der alte Inhalt gilt nicht mehr
            neu = r is None or r[0] != cache
            if neu:
                c.execute("DELETE FROM dds_meldungen")
                c.execute("REPLACE INTO dds_stand (id, cache_index, "
                          "geaendert, aktualisiert, vollstaendig) "
                          "VALUES (1, %s, %s, %s, %s)",
                          (cache, jetzt, jetzt, letzte))
            else:
                c.execute("UPDATE dds_stand SET aktualisiert = %s, "
                          "vollstaendig = vollstaendig OR %s WHERE id = 1",
                          (jetzt, letzte))
            if zeilen:
                c.executemany(SQL_MELDUNG, zeilen)
        self.db.commit()
        log("MariaDB: %s, %d Meldungen, Cache_index %d%s"
            % ("Seite %d" % seite if seite else "von DIA1 aus", len(zeilen),
               cache, " - neu, alter Inhalt ersetzt" if neu else ""))


# ------------------------------------------------------------ Prozessdaten

def signale_laden(pfad, rolle):
    """Signale der Rolle aus der Signalliste (mvbtrak2tsv.py), je ein dict
    mit richtung 'lesen' (Sink: das IDU steht als Empfaenger da) oder
    'schreiben' (Source: Telegramm des IDU)."""
    out = []
    with open(pfad, encoding="utf-8") as f:
        for zeile in f:
            w = zeile.rstrip("\n").split("\t")
            if zeile.startswith("#") or len(w) != 15:
                continue
            (port, tgm, quelle, zyklus, port_bytes, name, typ, datentyp,
             byte, bit, nbytes, faktor, einheit, senken, text) = w
            if quelle == rolle:
                richtung = "schreiben"
            elif rolle in senken.split(","):
                richtung = "lesen"
            else:
                continue
            out.append({"port": int(port), "telegramm": tgm, "quelle": quelle,
                        "zyklus": int(zyklus), "port_bytes": int(port_bytes),
                        "name": name, "typ": typ, "datentyp": datentyp,
                        "byte": int(byte), "bit": int(bit),
                        "bytes": int(nbytes), "faktor": float(faktor),
                        "einheit": einheit, "senken": senken, "text": text,
                        "richtung": richtung})
    return out


GANZZAHL = {"uint8": "B", "uint16": "H", "uint32": "I", "int16": "h"}


def pd_wert(s, daten):
    """Wert eines Signals aus den Portdaten (big-endian wie am MVB) als
    (wert, wert_zeit, wert_text, roh) - die Spalten in mvb_prozessdaten -
    oder None, wenn der Port dafuer zu kurz ist.

    bool: Bit bit % 8 in Byte byte + bit // 8, roh ist dieses Byte.
    uint8/16/32, int16: Ganzzahl. float: int16 * faktor (1000 Hex = 100 %).
    datetime: Sekunden seit 1970 + Ticks/65536 - als double exakt in wert,
    lesbar (UTC, auf Mikrosekunden) in wert_zeit. string: Zeichen bis zum
    ersten Nullbyte in wert_text."""
    b, dt = s["byte"], s["datentyp"]
    if dt == "bool":
        i = b + s["bit"] // 8
        if i >= len(daten):
            return None
        return (daten[i] >> (s["bit"] % 8)) & 1, None, None, "%02x" % daten[i]
    roh = daten[b:b + s["bytes"]]
    if len(roh) < s["bytes"]:
        return None
    if dt in GANZZAHL:
        return struct.unpack(">" + GANZZAHL[dt], roh)[0], None, None, roh.hex(" ")
    if dt == "float":
        return (struct.unpack(">h", roh)[0] * s["faktor"], None, None,
                roh.hex(" "))
    if dt == "datetime":
        sek, ticks = struct.unpack(">IH", roh)
        zeit = datetime.datetime(1970, 1, 1) + datetime.timedelta(
            seconds=sek, microseconds=round(ticks * 1000000 / 65536))
        return (sek + ticks / 65536, zeit.strftime("%Y-%m-%d %H:%M:%S.%f"),
                None, roh.hex(" "))
    if dt == "string":
        text = roh.split(b"\0", 1)[0].decode("latin-1").rstrip()
        return None, None, text, roh.hex(" ")
    return None, None, roh.hex(" "), roh.hex(" ")


def zeit_ms(t):
    return time.strftime("%Y-%m-%d %H:%M:%S", time.localtime(t)) + \
        ".%03d" % int(t % 1 * 1000)


PD_SPALTEN = ("port", "name", "richtung", "telegramm", "quelle", "zyklus_ms",
              "port_bytes", "typ", "datentyp", "byte_ofs", "bit_ofs",
              "faktor", "einheit", "beschreibung", "senken", "wert",
              "wert_zeit", "wert_text", "roh", "frisch", "gelesen",
              "geaendert")
SQL_PD_NEU = "INSERT INTO mvb_prozessdaten (%s) VALUES (%s)" % (
    ", ".join(PD_SPALTEN), ", ".join(["%s"] * len(PD_SPALTEN)))
SQL_PD_LESUNG = (
    "INSERT INTO mvb_prozessdaten (port, name, wert, wert_zeit, wert_text, "
    "roh, frisch, gelesen, geaendert) VALUES (%s, %s, %s, %s, %s, %s, %s, "
    "%s, %s) ON DUPLICATE KEY UPDATE wert = VALUES(wert), "
    "wert_zeit = VALUES(wert_zeit), wert_text = VALUES(wert_text), "
    "roh = VALUES(roh), frisch = VALUES(frisch), gelesen = VALUES(gelesen), "
    "geaendert = VALUES(geaendert)")


class PdDb:
    """Schreibt die Prozessdaten nach myboard.mvb_prozessdaten: jede Lesung
    eines Sink-Ports (und jedes Senden eines eigenen Ports) als eine
    Anweisung mit allen Signalen des Ports - Wert, frisch, gelesen;
    geaendert nur, wenn sich der Wert aendert.

    Die Tabelle ist eine MEMORY-Tabelle (db-setup.sql): rund 300 Lesungen
    pro Sekunde gingen sonst als Redo-Log von InnoDB auf die CFast. Beim
    Start und nach jedem Datenbankfehler wird sie neu angelegt - alle
    Signale der Rolle mit Beschreibung und letztem Stand.

    Eigener Thread wie DdsDb: die Hauptschleife reicht die Portdaten nur
    weiter (melden). Ist MariaDB nicht erreichbar, fuehrt der Thread nur den
    Stand im Speicher nach."""

    WARTEN = 5

    def __init__(self, sock, signale):
        self.sock = sock
        self.signale = signale
        self.je_port = {}
        for s in signale:
            self.je_port.setdefault(s["port"], []).append(s)
        # Name -> Port der gelesenen Signale (Rueckmeldungen der Softkeys)
        self.port_von = {s["name"]: s["port"] for s in signale
                         if s["richtung"] == "lesen"}
        self.eingang = queue.Queue(maxsize=5000)
        self.werte = {}         # (port, name) -> (wert, zeit, text, roh, geaendert)
        self.frisch = {}        # port -> True/False
        self.gelesen = {}       # port -> Zeit der letzten Lesung
        self.geschrieben = 0
        self.verworfen = 0
        self.db = None
        try:
            import pymysql
        except ImportError:
            self.pymysql = None
            log("PyMySQL fehlt - Prozessdaten werden nicht gespeichert")
            return
        self.pymysql = pymysql
        threading.Thread(target=self.laufen, name="pd-db", daemon=True).start()

    def melden(self, port, daten, frisch, t):
        """Eine Lesung aus der Hauptschleife (daten None, wenn nicht
        frisch)."""
        try:
            self.eingang.put_nowait((port, daten, frisch, t))
        except queue.Full:
            self.verworfen += 1
            if self.verworfen % 1000 == 1:
                log("MariaDB (Prozessdaten) kommt nicht nach - %d Lesungen "
                    "nicht geschrieben" % self.verworfen)

    def laufen(self):
        gestoert = False
        naechster_versuch = 0.0
        while True:
            eintrag = self.eingang.get()
            try:
                zeilen = self.verarbeiten(*eintrag)
            except Exception as e:      # der Thread darf nicht sterben
                log("Prozessdaten: Auswertung fehlgeschlagen: %r" % e)
                continue
            if self.db is None and time.time() < naechster_versuch:
                continue                # nur der Stand im Speicher
            try:
                if self.db is None:
                    # MEMORY kennt keine Transaktionen: autocommit
                    self.db = db_verbinden(self.pymysql, self.sock,
                                           autocommit=True)
                    self.anlegen()      # enthaelt schon diese Lesung
                elif zeilen:
                    with self.db.cursor() as c:
                        c.executemany(SQL_PD_LESUNG, zeilen)
                self.geschrieben += 1
                if gestoert:
                    log("MariaDB (Prozessdaten) wieder erreichbar")
                    gestoert = False
            except Exception as e:
                self.trennen()
                naechster_versuch = time.time() + self.WARTEN
                if not gestoert:
                    log("MariaDB (Prozessdaten): %s - neuer Versuch alle %d s"
                        % (e, self.WARTEN))
                    gestoert = True

    def verarbeiten(self, port, daten, frisch, t):
        """Stand im Speicher nachfuehren; Zeilen fuer SQL_PD_LESUNG."""
        gelesen = zeit_ms(t)
        self.frisch[port] = frisch
        self.gelesen[port] = gelesen
        zeilen = []
        for s in self.je_port.get(port, ()):
            k = (port, s["name"])
            w = pd_wert(s, daten) if daten is not None else None
            if w is not None:
                alt = self.werte.get(k)
                # geaendert: Zeit der Lesung, die den neuen Wert brachte
                self.werte[k] = w + (gelesen if alt is None or alt[:3] != w[:3]
                                     else alt[4],)
            wert = self.werte.get(k, (None,) * 5)
            zeilen.append(k + wert[:4] + (frisch, gelesen, wert[4]))
        return zeilen

    def trennen(self):
        if self.db is not None:
            try:
                self.db.close()
            except Exception:
                pass
            self.db = None

    def zeile(self, s):
        wert = self.werte.get((s["port"], s["name"]), (None,) * 5)
        return ((s["port"], s["name"], s["richtung"], s["telegramm"][:30],
                 s["quelle"][:20], s["zyklus"], s["port_bytes"], s["typ"][:16],
                 s["datentyp"], s["byte"], s["bit"], s["faktor"],
                 s["einheit"], s["text"][:500], s["senken"][:100])
                + wert[:4] + (self.frisch.get(s["port"]),
                              self.gelesen.get(s["port"]), wert[4]))

    def lesewert(self, name):
        """(Wert, frisch) eines gelesenen Signals oder None; aus dem
        Stand im Speicher, also auch ohne MariaDB."""
        port = self.port_von.get(name)
        if port is None:
            return None
        return (self.werte.get((port, name), (None,) * 5)[0],
                self.frisch.get(port))

    def anlegen(self):
        with self.db.cursor() as c:
            c.execute("DELETE FROM mvb_prozessdaten")
            c.executemany(SQL_PD_NEU, [self.zeile(s) for s in self.signale])
        log("MariaDB: mvb_prozessdaten angelegt, %d Signale - ab jetzt jede "
            "Lesung" % len(self.signale))


class PdAbfrage:
    """Liest die Sink-Ports im Takt ihrer Zykluszeit (Polling: fuer
    Prozessdaten gibt es keinen Interrupt) und gibt jede Lesung an PdDb
    weiter. Laeuft in der Hauptschleife, wie das Lebenszeichen."""

    def __init__(self, mvb, ports, db):
        self.mvb = mvb
        self.ports = dict(ports)        # Port -> (Laenge, Zyklus in ms)
        self.db = db
        jetzt = time.time()
        self.faellig = {p: jetzt for p in self.ports}
        self.lesungen = 0

    def naechste(self):
        return min(self.faellig.values(), default=None)

    def abfragen(self, jetzt):
        for port, f in list(self.faellig.items()):
            if f > jetzt:
                continue
            laenge, zyklus = self.ports[port]
            f += zyklus / 1000
            self.faellig[port] = f if f > jetzt else jetzt + zyklus / 1000
            try:
                daten, frisch = self.mvb.pd_read(port, laenge), True
            except OSError as e:
                if e.errno == errno.ETIMEDOUT:
                    daten, frisch = None, False
                elif e.errno == errno.ENETDOWN:
                    continue
                else:
                    log("Port %d nicht lesbar (%s) - wird nicht mehr "
                        "abgefragt" % (port, os.strerror(e.errno)))
                    del self.faellig[port]
                    continue
            self.lesungen += 1
            self.db.melden(port, daten, frisch, jetzt)


def pd_kodieren(s, wert, daten):
    """Gegenstueck zu pd_wert: wert in die Portdaten (bytearray) schreiben.
    Wirft ValueError, TypeError oder struct.error, wenn er nicht passt."""
    b, dt = s["byte"], s["datentyp"]
    if dt == "bool":
        i, m = b + s["bit"] // 8, 1 << (s["bit"] % 8)
        if wert not in (0, 1):          # auch True/False
            raise ValueError("bool: 0 oder 1")
        daten[i] = (daten[i] | m) if wert else (daten[i] & ~m & 0xFF)
        return
    n = s["bytes"]
    if dt in GANZZAHL:
        roh = struct.pack(">" + GANZZAHL[dt], int(wert))
    elif dt == "float":
        roh = struct.pack(">h", round(float(wert) / s["faktor"]))
    elif dt == "datetime":
        t = float(wert)
        sek = int(t)
        roh = struct.pack(">IH", sek, min(0xFFFF, round((t - sek) * 65536)))
    elif dt == "string":
        roh = str(wert).encode("latin-1")[:n].ljust(n, b"\0")
    else:
        roh = bytes.fromhex(str(wert))[:n].ljust(n, b"\0")
    daten[b:b + n] = roh


def wert_aus_text(s, text):
    """Wert fuer pd_kodieren aus Text (--wert NAME=WERT)."""
    dt = s["datentyp"]
    if dt == "bool":
        t = text.strip().lower()
        if t in ("1", "true", "ein", "ja"):
            return 1
        if t in ("0", "false", "aus", "nein"):
            return 0
        raise ValueError("bool: 0 oder 1")
    if dt in GANZZAHL:
        return int(text, 0)
    if dt in ("float", "datetime"):
        return float(text)
    return text


class Quellen:
    """Inhalt der Source-Ports der Rolle und ihr Senden.

    Jeder Port hat einen Puffer in Portlaenge, jedes Signal darin einen
    Wert: Vorgabe 0 bzw. leer - fuer Befehle und Softkeys "nicht
    gedrueckt". Setzen lassen sie sich mit --wert NAME=WERT und ueber den
    Socket ({"befehl": "setzen", "signal": ..., "wert": ..., "dauer": s}),
    mit "dauer" als Puls: danach gilt wieder der vorige Wert. Das
    Lebenszeichen fuehrt der Dienst selbst.

    Gesendet wird jeder Port im Takt seiner Zykluszeit - der Busmaster holt
    ihn in diesem Takt - und sofort nach jeder Aenderung; jedes Senden geht
    mit allen Signalen an PdDb."""

    def __init__(self, mvb, ports, signale, ls_port, db):
        self.mvb = mvb
        self.db = db
        self.daten = {p: bytearray(n) for p, n, _ in ports}
        self.zyklus = {p: z for p, _, z in ports}
        self.signale = {s["name"]: s for s in signale
                        if s["port"] in self.daten}
        self.ls_port = ls_port
        self.faellig = {p: 0.0 for p in self.daten}      # gleich senden
        self.pulse = {}         # Signal -> (Ende, Wert danach)
        self.fehler = {}        # Port -> Zahl der Fehlversuche

    def lebenszeichen(self, wert):
        struct.pack_into(">H", self.daten[self.ls_port], 0, wert)
        self.faellig[self.ls_port] = 0.0

    def setzen(self, name, wert, dauer=None):
        """Signal setzen; liefert einen Fehlertext oder None."""
        s = self.signale.get(name)
        if s is None:
            return "kein Source-Signal dieser Rolle: %s" % name
        if s["port"] == self.ls_port and s["byte"] < 2:
            return "das Lebenszeichen fuehrt der Dienst selbst"
        puffer = bytearray(self.daten[s["port"]])
        vorher = pd_wert(s, bytes(puffer))
        try:
            pd_kodieren(s, wert, puffer)
        except (ValueError, TypeError, struct.error, UnicodeError) as e:
            return "%s (%s): %s" % (name, s["datentyp"], e)
        self.daten[s["port"]][:] = puffer
        self.faellig[s["port"]] = 0.0
        if dauer:
            alt = self.pulse.get(name)
            danach = alt[1] if alt else (vorher[2] if s["datentyp"] == "string"
                                         else vorher[0])
            self.pulse[name] = (time.time() + float(dauer), danach)
        else:
            self.pulse.pop(name, None)
        return None

    def werte(self):
        out = []
        for s in self.signale.values():
            w = pd_wert(s, bytes(self.daten[s["port"]]))
            out.append({"signal": s["name"], "port": s["port"],
                        "datentyp": s["datentyp"],
                        "wert": w[2] if s["datentyp"] == "string" else w[0]})
        return out

    def naechste(self):
        return min(list(self.faellig.values())
                   + [e for e, _ in self.pulse.values()])

    def senden(self, jetzt):
        for name, (ende, danach) in list(self.pulse.items()):
            if ende <= jetzt:
                del self.pulse[name]
                self.setzen(name, danach)
        for port, f in self.faellig.items():
            if f > jetzt:
                continue
            daten = bytes(self.daten[port])
            self.faellig[port] = jetzt + self.zyklus[port] / 1000
            try:
                self.mvb.pd_write(port, daten)
            except OSError as e:
                n = self.fehler.get(port, 0) + 1
                self.fehler[port] = n
                if n == 1:
                    log("Port %d nicht geschrieben: %s"
                        % (port, os.strerror(e.errno)))
                continue
            if self.fehler.pop(port, 0):
                log("Port %d wieder geschrieben" % port)
            if self.db:
                self.db.melden(port, daten, True, jetzt)


def softkey_liste(quellen, pd_db, rolle):
    """Softkeys der Rolle fuer die Oberflaeche: (Gruppen, Rueckmeldungen).
    Gruppe {"name", "tasten": [{"signal", "text", "wert"}]} - wert 1,
    solange der Puls laeuft; Rueckmeldung {"signal", "text", "wert",
    "frisch"}. Nur Signale, die es in der Rolle gibt."""
    gruppen = []
    for name, tasten in SOFTKEYS:
        liste = []
        for kurz, text in tasten:
            s = quellen.signale.get("%s-%s" % (rolle, kurz))
            if s is None or s["datentyp"] != "bool":
                continue
            wert = pd_wert(s, bytes(quellen.daten[s["port"]]))
            liste.append({"signal": s["name"], "text": text,
                          "wert": wert[0] if wert else None})
        if liste:
            gruppen.append({"name": name, "tasten": liste})
    rueck = []
    for name, text in RUECKMELDUNGEN:
        w = pd_db.lesewert(name) if pd_db else None
        if w is not None:
            rueck.append({"signal": name, "text": text, "wert": w[0],
                          "frisch": w[1]})
    return gruppen, rueck


class Mvb:
    """/dev/mvblli0: Einrichtung, Prozess- und Message-Daten."""

    def __init__(self, fd):
        self.fd = fd

    def io(self, name, arg=0):
        req = IOC[name]
        req = req - (1 << 32) if req >= (1 << 31) else req
        if isinstance(arg, bytearray):
            return fcntl.ioctl(self.fd, req, arg, True)
        return fcntl.ioctl(self.fd, req, arg)

    def pd_write(self, port, data):
        # Der Treiber liest die vollen 38 Byte, als Laenge sieht er die
        # Portgroesse
        buf = bytearray(38)
        struct.pack_into("<HH", buf, 0, PD, port)
        buf[4:4 + len(data)] = data
        os.writev(self.fd, [memoryview(buf)[:len(data)]])

    def pd_read(self, port, size):
        """Daten eines Sink-Ports; OSError ETIMEDOUT, wenn sie aelter als
        die Sink-Time-Schwelle sind. Wie bei pd_write ist die Laenge die
        Portgroesse, der Treiber nimmt die ganze Struktur."""
        buf = bytearray(38)
        struct.pack_into("<HH", buf, 0, PD, port)
        os.readv(self.fd, [memoryview(buf)[:size]])
        return bytes(buf[4:4 + size])

    def md_write(self, dest, tail):
        buf = bytearray(38)
        struct.pack_into("<HH", buf, 0, MD_LOW, dest)
        buf[4:36] = frame(tail)
        os.writev(self.fd, [memoryview(buf)[:32]])

    def md_read(self):
        """Naechstes empfangenes Frame oder None."""
        buf = bytearray(38)
        struct.pack_into("<HH", buf, 0, MD_LOW, DIA1_ADDR)
        try:
            os.readv(self.fd, [memoryview(buf)[:32]])
        except OSError as e:
            if e.errno in (errno.ENOBUFS, errno.EAGAIN, errno.ENETDOWN):
                return None
            raise
        return bytes(buf[4:36])


class Dia1:
    """Message-Transport mit DIA1: beantwortet dessen Frames, fuehrt die
    DDS-Abfrage und liefert das Ergebnis an den wartenden Client."""

    def __init__(self, mvb, db=None):
        self.mvb = mvb
        self.db = db            # DdsDb oder None (--ohne-db)
        self.next_ref = 0x0043
        self.conns = {}
        self.pending = None     # laufende Abfrage
        self.letzte = None      # letztes Ergebnis (dict fuer JSON)
        self.rx = 0
        self.fremd = 0
        self.seiten = {}        # Seite -> Ergebnis, seit dem letzten REQ_FC

    def alloc_ref(self):
        ref = self.next_ref
        self.next_ref = (self.next_ref + 1) & 0xFFFF or 1
        return ref

    def tx(self, tail):
        self.mvb.md_write(DIA1_ADDR, tail)

    def seite_abrufen(self, reply_to, seite, anzahl, neu):
        """Seite 1 holt DIA1 mit REQ_FC neu, jede weitere mit
        REQ_NEXT_PAGE_FC. Einen Befehl fuer die vorige Seite gibt es
        nicht; schon geholte Seiten kommen deshalb aus dem Zwischenspeicher,
        und DIA1 bleibt mit seiner Seite im Gleichschritt. neu: Seite 1
        auf jeden Fall neu holen. Liefert (Fehlertext, None), (None,
        Ergebnis) ohne Anfrage an DIA1 oder (None, None), wenn die Anfrage
        laeuft."""
        if seite in self.seiten and not (neu and seite == 1):
            return None, dict(self.seiten[seite], zwischengespeichert=True)
        if seite == 1:
            self.seiten = {}
            self.start(reply_to, seite, anzahl)
            return None, None
        hoechste = max(self.seiten, default=0)
        if not hoechste or seite != hoechste + 1:
            return "Seite %d: zuerst Seite 1 abrufen" % seite, None
        if self.seiten[hoechste]["letzte"]:
            return None, {"ok": True, "seite": seite, "anzahl": 0,
                          "meldungen": [], "letzte": True}
        # weiter hinter der letzten gezeigten Meldung (der letzten der Seite
        # davor); Cache_index unveraendert so, wie DIA1 ihn in der Antwort
        # darauf genannt hat - er ist die Version des DDS-Caches, kein
        # Seitenzaehler
        vorige = self.seiten[hoechste]
        self.start(reply_to, seite, anzahl, vorige["meldungen"][-1],
                   vorige.get("cache_index", 1))
        return None, None

    def start(self, reply_to, seite, anzahl, nach=None, cache=1):
        """REQ_FC (Seite 1) oder, mit nach = letzte gezeigte Meldung,
        REQ_NEXT_PAGE_FC."""
        ref = self.alloc_ref()
        if nach is None:
            # Genau der Mitschnitt; Datenbyte 13 = Anzahl
            cr = (CALL_CR[:6] + struct.pack(">H", ref) + CALL_CR[8:27]
                  + bytes((anzahl,)))
            dt = CALL_DT
            was = "REQ_FC"
        else:
            satz = bytes.fromhex(nach["roh"])[:9]   # 00 80 00 Address Unk
            daten = next_page_data(cache, anzahl, satz)
            cr, dt = request_frames(ref, daten)
            was = ("REQ_NEXT_PAGE_FC nach Address %d (EreignisNr), "
                   "Cache_index %d, Daten %s"
                   % (nach["ereignisnr"], cache, daten.hex(" ")))
        now = time.time()
        self.pending = {"ref": ref, "t0": now, "t_last": now, "cc": False,
                        "antwort": None, "reply_to": reply_to,
                        "seite": seite, "anzahl": anzahl, "dt": dt}
        self.tx(cr)
        log("DDS-Abfrage an DIA1 gesendet, Ref %04X, Seite %d, bis %d "
            "Meldungen: %s" % (ref, seite, anzahl, was))

    def check_timeout(self):
        # Abgebrochen wird erst, wenn DIA1 eine Weile schweigt - viele
        # Meldungen brauchen viele DT-Frames und damit Zeit
        p = self.pending
        if p and time.time() - p["t_last"] > ABFRAGE_TIMEOUT:
            self.pending = None
            grund = ("DIA1 hat die Anfrage nicht angenommen (kein CC)"
                     if not p["cc"] else "keine DDS-Daten von DIA1")
            log("DDS-Abfrage Ref %04X: %s" % (p["ref"], grund))
            return p["reply_to"], {"ok": False, "fehler": grund}
        return None, None

    def handle(self, f):
        """Ein empfangenes Frame. Liefert (Client, Antwort) oder
        (None, None)."""
        self.rx += 1
        d = decode(f)
        if d["src"] != DIA1_ADDR:
            self.fremd += 1
            return None, None
        if self.pending:
            self.pending["t_last"] = time.time()
        back = swap_net(d["net"])
        side = 0x00 if d["caller"] else 0x80     # wir: die Gegenseite
        key = d["net"]
        if d["kind"] == "CR":
            self.tx(bytes((0x08,)) + back + bytes((side | 0x41,))
                    + struct.pack(">H", d["ref"]) + bytes((CC_CREDIT,)))
            self.conns[key] = {"ref": d["ref"], "len": d["len"],
                               "data": bytearray(d["data"]),
                               "call": d["caller"], "net": d["net"]}
            return self.check_done(key)
        if d["kind"] == "DT":
            self.tx(bytes((0x05,)) + back
                    + bytes((side | 0x60 | ((d["seq"] + 1) & 7),)))
            c = self.conns.get(key)
            if c is None:
                return None, None
            c["data"] += d["data"]
            return self.check_done(key)
        if d["kind"] == "CC":
            p = self.pending
            if p and d["ref"] == p["ref"] and not p["cc"]:
                p["cc"] = True
                self.tx(p["dt"])
        return None, None

    def check_done(self, key):
        c = self.conns[key]
        if len(c["data"]) < c["len"]:
            return None, None
        msg = bytes(c["data"][:c["len"]])
        del self.conns[key]
        p = self.pending
        if not c["call"]:
            # Reply von DIA1 auf unsere Anfrage
            if p:
                p["antwort"] = msg.hex()
            return None, None
        # Daten von DIA1 (Teil 2): Reply des Displays, dann auswerten
        # Quittung: derselbe Befehl ohne Payload, also die 10 Byte Header
        # der Antwort (bei ANS_FC gleich REPLY_DATA aus dem Mitschnitt)
        ref = self.alloc_ref()
        quittung = msg[:10] if len(msg) >= 10 else REPLY_DATA
        self.tx(bytes((0x17,)) + swap_net(c["net"]) + bytes((0x00,))
                + struct.pack(">HI", ref, len(quittung))
                + bytes((0x70, 0x00)) + quittung + REPLY_PAD)
        meld = dds_meldungen(msg) or []
        erg = {"ok": True, "anzahl": len(meld), "bytes": len(msg),
               "zeit": time.strftime("%d.%m.%Y %H:%M:%S"),
               "kopf": msg[:DDS_KOPF].hex(" "), "meldungen": meld}
        # Kopf der Antwort: 8 Command (ANS_FC = FB), 10-11 Cache_index
        # (Version des DDS-Caches bei DIA1), 12 Anzahl der Saetze
        if len(msg) >= 13:
            erg["cache_index"] = be16(msg, 10)
        log("DDS-Daten von DIA1: %d Byte, %d Meldungen, EreignisNr %s, "
            "Kopf %s" % (len(msg), len(meld), ereignis_bereich(meld),
                         erg["kopf"]))
        if not p:
            self.letzte = erg           # DIA1 hat von sich aus geschickt
            if self.db:
                self.db.speichern(erg, None)
            return None, erg
        erg["angefragt"] = p["anzahl"]
        if p["antwort"]:
            erg["antwort"] = p["antwort"]
        self.auswerten(p, meld, erg)
        if self.db:
            self.db.speichern(erg, erg["seite"])
        erg["dauer_s"] = round(time.time() - p["t0"], 2)
        self.pending = None
        self.letzte = erg
        return p["reply_to"], erg

    def auswerten(self, p, meld, erg):
        """Seite einordnen und zwischenspeichern."""
        seite = p["seite"]
        gezeigt ={m["ereignisnr"] for e in self.seiten.values()
                   for m in e["meldungen"]}
        neu = [m for m in meld if m["ereignisnr"] not in gezeigt]
        if meld and not neu:
            # Falls DIA1 nach der letzten Seite von vorn anfaengt
            erg["hinweis"] = ("DIA1 lieferte nur schon gezeigte Meldungen - "
                              "als Ende der Liste genommen")
            meld = []
        elif len(neu) < len(meld):
            erg.setdefault("hinweis", "%d Meldung(en) standen schon auf "
                           "frueheren Seiten - neue Ereignisse bei DIA1?"
                           % (len(meld) - len(neu)))
        if "hinweis" in erg:
            log("Seite %d: %s" % (seite, erg["hinweis"]))
        anfang = (seite - 1) * p["anzahl"] + 1
        for k, m in enumerate(meld):
            m["pos"] = anfang + k
        erg.update(seite=seite, anzahl=len(meld), meldungen=meld,
                   letzte=len(meld) < p["anzahl"])
        if meld or seite == 1:
            self.seiten[seite] = erg


def main():
    o = parse_args(sys.argv)
    stop = {"jetzt": False}

    def on_signal(signum, frame_):
        stop["jetzt"] = True

    signal.signal(signal.SIGTERM, on_signal)
    signal.signal(signal.SIGINT, on_signal)

    # /dev/mvblli0 kann kurz nach dem Booten noch fehlen
    t0 = time.time()
    while True:
        try:
            fd = os.open(DEV, os.O_RDWR)
            break
        except OSError as e:
            if time.time() - t0 > 60 or e.errno == errno.EBUSY:
                log("%s nicht zu oeffnen: %s%s" % (
                    DEV, os.strerror(e.errno),
                    " - eine andere Anwendung haelt es"
                    if e.errno == errno.EBUSY else ""))
                return 1
            time.sleep(1)

    mvb = Mvb(fd)
    wert = 0
    rolle = o["rolle"]
    adresse, (ls_port, ls_laenge, ls_zyklus), weitere = ROLLEN[rolle]

    # Signale der Rolle: Inhalt der Source-Ports, Sink-Ports, Datenbank
    signale = []
    try:
        signale = signale_laden(o["signale"], rolle)
    except OSError as e:
        log("Signalliste %s: %s - keine Prozessdaten, Source-Ports nur mit "
            "Lebenszeichen und Nullen" % (o["signale"], e.strerror))
    pd_db, senken = None, {}
    if signale and o["db"] and o["pd"]:
        pd_db = PdDb(o["db_socket"], signale)
        if pd_db.pymysql is None:
            pd_db = None
    if pd_db:
        eigene = {ls_port} | {p for p, _, _ in weitere}
        for s in signale:
            if (s["richtung"] == "lesen" and s["port"] not in eigene
                    and s["port_bytes"] in (2, 4, 8, 16, 32)):
                senken[s["port"]] = (s["port_bytes"], s["zyklus"])

    quellen = Quellen(
        mvb, [(ls_port, ls_laenge, ls_zyklus)]
        + (list(weitere) if o["alle"] else []),
        [s for s in signale if s["richtung"] == "schreiben"], ls_port, pd_db)
    quellen.lebenszeichen(wert)
    # feste Werte: erst die Datei, dann --wert
    werte = []
    try:
        with open(o["werte_datei"], encoding="utf-8") as f:
            for zeile in f:
                name, gleich, text = zeile.rstrip("\r\n").partition("=")
                if gleich and not name.lstrip().startswith("#"):
                    werte.append((name.strip(), text))
    except FileNotFoundError:
        pass
    except OSError as e:
        log("%s: %s" % (o["werte_datei"], e.strerror))
    for name, text in werte + o["werte"]:
        s = quellen.signale.get(name)
        try:
            grund = (quellen.setzen(name, wert_aus_text(s, text)) if s
                     else "kein gesendetes Source-Signal der Rolle %s" % rolle)
        except ValueError as e:
            grund = "%s: %s" % (s["datentyp"], e)
        log("--wert %s=%s: %s" % (name, text, grund or "gesetzt"))

    try:
        cfg = struct.pack("<H", len(quellen.daten) + len(senken))
        for p, puffer in quellen.daten.items():
            cfg += struct.pack("<HHH", p, len(puffer), PD_SRC)
        for p, (s, _) in sorted(senken.items()):
            cfg += struct.pack("<HHH", p, s, PD_SINK)
        mvb.io("WRITE_DEV_ADDR", adresse)
        mvb.io("HWINIT", bytearray(struct.pack("<QBBHHB x",
                                               0, 1, 1, 0xFFF, 0xFFF, 0)))
        mvb.io("MD_CONF", bytearray(struct.pack("<H",
                                                0xFFFF if o["ef1"] else 0)))
        mvb.io("PD_CONF", bytearray(cfg))
        # Quellen werden erst mit dem ersten write() aktiv
        for p, puffer in quellen.daten.items():
            mvb.pd_write(p, bytes(puffer))
        cmd = CTRL_CLEAR | (CTRL_LINE_A if o["single"] else 0)
        mvb.io("WRITE_CONTROL", bytearray(struct.pack("<HHBx", adresse, 43,
                                                      cmd)))
        if o["dsw_md"]:
            mvb.io("WRITE_DSW", (DSW_MD << 16) | DSW_MD)
        mvb.io("START")
    except OSError as e:
        log("Einrichtung fehlgeschlagen: %s" % os.strerror(e.errno))
        os.close(fd)
        return 1
    pd = PdAbfrage(mvb, senken, pd_db) if senken else None

    try:
        os.unlink(o["socket"])
    except FileNotFoundError:
        pass
    srv = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    srv.bind(o["socket"])
    os.chmod(o["socket"], 0o666)
    srv.listen(4)
    srv.setblocking(False)
    clients = {}            # socket -> Eingangspuffer

    log("%s (Geraet %d = %02XH) am MVB: Lebenszeichen auf Port %d alle %g s, "
        "%s, %s, Messages ueber %s%s; Steuerung %s"
        % (rolle, adresse, adresse, ls_port, o["takt"],
           "Leitung A allein" if o["single"] else "zwei Leitungen",
           "MD-Bit im DSW" if o["dsw_md"] else "DSW ohne MD-Bit",
           "EF1" if o["ef1"] else "EF0",
           ", dazu " + "/".join(str(p) for p, _, _ in weitere) if o["alle"]
           else "", o["socket"]))
    if pd_db:
        log("Prozessdaten: %d Sink-Ports mit %d Signalen, im Takt ihrer "
            "Zykluszeit gelesen, und %d gesendete Signale - jede Lesung -> "
            "myboard.mvb_prozessdaten"
            % (len(senken), sum(1 for s in signale if s["port"] in senken),
               sum(1 for s in signale if s["richtung"] == "schreiben")))
    elif not (o["db"] and o["pd"]):
        log("Prozessdaten: aus (%s)" % ("--ohne-db" if not o["db"]
                                        else "--ohne-pd"))

    dia = Dia1(mvb, DdsDb(o["db_socket"]) if o["db"] else None)

    def send(cl, obj):
        if cl is None or cl not in clients:
            return
        try:
            cl.sendall(json.dumps(obj, ensure_ascii=False).encode("utf-8")
                       + b"\n")
        except OSError:
            clients.pop(cl, None)
            cl.close()

    def request(cl, line):
        try:
            req = json.loads(line)
            befehl = req.get("befehl")
        except (ValueError, AttributeError):
            send(cl, {"ok": False, "fehler": "keine gueltige JSON-Anfrage"})
            return
        if befehl == "dds":
            if dia.pending:
                send(cl, {"ok": False, "fehler": "Abfrage laeuft bereits"})
                return
            anzahl = req.get("anzahl", o["anzahl"])
            seite = req.get("seite", 1)
            # ohne Seitenangabe wie frueher: Seite 1 neu von DIA1
            neu = bool(req.get("neu", "seite" not in req))

            def ganz(v, lo, hi):
                return isinstance(v, int) and lo <= v <= hi

            if not (ganz(anzahl, 1, 255) and ganz(seite, 1, 999)):
                send(cl, {"ok": False, "fehler": "anzahl 1..255, seite "
                          "1..999"})
                return
            try:
                grund, erg = dia.seite_abrufen(cl, seite, anzahl, neu)
                if grund:
                    send(cl, {"ok": False, "fehler": grund})
                elif erg:
                    send(cl, erg)
            except OSError as e:
                dia.pending = None
                send(cl, {"ok": False, "fehler": "Senden fehlgeschlagen: %s"
                          % os.strerror(e.errno)})
        elif befehl == "letzte":
            send(cl, dia.letzte or {"ok": False,
                                    "fehler": "noch keine DDS-Daten"})
        elif befehl == "setzen":
            dauer = req.get("dauer")
            if dauer is not None and not (isinstance(dauer, (int, float))
                                          and 0 < dauer <= 3600):
                send(cl, {"ok": False, "fehler": "dauer 0..3600 s"})
                return
            grund = quellen.setzen(str(req.get("signal")), req.get("wert"),
                                   dauer)
            send(cl, {"ok": False, "fehler": grund} if grund else {"ok": True})
        elif befehl == "quellen":
            send(cl, {"ok": True, "rolle": rolle, "signale": quellen.werte()})
        elif befehl == "softkeys":
            gruppen, rueck = softkey_liste(quellen, pd_db, rolle)
            send(cl, {"ok": True, "rolle": rolle, "dauer": o["softkey_dauer"],
                      "gruppen": gruppen, "rueckmeldungen": rueck})
        elif befehl == "softkey":
            # nur Softkeys der Liste - die Oberflaeche setzt keine
            # beliebigen Signale
            signal_name = str(req.get("signal"))
            gruppen, _ = softkey_liste(quellen, pd_db, rolle)
            if not any(t["signal"] == signal_name for g in gruppen
                       for t in g["tasten"]):
                send(cl, {"ok": False, "fehler": "kein Softkey der Rolle %s: %s"
                          % (rolle, signal_name)})
                return
            grund = quellen.setzen(signal_name, 1, o["softkey_dauer"])
            if grund:
                send(cl, {"ok": False, "fehler": grund})
                return
            log("Softkey %s (%g s)" % (signal_name, o["softkey_dauer"]))
            send(cl, {"ok": True, "dauer": o["softkey_dauer"]})
        elif befehl == "status":
            send(cl, {"ok": True, "rolle": rolle, "adresse": adresse,
                      "lebenszeichen": wert,
                      "abfrage_laeuft": bool(dia.pending),
                      "frames_empfangen": dia.rx,
                      "frames_andere_geraete": dia.fremd,
                      "pd_ports": len(pd.faellig) if pd else 0,
                      "pd_lesungen": pd.lesungen if pd else 0,
                      "pd_geschrieben": pd_db.geschrieben if pd_db else 0,
                      "pd_nicht_geschrieben": pd_db.verworfen if pd_db else 0})
        else:
            send(cl, {"ok": False, "fehler": "unbekannter Befehl"})

    naechster = time.time() + o["takt"]
    letzte_meldung = time.time()
    while not stop["jetzt"]:
        wait = 0.01 if dia.pending or dia.conns else 0.1
        bald = min(quellen.naechste(), naechster)
        if pd and pd.faellig:
            bald = min(bald, pd.naechste())
        wait = min(wait, max(0.0, bald - time.time()))
        try:
            r, _, _ = select.select([srv] + list(clients), [], [], wait)
        except InterruptedError:
            continue
        for s in r:
            if s is srv:
                try:
                    cl, _ = srv.accept()
                    cl.setblocking(False)
                    clients[cl] = b""
                except OSError:
                    pass
                continue
            try:
                data = s.recv(4096)
            except OSError:
                data = b""
            if not data:
                clients.pop(s, None)
                s.close()
                continue
            clients[s] += data
            while s in clients and b"\n" in clients[s]:
                line, clients[s] = clients[s].split(b"\n", 1)
                if line.strip():
                    request(s, line.decode("utf-8", errors="replace"))

        # Empfangene Message-Frames leer lesen - auch ohne Abfrage, sonst
        # laeuft der Ring des Treibers ueber
        while True:
            try:
                f = mvb.md_read()
            except OSError as e:
                log("Message-Empfang: %s" % os.strerror(e.errno))
                break
            if f is None:
                break
            try:
                cl, erg = dia.handle(f)
            except OSError as e:
                log("Message-Senden: %s" % os.strerror(e.errno))
                continue
            if erg is not None:
                send(cl, erg)
        cl, erg = dia.check_timeout()
        if erg is not None:
            send(cl, erg)
        if pd:
            pd.abfragen(time.time())

        if time.time() >= naechster:
            naechster += o["takt"]
            wert = (wert + 1) & 0xFFFF
            quellen.lebenszeichen(wert)
            if time.time() - letzte_meldung >= 600:
                log("Lebenszeichen laeuft, Wert %04X, Frames %d (andere "
                    "Geraete %d), Prozessdaten-Lesungen %d"
                    % (wert, dia.rx, dia.fremd, pd.lesungen if pd else 0))
                letzte_meldung = time.time()
        # Source-Ports im Takt ihrer Zykluszeit und nach jeder Aenderung
        quellen.senden(time.time())

    # STOP und close() nehmen das Geraet vom Bus und setzen den
    # Controller zurueck
    for cl in list(clients):
        cl.close()
    srv.close()
    try:
        os.unlink(o["socket"])
    except OSError:
        pass
    try:
        mvb.io("STOP")
    except OSError:
        pass
    if o["dsw_md"]:
        try:
            mvb.io("WRITE_DSW", DSW_MD << 16)
        except OSError:
            pass
    os.close(fd)
    log("beendet, letztes Lebenszeichen %04X" % wert)
    return 0


sys.exit(main())
