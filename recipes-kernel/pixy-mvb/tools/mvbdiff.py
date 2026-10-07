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

Vor jedem Lauf wird der Traffic Memory genullt (Registerblock
ausgenommen), damit beide Treiber vom selben Zustand ausgehen. Ohne
das vergleicht man die Reste frueherer Laeufe.

Ohne weitere Angabe wird der Controller NICHT gestartet: kein MVB_GO,
nichts geht auf den Bus, alles Geschriebene bleibt im Traffic Memory.
Zwei Schalter gehen darueber hinaus und gehoeren nur an einen Bus, an
dem das erlaubt ist:

    --md    Message-Daten senden
    --go    Controller starten (MVB_GO), Prozessdaten im Betrieb lesen,
            Zaehler und Interrupts ueber eine Minute beobachten

Dazu:

    --no-wipe       Traffic Memory vorher nicht nullen
    --ping          (nur mit --go) einen Verbindungsaufbau an Geraet 6
                    senden, wie Geraet 6 ihn sonst an uns schickt, und
                    auf die Antwort warten - erzeugt einen Empfangs-
                    Interrupt ohne laufende Anwendung
    --no-pd         PD_CONF und alle Prozessdatenzugriffe auslassen, der
                    Rest laeuft vollstaendig, START eingeschlossen.
                    (--stop-after taugt dafuer nicht: es ueberspringt
                    alles danach, auch START.)
    --stop-after N  nach Schritt N nur noch protokollieren
    --ports DATEI   Portliste "adresse groesse typ" statt der eingebauten

Beim Vergleich bleiben aussen vor: Zaehler, Timer, anstehende Ereignisse
und der zuletzt gesehene Master Frame (VOLATILE_REGS), DR Bit 9 und die
Zaehlerzeilen im Protokoll. Sie werden gezeigt, aber nicht gezaehlt.

Mit --go veraendert der laufende Bus den Traffic Memory staendig. Der
Vergleich der Abzuege ist dann nicht mehr aussagekraeftig - dort zaehlt
das Protokoll: Rueckgaben, gelesene Daten, freshness, Zaehlerstaende.
"""

import difflib
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

# Moegliche Plaetze der Service Area (TM_SERVICE_OFFSETS). Nach einem
# RESET (SCR = 0, so schliesst das Original) liegt der Registerblock am
# Grundplatz 0x3C00, erst MCR schiebt ihn an 0xFC00. Geloescht wird
# deshalb an keinem dieser Plaetze.
SA_CANDIDATES = (0x03C00, 0x07C00, 0x0FC00)
SA_FOR_MCM = (0x03C00, 0x07C00, 0x0FC00, 0x0FC00, 0x0FC00)

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

# Register, die der Bus oder die Zeit veraendert, nicht der Treiber:
# Zaehler, zuletzt gesehener Master Frame, anstehende Ereignisse und die
# Timer, deren Endwert von der Laufzeit der Warteschleife abhaengt. Sie
# werden ausgegeben, aber nicht verglichen.
VOLATILE_REGS = {0x390, 0x394, 0x398, 0x39C, 0x3B0, 0x3B4,
                 0x3D0, 0x3D4, 0x3F8, 0x3FC}

# DR Bit 9 wechselt beim Original gelegentlich; bis die Stichprobe
# gezeigt hat, was es ist, wird es beim Vergleich ausgeblendet.
DR_OFF = 0x388
DR_VOLATILE = 0x0200
DR_SAMPLES = 2000

# Protokollzeilen mit diesen Merkmalen haengen am laufenden Bus.
VOLATILE_LOG = ("frames ", "FC ", "Stichprobe", "fresh ")

# Protokollzeilen ueber den Zustand VOR dem Lauf - der stammt vom zuvor
# geschlossenen Treiber und gehoert nicht zum Vergleich.
PRESTATE_LOG = ("Traffic Memory geloescht", "Vorzustand")

# Die drei Message-Ringe: Name, Byteoffset im TM, Zahl der LLR. Die
# Puffer liegen hinter dem LLR-Feld, auf 32 Byte ausgerichtet; LLR k
# gehoert Puffer k, Puffer 0 (Waechter) bleibt unbenutzt
# (mvb_md_install_q).
RINGS = (("xmit_q0", 0x08000, 8),
         ("xmit_q1", 0x08140, 0xDE),
         ("rcve_q", 0x0A0A0, 0xDE))

# Physische Ports in der Service Area (Index = Dock bzw. PCS-Nummer)
PP_NAMES = {0x0: "FC8", 0x1: "EFS", 0x4: "EF0", 0x5: "EF1", 0x6: "MOS",
            0x7: "FC15", 0x8: "MSRC", 0xC: "MSNK"}

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

# Diese Schritte duerfen sich unterscheiden. Seit der Nachbau die
# Eingabepruefung von PD_NSDB uebernimmt, ist das keiner mehr; eine
# gueltige NSDB wuerde sich unterscheiden, die schickt dieses Skript
# aber nicht.
KNOWN_DIFF = set()

# Portliste fuer PD_CONF. (Adresse, Groesse in Byte, Typ: 1 = Senke,
# 2 = Quelle)
#
# Aus DIA1.TXT und DDA1.TXT, alle Telegramme mit $SINKMEMB IDU2 - das
# ist dieses Geraet. Die vierte Spalte des $TELEGRAM ist die Zykluszeit
# in Millisekunden (64, 128, 256, 1024), nicht die Portgroesse. Die
# Groesse ergibt sich aus dem hoechsten belegten Signaloffset,
# aufgerundet auf die naechste zulaessige MVB-Groesse:
#
#   181  BIT bei 2 (MP-NPanMiddle)    ->  4 Byte, alle 64 ms
#   460  AR8 bei 0                    ->  8 Byte, alle 64 ms
#   461  BOOLEAN1 bei 6               ->  8 Byte, alle 128 ms
#   462  BIT bei 12                   -> 16 Byte, alle 256 ms
#   471  BIT bei 6                    ->  8 Byte, alle 64 ms
#   472  ANALOG bei 30                -> 32 Byte, alle 1024 ms
#   473  CARD8 bei 15                 -> 16 Byte, alle 1024 ms
#   474  CARD32 bei 28                -> 32 Byte, alle 1024 ms
#   475  CARD32 bei 28                -> 32 Byte, alle 1024 ms
#   476  CARD16 bei 30                -> 32 Byte, alle 1024 ms
#   477  ANALOG bei 22                -> 32 Byte, alle 1024 ms
#
# Die Groesse muss stimmen: der F-Code steckt im MVB-Frame, und eine
# Senke nimmt nur Frames mit passendem F-Code an. Wer hier danebenliegt,
# empfaengt nichts - ohne Fehlermeldung. Mit --ports datei laesst sich
# die Liste ohne neues Skript austauschen (je Zeile "Adresse Groesse Typ").
PORTS = [
    (181,  4, 1),   # FLG1N, IN-XMVBLifeSFLG1 - Lebenszeichen, zaehlt hoch
    (460,  8, 1),
    (461,  8, 1),
    (462, 16, 1),
    (471,  8, 1),
    (472, 32, 1),
    (473, 16, 1),
    (474, 32, 1),
    (475, 32, 1),
    (476, 32, 1),
    (477, 32, 1),
    (491,  4, 2),   # eigene Quelle fuer den Schreibtest
    (902,  2, 1),   # nur fuer die kleinste Groessenklasse des Allokators
]
# Ports, deren Inhalt sich staendig aendern muss. Ein gleichbleibender
# Wert waere hier verdaechtig, ein wechselnder ist der eigentliche Beleg,
# dass wirklich empfangen wird und nicht nur einmal etwas dastand.
LIFESIGN = {
    181: "IN-XMVBLifeSFLG1",
    460: "NC-XTimeDate",
    471: "DDA1-XMVBLifeSig",
}

WRITE_PORT, WRITE_SIZE = 491, 4
DISABLE_PORT_ADDR = 902          # Senke: DISABLE_PORT muss EIO liefern

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


STOP_AFTER = None
NO_PD = False
WIPE = True
PING = False

# Ein Verbindungsaufbau, wie Geraet 6 ihn im Produktivabzug an uns
# geschickt hat (rcve_q, dreimal, SZ 27, MTC 0x80), jetzt in Gegen-
# richtung: wir rufen Geraet 6. Die Anwendung hatte jeden dieser Anrufe
# mit einem 6-Byte-Frame beantwortet (SZ 6 = DR/DC). Auf einen
# Verbindungsaufbau antwortet ein Geraet in jedem Fall - bestaetigend
# oder ablehnend -, und jede Antwort loest DTI1 aus. Byte 0..3
# ueberschreibt der Treiber mit dem Link_Header. conn_ref hier 0x0016.
#
# (Erster Versuch war der 6-Byte-Frame selbst - eine Antwort auf einen
# Anruf, den es nie gab; Geraet 6 hat ihn zu Recht ignoriert.)
PING_DEST = 6
PING_FRAME = bytes.fromhex(
    "00000000 1b 00d2 00d2 80 0016 000000457f000000f003000102ffff5505000123"
    .replace(" ", ""))


def call(log, name, fn):
    """Fuehrt fn aus, protokolliert Ergebnis oder errno."""
    if STOP_AFTER is not None and log.step >= STOP_AFTER:
        log.step += 1
        log.line("%02d %-22s uebersprungen (--stop-after)" % (log.step, name))
        return log.step, None
    try:
        ret = fn()
        return log.result(name, "" if ret is None else repr(ret), None), ret
    except OSError as e:
        return log.result(name, "", e.errno), None


# ------------------------------------------------------------- Abzuege
def snapshot(mm, outdir, step, name, sa_off=SA_OFF, tm=True):
    if tm:
        parts = []
        for off, ln in CHUNKS:
            parts.append(mm[TM + off:TM + off + ln])
        with open(os.path.join(outdir, "%02d_%s.bin" % (step, name)),
                  "wb") as f:
            for p in parts:
                f.write(p)

    lines = []
    for off in range(REG, 0x400, 4):
        nm = REGNAMES.get(off, "")
        if off in IVR:
            lines.append("SA+0x%03X  ----  %-5s uebersprungen" % (off, nm))
        else:
            v = struct.unpack_from("<H", mm, TM + sa_off + off)[0]
            lines.append("SA+0x%03X  %04X  %s" % (off, v, nm))
    # Fensterregister der ISA-Bruecke: BASR0/1 legen das TM-Fenster fest,
    # das untere Byte des BCR traegt die Interruptnummer (Parameter irq)
    for off, nm in ((0x320, "BASR0"), (0x322, "BASR1"), (0x324, "BCR")):
        v = struct.unpack_from("<H", mm, ISA + off)[0]
        lines.append("ISA+0x%03X %04X  %s" % (off, v, nm))
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
def holders(path):
    """PIDs, die path offen haben - wie fuser, ohne fuser."""
    pids = []
    for fd_dir in glob.glob("/proc/[0-9]*/fd"):
        try:
            for fd_ in os.listdir(fd_dir):
                if os.readlink(os.path.join(fd_dir, fd_)) == path:
                    pids.append(int(fd_dir.split("/")[2]))
                    break
        except OSError:
            pass
    return pids


def wipe_tm(log):
    """Traffic Memory nullen, damit beide Treiber vom selben Zustand
    ausgehen. Ohne das vergleicht man die Reste frueherer Laeufe.

    Nur in 16-Bit-Zugriffen (der MVBC vertraegt keine breiteren), nur
    bei gestopptem Controller, nur wenn niemand /dev/mvblli0 offen hat,
    und nie im Registerblock SA+0x380..0x3FF."""
    busy = holders(DEV)
    if busy:
        log.line("ABBRUCH: %s ist offen (PID %s) - nicht geloescht"
                 % (DEV, ", ".join(map(str, busy))))
        return False

    fb = os.open(BOARD, os.O_RDWR)
    try:
        mw = mmap.mmap(fb, BAR_SIZE, mmap.MAP_SHARED,
                       mmap.PROT_READ | mmap.PROT_WRITE)
    except OSError as e:
        # Ein Board-Treiber, dessen mmap nicht beschreibbar ist: dann
        # eben ohne Loeschen weiter, aber sichtbar im Protokoll.
        log.line("Traffic Memory NICHT geloescht: mmap schreibend "
                 "abgelehnt (%s)" % errname(e.errno))
        return True
    finally:
        os.close(fb)

    scrs = scr_all(mw)
    live = live_sa(mw)
    if any(scrs[sa] & 3 == 3 for sa in live):
        mw.close()
        log.line("ABBRUCH: Controller laeuft (%s) - nicht geloescht"
                 % fmt_scr(scrs, live))
        return False

    skip = set()
    for sa in SA_CANDIDATES:
        skip.update(range((sa + REG) // 2, (sa + 0x400) // 2))
    view = memoryview(mw)[TM:TM + 0x40000].cast("H")
    for i in range(0x20000):
        if i not in skip:
            view[i] = 0
    rest = [(i * 2, view[i]) for i in range(0x20000)
            if i not in skip and view[i]]
    view.release()
    mw.close()

    log.line("Traffic Memory geloescht: %d Worte, danach ungleich 0: %d"
             % (0x20000 - len(skip), len(rest)))
    # Was nach dem Loeschen sofort wieder beschrieben ist, schreibt der
    # Controller selbst. Vorzustand: das hat der zuvor geschlossene
    # Treiber hinterlassen, nicht der, der jetzt getestet wird.
    log.line("     Vorzustand %s" % fmt_scr(scrs, live))
    for off, v in rest[:16]:
        log.line("     Vorzustand TM+0x%05X  %-40s %04X"
                 % (off, describe(off, ({}, {})), v))
    return True


def scr_all(m):
    """SCR an jedem moeglichen Platz der Service Area."""
    return {sa: struct.unpack_from("<H", m, TM + sa + REG)[0]
            for sa in SA_CANDIDATES}


def live_sa(m):
    """Plaetze, an denen wirklich der Registerblock steht. An den anderen
    liegt gewoehnlicher Speicher mit beliebigem Inhalt. Erkennungszeichen:
    MCR traegt die Version MVBC02D (5), und sein mcm-Feld verweist auf
    genau diesen Platz."""
    out = []
    for sa in SA_CANDIDATES:
        mcr = struct.unpack_from("<H", m, TM + sa + REG + 4)[0]
        mcm = mcr & 7
        if mcr >> 11 == 5 and mcm < len(SA_FOR_MCM) and SA_FOR_MCM[mcm] == sa:
            out.append(sa)
    return out


def fmt_scr(scrs, live=()):
    return "  ".join("SCR@%04X %04X%s" % (sa, v, "*" if sa in live else "")
                     for sa, v in sorted(scrs.items())) + \
        "   (* = Registerblock)"


def irq_count():
    """Summe ueber alle CPUs der pixy-mvb-Zeile in /proc/interrupts.
    Zaehlt auch unter dem Original-Board-Treiber, der keine dbg_*-
    Zaehler hat."""
    try:
        for ln in open("/proc/interrupts"):
            if ln.rstrip().endswith("pixy-mvb"):
                f = ln.split()
                return sum(int(x) for x in f[1:] if x.isdigit())
    except OSError:
        pass
    return None


def rcve_state(mm):
    """QDT-Eintrag der Empfangsqueue (Hardwareposition) und das LLR mit
    Datenzeiger 0 (Waechter = Softwareposition)."""
    qdt = struct.unpack_from("<H", mm, TM + SA_OFF + 0x314)[0]
    name, base, n = RINGS[2]
    guard = None
    for k in range(n):
        if struct.unpack_from("<H", mm, TM + base + k * 4)[0] == 0:
            guard = k
            break
    return qdt, guard


def ping(fd, mm, log):
    """Anfrage an Geraet 6 senden und auf die Antwort warten.

    Die Antwort loest DTI1 aus. Mit irq > 0 holt der Treiber sie nur im
    Interrupt ab - kommt kein Interrupt an, steht sie zwar im Ring des
    Controllers (QDT rueckt vor), aber read() findet nichts."""
    log.line("\n--- Verbindungsaufbau an Geraet %d (aus dem Produktivabzug) ---"
             % PING_DEST)
    q0, g0 = rcve_state(mm)
    i0 = irq_count()
    tq_before = struct.unpack_from("<H", mm, TM + SA_OFF + 0x312)[0]
    req = mvb_port(3, PING_DEST)
    req[4:36] = PING_FRAME
    st, _ = call(log, "write() MD Anfrage", lambda: pd_write(fd, req, 32))
    tq_after_write = struct.unpack_from("<H", mm, TM + SA_OFF + 0x312)[0]

    reply = None
    t0 = time.time()
    while time.time() - t0 < 5.0 and reply is None:
        rd = mvb_port(3, PING_DEST)
        try:
            pd_read(fd, rd, 32)
            reply = bytes(rd[4:36])
        except OSError:
            time.sleep(0.05)
    dt = time.time() - t0
    q1, g1 = rcve_state(mm)
    i1 = irq_count()
    tq_end = struct.unpack_from("<H", mm, TM + SA_OFF + 0x312)[0]

    # Der Sendeeintrag xmit_q1 steht nach dem Einhaengen auf dem Frame und
    # rueckt weiter, sobald der Controller ihn gesendet hat.
    log.line("     Sendequeue xmit_q1: QDT %04X, nach write %04X, nach 5 s %04X"
             " - %s" % (tq_before, tq_after_write, tq_end,
                        "gesendet" if tq_end != tq_after_write
                        else "NICHT gesendet"))
    log.line("     Empfangsqueue: QDT %04X -> %04X, Waechter LLR %s -> %s"
             % (q0, q1, g0, g1))
    log.line("     Interrupt-Stichprobe Anfrage: pixy-mvb %s"
             % ("+%d" % (i1 - i0) if i0 is not None and i1 is not None
                else "nicht lesbar"))
    if reply is None:
        log.line("     Antwort: keine abgeholt in 5 s%s"
                 % (" - aber im Ring des Controllers angekommen"
                    if q1 != q0 else ""))
    else:
        log.line("     Antwort nach %.2f s: %s" % (dt, reply.hex(" ")))


def sample_dr(mm, log, label):
    """DR mehrfach lesen und die Haeufigkeit je Wert festhalten."""
    seen = {}
    for _ in range(DR_SAMPLES):
        v = struct.unpack_from("<H", mm, TM + SA_OFF + DR_OFF)[0]
        seen[v] = seen.get(v, 0) + 1
    bit9 = sum(n for v, n in seen.items() if v & DR_VOLATILE)
    log.line("     DR-Stichprobe %s (%d): %s   Bit 9 in %d"
             % (label, DR_SAMPLES,
                ", ".join("%04X x%d" % (v, n) for v, n in sorted(seen.items())),
                bit9))


def run(outdir, allow_md, go):
    os.makedirs(outdir, exist_ok=True)
    log = Log(os.path.join(outdir, "log.txt"))

    log.line("Treiber: " + ", ".join(modules()))
    params = []
    for f in sorted(glob.glob("/sys/module/pixy_mvblli/parameters/*")):
        name = os.path.basename(f)
        if name.startswith("dbg_"):
            continue
        try:
            params.append("%s=%s" % (name, open(f).read().strip()))
        except OSError:
            params.append("%s=?" % name)
    log.line("Parameter LLI: %s" % (" ".join(params) or "keine lesbar"))
    log.line("")

    if WIPE and not wipe_tm(log):
        log.close()
        sys.exit(3)

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
    sample_dr(mm, log, "nach open")

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

    # Die Adresse geht als Wert, nicht als Zeiger - so steht es im
    # Maschinencode des Originals. Ein Zeiger ist eine grosse Zahl und
    # ergibt EINVAL; das wird gleich mitgeprueft.
    call(log, "WRITE_DEV_ADDR Zeiger",
         lambda: io(IOC["WRITE_DEV_ADDR"],
                    bytearray(struct.pack("<H", TEST_ADDR)), True))
    call(log, "WRITE_DEV_ADDR",
         lambda: io(IOC["WRITE_DEV_ADDR"], TEST_ADDR))
    snapshot(mm, outdir, log.step, "devaddr")

    b2 = bytearray(2)
    call(log, "READ_DEV_ADDR", lambda: io(IOC["READ_DEV_ADDR"], b2, True))
    log.line("     Adresse nachher: 0x%04X" % struct.unpack("<H", b2)[0])

    # pb_mwd, ownership, ts_type, prt_addr_max, prt_indx_max, auto_reset_rld
    # ts_type muss 1 sein, sonst EIO (lp_ts_create). Die Grenzen nimmt
    # das Original nicht vom Aufrufer, sondern aus der Groesse des
    # Traffic Memory. Ohne erfolgreiches HWINIT stuerzt das Original
    # bei PD_CONF ab ("Killed") - das war der Absturz der ersten Runde.
    hw0 = bytearray(struct.pack("<QBBHHB x", 0, 0, 0, 0xFFF, 0xFFF, 0))
    call(log, "HWINIT ts_type 0", lambda: io(IOC["HWINIT"], hw0, True))
    hw = bytearray(struct.pack("<QBBHHB x", 0, 1, 1, 0xFFF, 0xFFF, 0))
    call(log, "HWINIT", lambda: io(IOC["HWINIT"], hw, True))
    snapshot(mm, outdir, log.step, "hwinit")

    call(log, "MD_CONF",
         lambda: io(IOC["MD_CONF"], bytearray(struct.pack("<H", 0)), True))
    snapshot(mm, outdir, log.step, "mdconf")

    cfg = struct.pack("<H", len(PORTS))
    for a, s, t in PORTS:
        cfg += struct.pack("<HHH", a, s, t)
    if NO_PD:
        log.line("   (PD_CONF uebersprungen, --no-pd)")
    else:
        call(log, "PD_CONF",
             lambda: io(IOC["PD_CONF"], bytearray(cfg), True))
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
    # Wert, nicht Zeiger: oberes Wort Maske, unteres Wort Wert
    call(log, "WRITE_DSW",
         lambda: io(IOC["WRITE_DSW"], 0x00FF0011))
    snapshot(mm, outdir, log.step, "writedsw")
    # Und wieder zurueck: das DSW sieht der Busmaster. Mit 0x0011 im
    # unteren Byte meldet sich das Geraet womoeglich so, dass der Master
    # keine Messages mit ihm austauscht - --ping sendete danach nicht.
    dsw_orig = struct.unpack("<H", d)[0]
    call(log, "WRITE_DSW zurueck",
         lambda: io(IOC["WRITE_DSW"], 0xFFFF0000 | dsw_orig))

    # Kommandobyte 0x03 = cla|clb: setzt nur die Fehlerzaehler zurueck.
    # Die Leitungsbits 0x0C bleiben aus - die wuerden die Leitungswahl
    # umschalten, und die Karte haengt am Bus.
    call(log, "WRITE_CONTROL",
         lambda: io(IOC["WRITE_CONTROL"],
                    bytearray(struct.pack("<HHBx", TEST_ADDR, 43, 0x03))))
    snapshot(mm, outdir, log.step, "writectrl")

    # Prozessdaten schreiben: zweimal, damit die Seitenumschaltung sichtbar wird
    for n, pat in (() if NO_PD else ((1, 0xA5), (2, 0x5A))):
        port = mvb_port(1, WRITE_PORT)
        for i in range(WRITE_SIZE // 2):
            struct.pack_into("<H", port, 4 + i * 2, pat << 8 | pat)
        call(log, "write() PD #%d" % n,
             lambda p=port: pd_write(fd, p, WRITE_SIZE))
        snapshot(mm, outdir, log.step, "writepd%d" % n)

    if not NO_PD:
        rd = mvb_port(1, WRITE_PORT)
        call(log, "read() PD", lambda: pd_read(fd, rd, WRITE_SIZE))

    if allow_md:
        # Ohne START bleibt alles in den Sendequeues liegen und laesst
        # sich byteweise vergleichen: Link_Header, Waechter, QDT, die
        # Ereignisframe-Ports EF0/EF1 und MR. Danach Verwerfen und
        # erneutes Senden - das traegt die QDT ab der aktuellen Position
        # neu ein.
        def md_frame(typ, fill):
            md = mvb_port(typ, 6)
            for i in range(2, 16):
                struct.pack_into("<H", md, 4 + i * 2, fill + i)
            return md
        call(log, "write() MD niedrig",
             lambda: pd_write(fd, md_frame(3, 0x1200), 32))
        call(log, "write() MD niedrig 2",
             lambda: pd_write(fd, md_frame(3, 0x1300), 32))
        call(log, "write() MD hoch",
             lambda: pd_write(fd, md_frame(2, 0x2200), 32))
        snapshot(mm, outdir, log.step, "writemd")
        call(log, "write() MD Port 256",
             lambda: pd_write(fd, mvb_port(3, 256), 32))
        g3 = bytearray(4)
        call(log, "MD_GET_STATUS (MD)",
             lambda: io(IOC["MD_GET_STATUS"], g3, True))
        log.line("     status 0x%04X" % struct.unpack_from("<H", g3)[0])
        call(log, "MD_FLUSH_QUEUE", lambda: io(IOC["MD_FLUSH_QUEUE"]))
        snapshot(mm, outdir, log.step, "mdflush")
        call(log, "write() MD nach Flush",
             lambda: pd_write(fd, md_frame(3, 0x1400), 32))
        snapshot(mm, outdir, log.step, "mdnachflush")
        rd = mvb_port(3, 6)
        call(log, "read() MD", lambda: pd_read(fd, rd, 32))
    else:
        log.line("   (Message-Daten uebersprungen, --md erlaubt sie)")

    if not NO_PD:
        # Ereignisaufzeichnung: setzt im PCS das DTI-Feld auf 7 und gibt
        # beim ersten Port DTI7 frei. Vor START wieder abgemeldet.
        sink = [q[0] for q in PORTS if q[2] == 1][0]
        ev = lambda port, n: bytearray(struct.pack("<HH", port, n))
        call(log, "REC_CONF Senke",
             lambda: io(IOC["REC_CONF"], ev(sink, 20), True))
        snapshot(mm, outdir, log.step, "recconf")
        call(log, "REC_CONF doppelt",
             lambda: io(IOC["REC_CONF"], ev(sink, 20), True))
        call(log, "REC_CONF Quelle",
             lambda: io(IOC["REC_CONF"], ev(WRITE_PORT, 20), True))
        call(log, "REC_CONF 21 Eintr.",
             lambda: io(IOC["REC_CONF"], ev(sink, 21), True))
        call(log, "REC_DEL",
             lambda: io(IOC["REC_DEL"], ev(sink, 0), True))
        snapshot(mm, outdir, log.step, "recdel")
        call(log, "REC_DEL nochmal",
             lambda: io(IOC["REC_DEL"], ev(sink, 0), True))

        # DISABLE_PORT, ebenfalls als Wert: auf einer Senke EIO, auf der
        # Quelle werden nur die Typbits im PCS geloescht.
        call(log, "DISABLE_PORT Senke",
             lambda: io(IOC["DISABLE_PORT"], DISABLE_PORT_ADDR))
        call(log, "DISABLE_PORT Quelle",
             lambda: io(IOC["DISABLE_PORT"], WRITE_PORT))
        snapshot(mm, outdir, log.step, "disable")

    call(log, "MD_FLUSH_QUEUE arg",
         lambda: io(IOC["MD_FLUSH_QUEUE"], 1))

    # ---- Betrieb: nur mit --go, hier laeuft der Controller wirklich ----
    if go:
        log.line("\n--- Betrieb (MVB_GO) ---")
        irq0 = irq_count()
        ok, _ = call(log, "START", lambda: io(IOC["START"]))
        # Interruptkette vom MVBC bis zur CPU (irqstate.py im selben
        # Verzeichnis); fehlt es, geht es ohne
        try:
            import irqstate
            for ln in irqstate.capture()[1:]:
                log.line("     Stichprobe Zustand: " + ln)
        except Exception as e:
            log.line("     Stichprobe Zustand: nicht erfasst (%s)" % e)
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

        seen = {}          # Port -> zuletzt gelesene Daten
        changed = set()
        c0 = counters()
        r0 = [struct.unpack_from("<H", mm, TM + SA_OFF + o)[0]
              for o in (0x390, 0x394, 0x3B8, 0x3BC, 0x3B0, 0x3B4,
                        0x3C0, 0x3C4)]
        log.line("     FC %04X  EC %04X  IMR0 %04X IMR1 %04X  "
                 "IPR0 %04X IPR1 %04X  ISR0 %04X ISR1 %04X" % tuple(r0))

        for round_ in range(3):
            time.sleep(20)
            r = [struct.unpack_from("<H", mm, TM + SA_OFF + o)[0]
                 for o in (0x390, 0x394, 0x3C0, 0x3C4, 0x3B0, 0x3B4)]
            log.line("     +%2ds  FC %04X EC %04X  ISR0 %04X ISR1 %04X  "
                     "IPR0 %04X IPR1 %04X"
                     % ((round_ + 1) * 20, r[0], r[1], r[2], r[3], r[4], r[5]))

            for a_, sz, t in PORTS:
                if NO_PD or t != 1:
                    continue
                buf = mvb_port(1, a_)
                try:
                    pd_read(fd, buf, sz)
                    raw = bytes(buf[4:4 + sz])
                    data = raw[:8].hex()
                    fresh = struct.unpack_from("<H", buf, 36)[0]

                    mark = ""
                    if fresh < 0xFFFF:
                        mark = "   <-- EMPFANGEN"
                    if a_ in seen and seen[a_] != raw:
                        changed.add(a_)
                        mark += "  WERT WECHSELT"
                    seen[a_] = raw

                    log.line("       Port %4d %-18s %-16s  fresh %5d%s"
                             % (a_, LIFESIGN.get(a_, ""), data, fresh, mark))
                except OSError as e:
                    log.line("       Port %4d  %s" % (a_, errname(e.errno)))

        log.line("     Lebenszeichen:")
        for a_, nm in sorted(LIFESIGN.items()):
            if a_ not in [q[0] for q in PORTS]:
                continue
            if a_ in changed:
                log.line("       Port %4d %-18s wechselt - EMPFANG BELEGT"
                         % (a_, nm))
            elif a_ in seen:
                log.line("       Port %4d %-18s unveraendert" % (a_, nm))
            else:
                log.line("       Port %4d %-18s nicht gelesen" % (a_, nm))

        c1 = counters()
        irq1 = irq_count()
        if irq0 is None or irq1 is None:
            log.line("     Interrupt-Stichprobe: pixy-mvb nicht in "
                     "/proc/interrupts")
        else:
            log.line("     Interrupt-Stichprobe: pixy-mvb +%d in 60 s "
                     "(%d -> %d)" % (irq1 - irq0, irq0, irq1))
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

        if PING:
            ping(fd, mm, log)

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

    sample_dr(mm, log, "vor close")
    snapshot(mm, outdir, 99, "ende")
    os.close(fd)
    log.line("\ngeschlossen")

    # Zustand nach release(): was mvb_deinit_board hinterlaesst, findet
    # der naechste Treiber vor.
    time.sleep(0.2)
    log.line("     nach close %s" % fmt_scr(scr_all(mm), live_sa(mm)))
    snapshot(mm, outdir, 99, "zu")
    # Nach einem RESET steht der Registerblock am Grundplatz
    snapshot(mm, outdir, 99, "zu_grundplatz", sa_off=SA_CANDIDATES[0],
             tm=False)
    mm.close()
    log.close()


# ------------------------------------------------------------ Vergleich
MAX_LINES = 12          # je Bereich und Schritt


def region(off):
    for a, b, n in REGIONS:
        if a <= off < b:
            return n
    return "?"


def bin_offset(i):
    """Index in der Abzugsdatei -> Offset im Traffic Memory."""
    first = CHUNKS[0][1]
    return i if i < first else CHUNKS[1][0] + (i - first)


def words(data):
    """Abzug -> {TM-Offset: Wort}"""
    n = len(data) // 2
    vals = struct.unpack("<%dH" % n, data[:n * 2])
    return {bin_offset(i * 2): v for i, v in enumerate(vals)}


def dock_index(rel):
    """Byte im Datenbereich -> (Dock-Index, Seite, Wort). 64 Byte je vier
    Docks, darin zwei Seiten zu 32 Byte, darin vier Docks zu 8 Byte."""
    return (rel // 64) * 4 + (rel % 32) // 8, (rel % 64) // 32, (rel % 8) // 2


def port_of(rev, idx):
    """Dock-Index -> Portname aus der PIT beider Seiten."""
    a, b = rev[0].get(idx), rev[1].get(idx)
    if a == b:
        return " (Port %d)" % a if a is not None else ""
    return " (Port %s|%s)" % (a if a is not None else "-",
                              b if b is not None else "-")


def reverse_pit(w):
    rev = {}
    for port in range(0x1000):
        idx = w.get(port * 2, 0)
        if idx:
            rev.setdefault(idx, port)
    return rev


def ring_desc(off):
    for name, base, n in RINGS:
        llr_end = base + n * 4
        data = (llr_end + 31) & ~31
        if base <= off < llr_end:
            k = (off - base) // 4
            field = "Daten" if (off - base) % 4 < 2 else "Folge"
            return "%s LLR %d %szeiger" % (name, k, field)
        if data <= off < data + n * 32:
            k = (off - data) // 32
            return "%s Puffer %d Byte %d" % (name, k, (off - data) % 32)
    return "frei"


def sa_desc(rel):
    if rel < 0x200:
        idx, page, w = dock_index(rel)
        return "phys. Dock %s Seite %d Wort %d" % (
            PP_NAMES.get(idx, str(idx)), page, w)
    if rel < 0x300:
        i = (rel - 0x200) // 8
        return "phys. PCS %s Wort %d" % (PP_NAMES.get(i, str(i)),
                                         (rel % 8) // 2)
    if rel < 0x310:
        return "MFS +%d" % (rel - 0x300)
    q = {0x310: "QDT xmit_q0", 0x312: "QDT xmit_q1", 0x314: "QDT rcve_q"}
    return q.get(rel, "SA+0x%03X" % rel)


def describe(off, rev):
    if off < 0x2000:
        return "PIT Port %d" % (off // 2)
    if off < 0x4000:
        return "da_pit Port %d" % ((off - 0x2000) // 2)
    if off < 0x8000:
        return "da_pcs %d Wort %d" % ((off - 0x4000) // 8, (off % 8) // 2)
    if off < 0xFC00:
        return ring_desc(off)
    if off < 0x10000:
        return sa_desc(off - 0xFC00)
    if off < 0x20000:
        idx, page, w = dock_index(off - 0x10000)
        return "Dock %d%s Seite %d Wort %d" % (idx, port_of(rev, idx),
                                               page, w)
    if off < 0x30000:
        idx, page, w = dock_index(off - 0x20000)
        return "Force %d%s Seite %d Wort %d" % (idx, port_of(rev, idx),
                                                page, w)
    if off < 0x38000:
        i = (off - 0x30000) // 8
        return "PCS %d%s Wort %d" % (i, port_of(rev, i), (off % 8) // 2)
    idx, page, w = dock_index(off - 0x38000)
    return "da_data %d Seite %d Wort %d" % (idx, page, w)


ISA_KEY = 0x10000         # ISA-Register im Registervergleich
ISA_NAMES = {0x320: "BASR0", 0x322: "BASR1", 0x324: "BCR"}


def read_regs(path):
    out = {}
    for ln in open(path).read().splitlines():
        f = ln.split()
        if len(f) < 2 or f[1] == "----":
            continue
        if f[0].startswith("ISA+0x"):
            out[ISA_KEY | int(f[0][6:], 16)] = int(f[1], 16)
        else:
            out[int(f[0][5:], 16)] = int(f[1], 16)
    return out


def reg_label(o):
    if o & ISA_KEY:
        return "ISA+0x%03X %-5s" % (o & 0xFFF, ISA_NAMES.get(o & 0xFFF, ""))
    return "SA+0x%03X %-5s" % (o, REGNAMES.get(o, ""))


def volatile_line(s):
    return any(k in s for k in VOLATILE_LOG)


def compare(da, db):
    rc = 0
    la = open(os.path.join(da, "log.txt")).read().splitlines()
    lb = open(os.path.join(db, "log.txt")).read().splitlines()

    def pre(L):
        return [x.strip() for x in L if any(k in x for k in PRESTATE_LOG)]

    def core(L):
        return [x for x in L
                if not x.startswith("Treiber:") and not volatile_line(x)
                and not any(k in x for k in PRESTATE_LOG)]

    print("=== Protokoll ===")
    diff = 0
    ca, cb = core(la), core(lb)
    sm = difflib.SequenceMatcher(a=ca, b=cb, autojunk=False)
    for op, i1, i2, j1, j2 in sm.get_opcodes():
        if op == "equal":
            continue
        for k in range(max(i2 - i1, j2 - j1)):
            a = ca[i1 + k] if i1 + k < i2 else "(fehlt)"
            b = cb[j1 + k] if j1 + k < j2 else "(fehlt)"
            tag = ""
            if any(x in a for x in KNOWN_DIFF):
                tag = "   (bekannter Unterschied)"
            else:
                diff += 1
            print("  A: %s\n  B: %s%s" % (a, b, tag))
    print("  %d Abweichungen" % diff)
    rc += diff

    print("\n=== Vorzustand (vom zuvor geschlossenen Treiber, nicht verglichen) ===")
    for tag, L in (("A", la), ("B", lb)):
        for x in pre(L):
            print("  %s: %s" % (tag, x))

    print("\n=== Stichproben (nicht verglichen) ===")
    for tag, L in (("A", la), ("B", lb)):
        for x in L:
            if "Stichprobe" in x:
                print("  %s: %s" % (tag, x.strip()))

    print("\n=== Speicherabzuege ===")
    fa = sorted(os.path.basename(p) for p in glob.glob(os.path.join(da, "*.bin")))
    fb = sorted(os.path.basename(p) for p in glob.glob(os.path.join(db, "*.bin")))
    if fa != fb:
        print("  unterschiedliche Schrittfolge:")
        print("   nur in A: %s" % sorted(set(fa) - set(fb)))
        print("   nur in B: %s" % sorted(set(fb) - set(fa)))
        return rc + 1

    prev = None
    for name in fa:
        wa = words(open(os.path.join(da, name), "rb").read())
        wb = words(open(os.path.join(db, name), "rb").read())
        d = sorted((o, wa[o], wb.get(o)) for o in wa if wa[o] != wb.get(o))
        if not d:
            print("  %-24s gleich" % name)
            prev = None
            continue
        rc += 1
        if d == prev:
            print("  %-24s UNTERSCHIED, wie vorher (%d Worte)" % (name, len(d)))
            continue
        prev = d
        rev = (reverse_pit(wa), reverse_pit(wb))
        print("  %-24s UNTERSCHIED, %d Worte" % (name, len(d)))
        by_reg = {}
        for o, x, y in d:
            by_reg.setdefault(region(o), []).append((o, x, y))
        for r in sorted(by_reg, key=lambda k: by_reg[k][0][0]):
            items = by_reg[r]
            print("    %s: %d Worte" % (r, len(items)))
            for o, x, y in items[:MAX_LINES]:
                print("      TM+0x%05X  %-44s A %04X  B %04X"
                      % (o, describe(o, rev), x, y))
            if len(items) > MAX_LINES:
                print("      ... und %d weitere" % (len(items) - MAX_LINES))

    print("\n=== Register (ohne %s, DR ohne Bit 9) ==="
          % " ".join(REGNAMES[o] for o in sorted(VOLATILE_REGS)))
    prev = None
    for name in sorted(os.path.basename(p)
                       for p in glob.glob(os.path.join(da, "*.regs"))):
        ra = read_regs(os.path.join(da, name))
        rb = read_regs(os.path.join(db, name))
        d = []
        for o in sorted(ra):
            if o in VOLATILE_REGS:
                continue
            x, y = ra[o], rb.get(o)
            if o == DR_OFF and y is not None:
                x, y = x & ~DR_VOLATILE, y & ~DR_VOLATILE
            if x != y:
                d.append((o, ra[o], rb.get(o)))
        if not d:
            print("  %-24s gleich" % name)
            prev = None
            continue
        rc += 1
        if d == prev:
            print("  %-24s UNTERSCHIED, wie vorher" % name)
            continue
        prev = d
        print("  %-24s UNTERSCHIED" % name)
        for o, x, y in d:
            print("      %s A %04X  B %s"
                  % (reg_label(o), x, "%04X" % y if y is not None else "----"))

    print("\n%s" % ("ALLES GLEICH" if rc == 0 else "%d Stellen weichen ab" % rc))
    return rc


def main():
    if len(sys.argv) >= 3 and sys.argv[1] == "run":
        global STOP_AFTER, NO_PD, WIPE, PING, PORTS, WRITE_PORT, WRITE_SIZE
        global DISABLE_PORT_ADDR
        NO_PD = "--no-pd" in sys.argv
        WIPE = "--no-wipe" not in sys.argv
        PING = "--ping" in sys.argv
        for i, a_ in enumerate(sys.argv):
            if a_ == "--stop-after":
                STOP_AFTER = int(sys.argv[i + 1])
            if a_ == "--ports":
                lst = []
                for ln in open(sys.argv[i + 1]):
                    ln = ln.split("#")[0].split()
                    if len(ln) == 3:
                        lst.append(tuple(int(x) for x in ln))
                if not lst:
                    print("--ports: keine brauchbare Zeile gefunden")
                    sys.exit(2)
                PORTS = lst
                src = [q for q in PORTS if q[2] == 2]
                if src:
                    WRITE_PORT, WRITE_SIZE = src[0][0], src[0][1]
                DISABLE_PORT_ADDR = [q for q in PORTS if q[2] == 1][-1][0]
                print("Portliste aus %s: %d Ports" % (sys.argv[i + 1], len(PORTS)))
        run(sys.argv[2], "--md" in sys.argv, "--go" in sys.argv)
    elif len(sys.argv) == 4 and sys.argv[1] == "compare":
        sys.exit(1 if compare(sys.argv[2], sys.argv[3]) else 0)
    else:
        print(__doc__)
        sys.exit(2)


main()
