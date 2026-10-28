#!/usr/bin/env python3
"""myboard-tasten: Hardkeys der Front des Pixy 1000 nach MariaDB.

Die 32 Tasten um den Bildschirm meldet der System-Controller als
USB-Tastatur (INC-100 FW User Manual, 5.11 "Front panel keyboard"; Pixy
1000 und INC-100 sind darin gleich). Der Dienst liest die Ereignisse
direkt vom Kernel (/dev/input/event*), ohne die Tastatur zu greifen - die
Qt-Oberflaeche bzw. Firefox bekommen jede Taste wie bisher.

Je Druck eine Zeile in myboard.tasten, geschrieben sofort beim Druecken;
beim Loslassen kommen losgelassen und dauer_ms dazu. Haelt man eine Taste,
schickt der Kernel Wiederholungen - die zaehlen nicht als neuer Druck.

Beschriftung und Position (P41,n) stammen aus der Standardbelegung des
Handbuchs (Figure 8). Ist die Belegung im System-Controller geaendert
(Parameter 41), passen taste und position nicht mehr; zeichen, keycode
und kernel_name zeigen dann, was wirklich kam. Tasten anderer Tastaturen
(z. B. per USB angesteckt) landen ebenfalls in der Tabelle, mit leerer
Position und dem Namen des Geraets.

Optionen:
    --db-socket PFAD   Socket von MariaDB (Vorgabe /var/lib/mysql/mysql.sock)
    --geraet PFAD      nur dieses Eingabegeraet lesen (mehrfach; zum Testen)
    --behalten N       hoechstens N Zeilen behalten (Vorgabe 100000)

Fehlt MariaDB, bleiben die Drucke im Speicher und werden nachgetragen,
sobald sie erreichbar ist.
"""

import errno
import os
import pwd
import select
import signal
import struct
import sys
import time

DB_SOCK = "/var/lib/mysql/mysql.sock"
SYSFS = "/sys/class/input"
EV_KEY = 1
EREIGNIS = struct.Struct("llHHi")       # struct input_event (64 Bit)
NEU_SUCHEN = 3          # s: neue Eingabegeraete (USB kommt spaeter)
WARTEN = 5              # s bis zum naechsten Versuch mit MariaDB
KUERZEN = 600           # s zwischen zwei Kuerzungen der Tabelle
OFFEN_MAX = 10000       # Drucke im Speicher, solange MariaDB fehlt

# Standardbelegung der Front, Figure 8 des Handbuchs: Linux-Keycode ->
# (Beschriftung, Position, gesendetes Zeichen, HID Usage ID, Kernel-Name)
FRONT = {
    1:   ("Ein/Aus",        "P41,0",  "ESC",    41, "KEY_ESC"),
    50:  ("Fahne",          "P41,1",  "m",      16, "KEY_M"),
    23:  ("Info",           "P41,2",  "i",      12, "KEY_I"),
    25:  ("Dokument",       "P41,3",  "p",      19, "KEY_P"),
    20:  ("Werkzeug",       "P41,4",  "t",      23, "KEY_T"),
    47:  ("frei 1",         "P41,5",  "v",      25, "KEY_V"),
    17:  ("frei 2",         "P41,6",  "w",      26, "KEY_W"),
    45:  ("Helligkeit",     "P41,7",  "x",      27, "KEY_X"),
    38:  ("Kontrast",       "P41,8",  "l",      15, "KEY_L"),
    19:  ("Umschalten",     "P41,9",  "r",      21, "KEY_R"),
    46:  ("C",              "P41,10", "c",       6, "KEY_C"),
    105: ("Pfeil links",    "P41,11", "Left",   80, "KEY_LEFT"),
    106: ("Pfeil rechts",   "P41,12", "Right",  79, "KEY_RIGHT"),
    103: ("Pfeil hoch",     "P41,13", "Up",     82, "KEY_UP"),
    108: ("Pfeil runter",   "P41,14", "Down",   81, "KEY_DOWN"),
    28:  ("E (Enter)",      "P41,15", "Return", 40, "KEY_ENTER"),
}
for _i in range(9):                     # 1..9: KEY_1..KEY_9 = 2..10
    FRONT[2 + _i] = (str(_i + 1), "P41,%d" % (16 + _i), str(_i + 1),
                     30 + _i, "KEY_%d" % (_i + 1))
FRONT[11] = ("0", "P41,25", "0", 39, "KEY_0")
for _i in range(6):                     # F1..F6: KEY_F1..KEY_F6 = 59..64
    FRONT[59 + _i] = ("F%d" % (_i + 1), "P41,%d" % (26 + _i),
                      "F%d" % (_i + 1), 58 + _i, "KEY_F%d" % (_i + 1))
# Eine Tastatur der Front kennt mindestens diese Tasten
MERKMAL = (2, 59, 103)                  # KEY_1, KEY_F1, KEY_UP

SQL_DRUCK = (
    "INSERT INTO tasten (gedrueckt, losgelassen, dauer_ms, taste, position, "
    "zeichen, hid, keycode, kernel_name, geraet) "
    "VALUES (%s, %s, %s, %s, %s, %s, %s, %s, %s, %s)")
SQL_LOS = "UPDATE tasten SET losgelassen = %s, dauer_ms = %s WHERE id = %s"


def log(msg):
    print(msg, flush=True)              # Journal: journalctl -u myboard-tasten


def zeit_ms(us):
    """Zeit in Mikrosekunden (ganzzahlig, wie vom Kernel) fuer DATETIME(3)."""
    return time.strftime("%Y-%m-%d %H:%M:%S", time.localtime(us // 1000000)) \
        + ".%03d" % (us % 1000000 // 1000)


def tastaturen():
    """{Pfad: Name} der Eingabegeraete, die Tasten der Front kennen."""
    out = {}
    try:
        namen = os.listdir(SYSFS)
    except OSError:
        return out
    for n in namen:
        if not n.startswith("event"):
            continue
        basis = os.path.join(SYSFS, n, "device")
        try:
            with open(os.path.join(basis, "capabilities", "key")) as f:
                worte = f.read().split()
            with open(os.path.join(basis, "name")) as f:
                name = f.read().strip()
        except OSError:
            continue
        # Bitfeld in Worten zu 64 Bit, das hoechste zuerst
        bits = int("".join(w.zfill(16) for w in worte) or "0", 16)
        if all(bits >> k & 1 for k in MERKMAL):
            out["/dev/input/" + n] = name
    return out


class Tasten:
    def __init__(self, sock, behalten):
        self.sock = sock
        self.behalten = behalten
        self.db = None
        self.pymysql = None
        self.naechster_versuch = 0.0
        self.gestoert = False
        self.zu_schreiben = []      # neue Drucke
        self.nachtragen = []        # schon geschriebene, jetzt losgelassen
        self.gedrueckt = {}         # (Geraet, Keycode) -> Druck
        self.verworfen = 0
        self.letzte_kuerzung = 0.0
        try:
            import pymysql
            self.pymysql = pymysql
        except ImportError:
            log("PyMySQL fehlt - Tasten werden nur im Journal gemeldet")

    def ereignis(self, geraet, sek, usek, code, wert):
        t = sek * 1000000 + usek            # ganzzahlig: keine Rundungsfehler
        schluessel = (geraet, code)
        if wert == 1:                               # gedrueckt
            taste, pos, zeichen, hid, kname = FRONT.get(
                code, ("", "", "", None, "KEY_%d" % code))
            druck = {"gedrueckt": t, "los": None, "id": None,
                     "werte": (taste, pos, zeichen, hid, code, kname,
                               geraet[:80])}
            self.gedrueckt[schluessel] = druck
            self.zu_schreiben.append(druck)
            if len(self.zu_schreiben) > OFFEN_MAX:
                self.zu_schreiben.pop(0)
                self.verworfen += 1
        elif wert == 0:                             # losgelassen
            druck = self.gedrueckt.pop(schluessel, None)
            if druck is None:
                return
            druck["los"] = t
            if druck["id"] is not None:
                self.nachtragen.append(druck)
        # wert 2: Wiederholung beim Halten - kein neuer Druck

    def zeile(self, d):
        los = d["los"]
        dauer = (los - d["gedrueckt"] + 500) // 1000 if los else None
        return ((zeit_ms(d["gedrueckt"]), zeit_ms(los) if los else None,
                 dauer) + d["werte"])

    def schreiben(self):
        if not (self.zu_schreiben or self.nachtragen) or not self.pymysql:
            return
        if self.db is None and time.time() < self.naechster_versuch:
            return
        try:
            if self.db is None:
                self.db = self.pymysql.connect(
                    unix_socket=self.sock, database="myboard",
                    charset="utf8mb4", autocommit=True,
                    user=pwd.getpwuid(os.geteuid()).pw_name,
                    connect_timeout=5, read_timeout=10, write_timeout=10)
            with self.db.cursor() as c:
                while self.zu_schreiben:
                    d = self.zu_schreiben[0]
                    c.execute(SQL_DRUCK, self.zeile(d))
                    d["id"] = c.lastrowid
                    self.zu_schreiben.pop(0)
                while self.nachtragen:
                    d = self.nachtragen[0]
                    z = self.zeile(d)
                    c.execute(SQL_LOS, (z[1], z[2], d["id"]))
                    self.nachtragen.pop(0)
                if time.time() - self.letzte_kuerzung > KUERZEN:
                    self.letzte_kuerzung = time.time()
                    c.execute("SELECT MAX(id) FROM tasten")
                    hoechste = c.fetchone()[0]
                    if hoechste and hoechste > self.behalten:
                        c.execute("DELETE FROM tasten WHERE id <= %s",
                                  (hoechste - self.behalten,))
            if self.gestoert:
                log("MariaDB wieder erreichbar%s" % (
                    ", %d Drucke verworfen" % self.verworfen
                    if self.verworfen else ""))
                self.gestoert = False
        except Exception as e:
            if self.db is not None:
                try:
                    self.db.close()
                except Exception:
                    pass
            self.db = None
            self.naechster_versuch = time.time() + WARTEN
            if not self.gestoert:
                log("MariaDB: %s - Drucke bleiben im Speicher, neuer Versuch "
                    "alle %d s" % (e, WARTEN))
                self.gestoert = True


def main():
    sock, feste, behalten = DB_SOCK, [], 100000
    a = sys.argv[1:]
    while a:
        o = a.pop(0)
        if o == "--db-socket" and a:
            sock = a.pop(0)
        elif o == "--geraet" and a:
            feste.append(a.pop(0))
        elif o == "--behalten" and a:
            behalten = int(a.pop(0))
        else:
            log("Aufruf: myboard-tasten [--db-socket PFAD] [--geraet PFAD]... "
                "[--behalten N]")
            return 2

    stop = {"jetzt": False}

    def beenden(signum, frame_):
        stop["jetzt"] = True

    signal.signal(signal.SIGTERM, beenden)
    signal.signal(signal.SIGINT, beenden)

    tasten = Tasten(sock, behalten)
    offen = {}                  # fd -> (Pfad, Name)
    gesucht = 0.0
    log("myboard-tasten: Hardkeys der Front -> myboard.tasten")
    while not stop["jetzt"]:
        if time.time() - gesucht > NEU_SUCHEN:
            gesucht = time.time()
            geraete = ({p: os.path.basename(p) for p in feste} if feste
                       else tastaturen())
            bekannt = {p for p, _ in offen.values()}
            for pfad, name in geraete.items():
                if pfad in bekannt:
                    continue
                try:
                    fd = os.open(pfad, os.O_RDONLY | os.O_NONBLOCK)
                except OSError as e:
                    log("%s (%s): %s" % (pfad, name, e.strerror))
                    continue
                offen[fd] = (pfad, name)
                log("lese %s (%s)" % (pfad, name))
        try:
            r, _, _ = select.select(list(offen), [], [],
                                    1.0 if not tasten.gestoert else 0.5)
        except InterruptedError:
            continue
        for fd in r:
            pfad, name = offen[fd]
            try:
                daten = os.read(fd, EREIGNIS.size * 64)
            except OSError as e:
                if e.errno == errno.EAGAIN:
                    continue
                daten = b""
            if not daten:               # Geraet weg (USB abgezogen)
                log("%s (%s) verschwunden" % (pfad, name))
                os.close(fd)
                del offen[fd]
                continue
            for i in range(0, len(daten) - EREIGNIS.size + 1, EREIGNIS.size):
                sek, usek, typ, code, wert = EREIGNIS.unpack_from(daten, i)
                if typ == EV_KEY:
                    tasten.ereignis(name, sek, usek, code, wert)
        tasten.schreiben()

    tasten.schreiben()
    for fd in offen:
        os.close(fd)
    return 0


if __name__ == "__main__":
    sys.exit(main())
