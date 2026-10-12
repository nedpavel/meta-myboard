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

    --dauer S        so lange zuhoeren (Vorgabe 60)
    --nur-hoeren     keine Anfrage senden, nur auf DIA1 antworten
    --keine-antwort  in Teil 2 keine Reply-Message an DIA1 senden
    --ziel N         Geraeteadresse von DIA1 (Vorgabe 6)
    --ref HEX        Referenz der eigenen Anfrage (Vorgabe 0043)
    --hoch           Frames mit hoher Prioritaet senden (Vorgabe niedrig)
    --single-line    Einleitungsbetrieb auf Leitung A
    --dsw-md         MD-Bit im DSW setzen, am Ende wieder loeschen

Nur am Laborbus. Der Controller laeuft waehrend des Tests (MVB_GO).
Gelesen wird nur unterhalb von SA+0x3C0, die Interruptzeile bleibt
unberuehrt - der Message-Empfang braucht DTI1.
"""

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
    "MD_CONF":        0x40024C0E,
    "WRITE_CONTROL":  0x40064C14,
    "HWINIT":         0x40104C15,
}
MD_HIGH, MD_LOW = 2, 3
OWN_ADDR = 240
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
        self.f = open(path, "w")
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
         "ziel": 6, "ref": 0x0043, "hoch": False, "single": False,
         "dsw_md": False}
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
        else:
            print("Unbekannte Option: %s\n" % a)
            print(__doc__)
            sys.exit(2)
        i += 1
    return o


def main():
    o = parse_args(sys.argv)
    os.makedirs(o["dir"], exist_ok=True)
    log = Log(os.path.join(o["dir"], "log.txt"))
    raw = open(os.path.join(o["dir"], "frames.txt"), "w")
    names = {OWN_ADDR: "IDU2", o["ziel"]: "DIA1"}
    typ = MD_HIGH if o["hoch"] else MD_LOW

    log.line("DDS-Abfrage an DIA1 (Geraet %d), eigene Adresse %d, %s, %s"
             % (o["ziel"], OWN_ADDR, "nur hoeren" if o["nur_hoeren"]
                else "Anfrage Ref %04X" % o["ref"],
                "Prioritaet hoch" if o["hoch"] else "Prioritaet niedrig"))

    fd = os.open(DEV, os.O_RDWR)

    def io(req, arg=0, mutate=False):
        if isinstance(arg, bytearray):
            return fcntl.ioctl(fd, sig(req), arg, mutate)
        return fcntl.ioctl(fd, sig(req), arg)

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
        step("MD_CONF", lambda: io(IOC["MD_CONF"],
                                   bytearray(struct.pack("<H", 0)), True))
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

        def dsw():
            b = bytearray(2)
            try:
                io(IOC["READ_DSW"], b, True)
                return "%04X" % struct.unpack("<H", b)[0]
            except OSError:
                return "?"

        def tx(tail, note):
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

        def rx():
            buf = bytearray(38)
            struct.pack_into("<HH", buf, 0, typ, o["ziel"])
            os.readv(fd, [memoryview(buf)[:32]])
            return bytes(buf[4:36])

        # Eingehende Messages je Verbindung (Quelle + Netzkopf)
        conns = {}
        got = {"rx": 0, "tx": 0, "antwort1": None, "daten2": None,
               "cc_anfrage": False, "cc_reply": False}
        reply_ref = (o["ref"] + 1) & 0xFFFF

        def handle(f):
            d = decode(f)
            got["rx"] += 1
            log.line("RX %s" % describe(d, names))
            raw.write("%.3f RX %s\n" % (time.time() - log.t0, f.hex()))
            raw.flush()
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
            for k in range(0, len(msg), 32):
                log.line("   %04X  %s" % (k, msg[k:k + 32].hex(" ")))
            if c["call"] and not o["keine_antwort"]:
                net = swap_net(c["net"])
                tx(bytes((0x17,)) + net + bytes((0x00,))
                   + struct.pack(">HI", reply_ref, len(REPLY_DATA))
                   + bytes((0x70, 0x00)) + REPLY_DATA + REPLY_PAD,
                   "Reply an DIA1 Ref %04X" % reply_ref)

        q_last = qdt()
        d_last = dti1()
        log.line("Status  QDT xmit0 %04X xmit1 %04X rcve %04X  DSW %s  "
                 "dbg_dti1 %s" % (q_last + (dsw(), d_last)))

        t0 = time.time()
        call_sent = o["nur_hoeren"]
        next_status = t0 + 5
        while time.time() - t0 < o["dauer"]:
            # Kurz warten, damit der Busmaster das Geraet nach START sieht
            if not call_sent and time.time() - t0 >= 2:
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
            if q != q_last:
                what = []
                if q[:2] != q_last[:2]:
                    what.append("Sendequeue bewegt - gesendet")
                if q[2] != q_last[2]:
                    what.append("Empfangsqueue bewegt")
                log.line("QDT  xmit0 %04X xmit1 %04X rcve %04X  (%s)"
                         % (q + (", ".join(what),)))
                q_last = q
            if time.time() >= next_status:
                d_now = dti1()
                log.line("Status  QDT xmit0 %04X xmit1 %04X rcve %04X  "
                         "DSW %s  dbg_dti1 %s" % (q + (dsw(), d_now)))
                next_status += 5
            time.sleep(0.01)

        # Zusammenfassung
        log.line()
        log.line("Zusammenfassung")
        d_end = dti1()
        log.line("   Frames empfangen %d, dbg_dti1 %s -> %s"
                 % (got["rx"], d_last, d_end))
        if not o["nur_hoeren"]:
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
