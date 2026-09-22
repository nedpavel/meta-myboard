#!/usr/bin/env python3
"""Differenztest des LLI gegen den Originaltreiber.

Fuehrt eine feste Folge von ioctls, Schreib- und Lesezugriffen auf
/dev/mvblli0 aus und haelt nach jedem Schritt fest:

  * Rueckgabewert und errno des Aufrufs
  * den gesamten Traffic Memory (256 KiB, Registerblock ausgespart)
  * den Registersatz des MVBC (IVR ausgespart)

Einmal mit dem originalen LLI laufen lassen, einmal mit dem Nachbau,
dann die beiden Verzeichnisse vergleichen. Was dabei gleich ist, ist
geprueft - und zwar bis aufs Byte, nicht nur an 32 Registern.

    mvbdiff.py run     <Verzeichnis>
    mvbdiff.py compare <VerzeichnisA> <VerzeichnisB>

Ohne weitere Angabe wird der Controller NICHT gestartet: kein MVB_GO,
nichts geht auf den Bus, alles Geschriebene bleibt im Traffic Memory.
Zwei Schalter gehen darueber hinaus und gehoeren nur an einen Bus, an
dem das erlaubt ist:

    --md    Message-Daten senden
    --go    Controller starten (MVB_GO), Prozessdaten im Betrieb lesen,
            Zaehler und Interrupts ueber drei Minuten beobachten

Mit --go veraendert der laufende Bus den Traffic Memory staendig. Der
Vergleich der Abzuege ist dann nicht mehr aussagekraeftig - dort zaehlt
das Protokoll: Rueckgaben, gelesene Daten, freshness, Zaehlerstaende.
"""

import errno as E
import fcntl
import glob
import mmap
import os
import struct
import sys
import time

DEV = "/dev/mvblli0"
BOARD = "/dev/mvb0"

BAR_SIZE = 0x4000000
ISA = 0x2000000
TM = ISA + 0x40000
SA_OFF = 0x0FC00
REG = 0x380
IVR = (0x3C8, 0x3CC)

# Traffic Memory ohne den Registerblock: zwei Stuecke mit einer Luecke
CHUNKS = ((0x00000, 0x0FF80), (0x10000, 0x30000))

REGIONS = [
    (0x00000, 0x02000, "la_pit"),
    (0x02000, 0x04000, "da_pit"),
    (0x04000, 0x08000, "da_pcs"),
    (0x08000, 0x0FC00, "frei / Message-Ringe"),
    (0x0FC00, 0x10000, "Service Area"),
    (0x10000, 0x20000, "la_data"),
    (0x20000, 0x30000, "la_frce"),
    (0x30000, 0x38000, "la_pcs"),
    (0x38000, 0x40000, "da_data"),
]

REGNAMES = {
    0x380: "SCR", 0x384: "MCR", 0x388: "DR", 0x38C: "STSR",
    0x390: "FC", 0x394: "EC", 0x398: "MFR", 0x39C: "MFRE",
    0x3A0: "MR", 0x3A4: "MR2", 0x3A8: "DPR", 0x3AC: "DPR2",
    0x3B0: "IPR0", 0x3B4: "IPR1", 0x3B8: "IMR0", 0x3BC: "IMR1",
    0x3C0: "ISR0", 0x3C4: "ISR1", 0x3C8: "IVR0", 0x3CC: "IVR1",
    0x3D0: "ECA", 0x3D4: "ECB", 0x3D8: "DAOR", 0x3DC: "DAOK",
    0x3E0: "TCR", 0x3F0: "TR1", 0x3F4: "TR2", 0x3F8: "TC1",
    0x3FC: "TC2",
}

# ---------------------------------------------------------------- ioctls
IOC = {
    "READ_DEV_ADDR":   0x80024C01,
    "WRITE_DEV_ADDR":  0x40024C02,
    "READ_DSW":        0x80024C03,
    "WRITE_DSW":       0x40044C04,
    "START":           0x00004C05,
    "STOP":            0x00004C06,
    "RETRIGGER":       0x40044C07,
    "READ_STATS":      0x40644C08,
    "REC_CONF":        0x40044C09,
    "REC_DEL":         0x40044C0A,
    "PD_NSDB":         0x40104C0B,
    "PD_CONF":         0x40024C0C,
    "MD_NSDB":         0x40104C0D,
    "MD_CONF":         0x40024C0E,
    "BA_NSDB":         0x40104C0F,
    "DISABLE_PORT":    0x40024C10,
    "READ_TM":         0x80184C11,
    "MD_FLUSH_QUEUE":  0x00004C12,
    "MD_GET_STATUS":   0x80044C13,
    "WRITE_CONTROL":   0x40064C14,
    "HWINIT":          0x40104C15,
    "USERS":           0x80044C16,
}

# Diese Schritte duerfen sich unterscheiden: der Nachbau baut die
# NSDB-Wege bewusst nicht nach und meldet sich dort ab.
KNOWN_DIFF = {"PD_NSDB"}

# Portliste fuer PD_CONF - deckt alle Groessenklassen ab.
# (Adresse, Groesse, Typ: 1 = Senke, 2 = Quelle)
PORTS = [
    (491,  4, 2),
    (192, 32, 1),
    (290, 16, 1),
    (138,  8, 1),
    (100,  2, 1),
    (101, 32, 1),
]
WRITE_PORT, WRITE_SIZE = 491, 4

TEST_ADDR = 240          # die eigene Adresse des Geraets


def mvb_port(typ, port):
    """Container von read()/write(): 38 Byte, type und port vorbelegt."""
    b = bytearray(38)
    struct.pack_into("<HH", b, 0, typ, port)
    return b


def pd_write(fd, buf, size):
    """Der Treiber liest immer die vollen 38 Byte, als Laenge sieht er
    aber die Portgroesse. Das geht nur mit einem Ausschnitt ueber einem
    ausreichend grossen Puffer."""
    return os.writev(fd, [memoryview(buf)[:size]])


def pd_read(fd, buf, size):
    return os.readv(fd, [memoryview(buf)[:size]])


def sig(req):
    """fcntl.ioctl will die Nummer vorzeichenbehaftet."""
    return req - (1 << 32) if req >= (1 << 31) else req


class Log:
    def __init__(self, path):
        self.f = open(path, "w")
        self.step = 0

    def line(self, s):
        self.f.write(s + "\n")
        print(s)

    def result(self, name, ret, err):
        self.step += 1
        if err is None:
            self.line("%02d %-22s ok        %s" % (self.step, name, ret))
        else:
            self.line("%02d %-22s FEHLER    %s (%d)"
                      % (self.step, name, errname(err), err))
        return self.step

    def close(self):
        self.f.close()


def errname(n):
    for k, v in vars(E).items():
        if k.startswith("E") and v == n:
            return k
    return "errno %d" % n


def call(log, name, fn):
    """Fuehrt fn aus, protokolliert Ergebnis oder errno."""
    try:
        ret = fn()
        return log.result(name, "" if ret is None else repr(ret), None), ret
    except OSError as e:
        return log.result(name, "", e.errno), None


# ------------------------------------------------------------- Abzuege
def snapshot(mm, outdir, step, name):
    parts = []
    for off, ln in CHUNKS:
        parts.append(mm[TM + off:TM + off + ln])
    with open(os.path.join(outdir, "%02d_%s.bin" % (step, name)), "wb") as f:
        for p in parts:
            f.write(p)

    lines = []
    for off in range(REG, 0x400, 4):
        nm = REGNAMES.get(off, "")
        if off in IVR:
            lines.append("SA+0x%03X  ----  %-5s uebersprungen" % (off, nm))
        else:
            v = struct.unpack_from("<H", mm, TM + SA_OFF + off)[0]
            lines.append("SA+0x%03X  %04X  %s" % (off, v, nm))
    with open(os.path.join(outdir, "%02d_%s.regs" % (step, name)), "w") as f:
        f.write("\n".join(lines) + "\n")


def modules():
    out = []
    for p in sorted(glob.glob("/sys/module/pixy_mvb*/parameters/../srcversion")):
        mod = p.split("/")[3]
        try:
            with open(p) as f:
                out.append("%s %s" % (mod, f.read().strip()))
        except OSError:
            pass
    if not out:
        for d in sorted(glob.glob("/sys/module/pixy_mvb*")):
            out.append(os.path.basename(d))
    return out


# ---------------------------------------------------------------- Ablauf
def run(outdir, allow_md, go):
    os.makedirs(outdir, exist_ok=True)
    log = Log(os.path.join(outdir, "log.txt"))

    log.line("Treiber: " + ", ".join(modules()))
    log.line("")

    fb = os.open(BOARD, os.O_RDWR)
    try:
        mm = mmap.mmap(fb, BAR_SIZE, mmap.MAP_SHARED, mmap.PROT_READ)
    finally:
        os.close(fb)

    fd = os.open(DEV, os.O_RDWR)
    log.line("geoeffnet: " + DEV)

    # Sicherheitsabfrage. Solange der Controller in CONFIG steht, bleibt
    # alles Folgende im Traffic Memory und geht nicht auf den Bus. Laeuft
    # er, ist dieses Skript das falsche Werkzeug.
    scr = struct.unpack_from("<H", mm, TM + SA_OFF + 0x380)[0]
    il = scr & 3
    log.line("SCR 0x%04X, IL = %d (%s)"
             % (scr, il, ("RESET", "CONFIG", "TEST", "RUNNING")[il]))
    if il == 3 and not go:
        os.close(fd)
        log.line("ABBRUCH: der Controller laeuft. Dieses Skript schreibt "
                 "in Portpuffer und Register und darf das nur bei "
                 "gestopptem Controller tun.")
        log.close()
        sys.exit(3)
    snapshot(mm, outdir, 0, "open")

    def io(req, buf=0, mutate=False):
        if isinstance(buf, bytearray):
            return fcntl.ioctl(fd, sig(req), buf, mutate)
        return fcntl.ioctl(fd, sig(req), buf)

    # ---- Fehlerpfade: erwartet wird ueberall ein Fehler ----
    log.line("\n--- Fehlerpfade ---")
    call(log, "START ohne Adresse", lambda: io(IOC["START"]))
    call(log, "STOP bei Stillstand", lambda: io(IOC["STOP"]))
    call(log, "RETRIGGER", lambda: io(IOC["RETRIGGER"], bytearray(4), True))
    call(log, "REC_CONF", lambda: io(IOC["REC_CONF"], bytearray(4), True))
    call(log, "REC_DEL", lambda: io(IOC["REC_DEL"], bytearray(4), True))
    call(log, "MD_NSDB", lambda: io(IOC["MD_NSDB"], bytearray(16), True))
    call(log, "BA_NSDB", lambda: io(IOC["BA_NSDB"], bytearray(16), True))
    call(log, "PD_NSDB", lambda: io(IOC["PD_NSDB"], bytearray(16), True))
    call(log, "arg = NULL", lambda: io(IOC["READ_DEV_ADDR"], 0))
    call(log, "unbekannte Nummer", lambda: io(0x80024C17, bytearray(2), True))
    call(log, "falsches Magic", lambda: io(0x80024D01, bytearray(2), True))

    # ---- Konfiguration ----
    log.line("\n--- Konfiguration ---")
    b = bytearray(2)
    call(log, "READ_DEV_ADDR", lambda: io(IOC["READ_DEV_ADDR"], b, True))
    log.line("     Adresse vorher: 0x%04X" % struct.unpack("<H", b)[0])

    call(log, "WRITE_DEV_ADDR",
         lambda: io(IOC["WRITE_DEV_ADDR"],
                    bytearray(struct.pack("<H", TEST_ADDR)), True))
    snapshot(mm, outdir, log.step, "devaddr")

    b2 = bytearray(2)
    call(log, "READ_DEV_ADDR", lambda: io(IOC["READ_DEV_ADDR"], b2, True))
    log.line("     Adresse nachher: 0x%04X" % struct.unpack("<H", b2)[0])

    hw = bytearray(struct.pack("<QBBHHB x", 0, 0, 0, 0, 0, 0))
    call(log, "HWINIT", lambda: io(IOC["HWINIT"], hw, True))
    snapshot(mm, outdir, log.step, "hwinit")

    call(log, "MD_CONF",
         lambda: io(IOC["MD_CONF"], bytearray(struct.pack("<H", 0)), True))
    snapshot(mm, outdir, log.step, "mdconf")

    cfg = struct.pack("<H", len(PORTS))
    for a, s, t in PORTS:
        cfg += struct.pack("<HHH", a, s, t)
    call(log, "PD_CONF", lambda: io(IOC["PD_CONF"], bytearray(cfg), True))
    snapshot(mm, outdir, log.step, "pdconf")

    # ---- Abfragen ----
    log.line("\n--- Abfragen ---")
    tmb = bytearray(24)
    call(log, "READ_TM", lambda: io(IOC["READ_TM"], tmb, True))
    ts, addr, sid = struct.unpack("<ixxxxQi4x", tmb)
    log.line("     ts_id %d  size_id %d" % (ts, sid))

    st = bytearray(0x64)
    call(log, "READ_STATS", lambda: io(IOC["READ_STATS"], st, True))
    f = struct.unpack_from("<7H2x4I", st, 0)
    log.line("     is_active %d is_init %d has_pd %d has_md %d addr 0x%04X"
             % (f[0], f[1], f[2], f[3], f[5]))
    log.line("     frames %u errors %u a %u b %u" % f[7:11])
    log.line("     t_ignore %d line_config %d"
             % struct.unpack_from("<HH", st, 0x60))

    g = bytearray(4)
    call(log, "MD_GET_STATUS", lambda: io(IOC["MD_GET_STATUS"], g, True))
    log.line("     status 0x%08X" % struct.unpack("<I", g)[0])

    u = bytearray(4)
    call(log, "USERS", lambda: io(IOC["USERS"], u, True))
    log.line("     users %d" % struct.unpack("<I", u)[0])

    d = bytearray(2)
    call(log, "READ_DSW", lambda: io(IOC["READ_DSW"], d, True))
    log.line("     dsw 0x%04X" % struct.unpack("<H", d)[0])

    # ---- Schreibende Zugriffe, Controller bleibt gestoppt ----
    log.line("\n--- Schreibende Zugriffe ---")
    call(log, "WRITE_DSW",
         lambda: io(IOC["WRITE_DSW"],
                    bytearray(struct.pack("<I", 0x00FF0011)), True))
    snapshot(mm, outdir, log.step, "writedsw")

    # Kommandobyte 0x03 = cla|clb: setzt nur die Fehlerzaehler zurueck.
    # Die Leitungsbits 0x0C bleiben aus - die wuerden die Leitungswahl
    # umschalten, und die Karte haengt am Bus.
    call(log, "WRITE_CONTROL",
         lambda: io(IOC["WRITE_CONTROL"],
                    bytearray(struct.pack("<HHBx", TEST_ADDR, 43, 0x03))))
    snapshot(mm, outdir, log.step, "writectrl")

    # Prozessdaten schreiben: zweimal, damit die Seitenumschaltung sichtbar wird
    for n, pat in ((1, 0xA5), (2, 0x5A)):
        port = mvb_port(1, WRITE_PORT)
        for i in range(WRITE_SIZE // 2):
            struct.pack_into("<H", port, 4 + i * 2, pat << 8 | pat)
        call(log, "write() PD #%d" % n,
             lambda p=port: pd_write(fd, p, WRITE_SIZE))
        snapshot(mm, outdir, log.step, "writepd%d" % n)

    rd = mvb_port(1, WRITE_PORT)
    st, _ = call(log, "read() PD", lambda: pd_read(fd, rd, WRITE_SIZE))

    if allow_md:
        md = mvb_port(3, 6)
        for i in range(2, 16):
            struct.pack_into("<H", md, 4 + i * 2, 0x1234)
        call(log, "write() MD", lambda: pd_write(fd, md, 32))
        snapshot(mm, outdir, log.step, "writemd")
        call(log, "MD_FLUSH_QUEUE", lambda: io(IOC["MD_FLUSH_QUEUE"]))
        snapshot(mm, outdir, log.step, "mdflush")
    else:
        log.line("   (Message-Daten uebersprungen, --md erlaubt sie)")

    call(log, "DISABLE_PORT",
         lambda: io(IOC["DISABLE_PORT"],
                    bytearray(struct.pack("<H", PORTS[-1][0])), True))
    snapshot(mm, outdir, log.step, "disable")

    # ---- Betrieb: nur mit --go, hier laeuft der Controller wirklich ----
    if go:
        log.line("\n--- Betrieb (MVB_GO) ---")
        ok, _ = call(log, "START", lambda: io(IOC["START"]))
        snapshot(mm, outdir, log.step, "started")

        def counters():
            out = {}
            for f in sorted(glob.glob(
                    "/sys/module/pixy_mvb*/parameters/dbg_*")):
                try:
                    with open(f) as h:
                        out[f.split("/")[3] + "/" + os.path.basename(f)] = \
                            h.read().strip()
                except OSError:
                    pass
            return out

        c0 = counters()
        r0 = [struct.unpack_from("<H", mm, TM + SA_OFF + o)[0]
              for o in (0x390, 0x394)]
        log.line("     FC %04X  EC %04X" % tuple(r0))

        for round_ in range(3):
            time.sleep(20)
            r = [struct.unpack_from("<H", mm, TM + SA_OFF + o)[0]
                 for o in (0x390, 0x394, 0x3C4, 0x3B0)]
            log.line("     +%2ds  FC %04X  EC %04X  ISR1 %04X  IPR0 %04X"
                     % ((round_ + 1) * 20, r[0], r[1], r[2], r[3]))

            for a_, sz, t in PORTS:
                if t != 1:
                    continue
                buf = mvb_port(1, a_)
                try:
                    pd_read(fd, buf, sz)
                    data = bytes(buf[4:4 + min(sz, 8)]).hex()
                    fresh = struct.unpack_from("<H", buf, 36)[0]
                    log.line("       Port %4d  %-16s  freshness %5d"
                             % (a_, data, fresh))
                except OSError as e:
                    log.line("       Port %4d  %s" % (a_, errname(e.errno)))

        c1 = counters()
        log.line("     Zaehler:")
        for k in sorted(set(c0) | set(c1)):
            if c0.get(k) != c1.get(k):
                log.line("       %-36s %s -> %s"
                         % (k, c0.get(k, "-"), c1.get(k, "-")))
        if c0 == c1:
            log.line("       unveraendert - keine Interrupts im Betrieb")

        st2 = bytearray(0x64)
        call(log, "READ_STATS (Betrieb)",
             lambda: io(IOC["READ_STATS"], st2, True))
        log.line("     frames %u errors %u a %u b %u"
                 % struct.unpack_from("<4I", st2, 0x10))

        g2 = bytearray(4)
        call(log, "MD_GET_STATUS (Betrieb)",
             lambda: io(IOC["MD_GET_STATUS"], g2, True))
        log.line("     status 0x%08X" % struct.unpack("<I", g2)[0])

        snapshot(mm, outdir, log.step, "running")
        call(log, "STOP", lambda: io(IOC["STOP"]))
        snapshot(mm, outdir, log.step, "stopped")

    # ---- Exklusivitaet ----
    log.line("\n--- Exklusivitaet ---")
    def second_open():
        f2 = os.open(DEV, os.O_RDWR)
        os.close(f2)
        return "zweites open war moeglich"
    call(log, "zweites open()", second_open)

    snapshot(mm, outdir, 99, "ende")
    os.close(fd)
    log.line("\ngeschlossen")
    mm.close()
    log.close()


# ------------------------------------------------------------ Vergleich
def region(off):
    for a, b, n in REGIONS:
        if a <= off < b:
            return n
    return "?"


def bin_offset(i):
    """Index in der Abzugsdatei -> Offset im Traffic Memory."""
    first = CHUNKS[0][1]
    return i if i < first else CHUNKS[1][0] + (i - first)


def compare(da, db):
    rc = 0
    la = open(os.path.join(da, "log.txt")).read().splitlines()
    lb = open(os.path.join(db, "log.txt")).read().splitlines()

    print("=== Protokoll ===")
    diff = 0
    for a, b in zip(la, lb):
        if a != b and not a.startswith("Treiber:"):
            tag = ""
            if any(k in a for k in KNOWN_DIFF):
                tag = "   (bekannter Unterschied)"
            else:
                diff += 1
            print("  A: %s\n  B: %s%s" % (a, b, tag))
    if len(la) != len(lb):
        print("  Protokolle verschieden lang: %d gegen %d" % (len(la), len(lb)))
        diff += 1
    print("  %d unerwartete Abweichungen" % diff)
    rc += diff

    print("\n=== Speicherabzuege ===")
    fa = sorted(os.path.basename(p) for p in glob.glob(os.path.join(da, "*.bin")))
    fb = sorted(os.path.basename(p) for p in glob.glob(os.path.join(db, "*.bin")))
    if fa != fb:
        print("  unterschiedliche Schrittfolge:")
        print("   nur in A: %s" % sorted(set(fa) - set(fb)))
        print("   nur in B: %s" % sorted(set(fb) - set(fa)))
        return rc + 1

    for name in fa:
        A = open(os.path.join(da, name), "rb").read()
        B = open(os.path.join(db, name), "rb").read()
        if A == B:
            print("  %-24s gleich" % name)
            continue
        bad = {}
        for i in range(min(len(A), len(B))):
            if A[i] != B[i]:
                off = bin_offset(i)
                r = region(off)
                if r not in bad:
                    bad[r] = [0, off]
                bad[r][0] += 1
        print("  %-24s UNTERSCHIED" % name)
        for r in sorted(bad, key=lambda k: bad[k][1]):
            cnt, first = bad[r]
            print("      %-22s %5d Byte, ab TM+0x%05X" % (r, cnt, first))
        rc += 1

    print("\n=== Register ===")
    for name in sorted(os.path.basename(p)
                       for p in glob.glob(os.path.join(da, "*.regs"))):
        A = open(os.path.join(da, name)).read().splitlines()
        B = open(os.path.join(db, name)).read().splitlines()
        d = [(x, y) for x, y in zip(A, B) if x != y]
        if not d:
            print("  %-24s gleich" % name)
            continue
        print("  %-24s UNTERSCHIED" % name)
        for x, y in d:
            print("      A: %s\n      B: %s" % (x, y))
        rc += 1

    print("\n%s" % ("ALLES GLEICH" if rc == 0 else "%d Stellen weichen ab" % rc))
    return rc


def main():
    if len(sys.argv) >= 3 and sys.argv[1] == "run":
        run(sys.argv[2], "--md" in sys.argv, "--go" in sys.argv)
    elif len(sys.argv) == 4 and sys.argv[1] == "compare":
        sys.exit(1 if compare(sys.argv[2], sys.argv[3]) else 0)
    else:
        print(__doc__)
        sys.exit(2)


main()
