#!/usr/bin/env python3
"""Geraetestatuswort (DSW) direkt aus dem Traffic Memory lesen - ohne
/dev/mvblli0 zu oeffnen.

READ_DSW braucht /dev/mvblli0, und das haelt die Herstelleranwendung
exklusiv offen (ein zweites open() liefert EBUSY). Das DSW steht aber als
Datum des physischen Ports FC15 im Traffic Memory, und das ist ueber
/dev/mvb0 lesbar - nicht exklusiv, also im laufenden Betrieb, auf dem
Herstellerimage wie auf dem eigenen. Es ist genau der Wert, den der
Busmaster mit seiner Statusabfrage (F-Code 15) bekommt.

Vorweg gibt es die Einrichtung der physischen Ports aus, ueber die
Message-Daten laufen: Message-Quelle und -Senke (F-Code 12), die
Ereignisports (F-Code 9), Mastership und Device Status, dazu die QDT der
Message-Queues und den Master Frame Slot. Auf dem Herstellergeraet und
auf dem eigenen ausgefuehrt, zeigt der Vergleich, ob der Controller fuer
Messages gleich eingerichtet ist. Die Spalte "Nachbau" nennt das Wort 0,
das pixy-mvblli schreibt.

Gelesen werden nur die Ports und PCS der Service Area sowie SCR, MCR und
DR aus der ersten Cachezeile des Registerblocks. Die zweite Cachezeile
(SA+0x3C0..0x3FF mit IVR0/IVR1) wird nicht angefasst - sie wuerde der
laufenden Anwendung Interrupts stehlen.

    dswread.py [Anzahl [Abstand in Sekunden]]     Vorgabe: 5 Lesungen, 1 s
"""

import mmap
import os
import struct
import sys
import time

BAR_SIZE = 0x4000000
ISA = 0x2000000
TM = ISA + 0x40000

# Lage der Service Area je mcm; erkannt wird sie am MCR (Version 5 und
# ein mcm, das auf genau diesen Platz verweist)
SA_CANDIDATES = (0x03C00, 0x07C00, 0x0FC00)
SA_FOR_MCM = (0x03C00, 0x07C00, 0x0FC00, 0x0FC00, 0x0FC00)

SCR = 0x380
MCR = 0x384
DR = 0x388

# Physischer Port FC15 (Index 7): Daten in SA+0x000.., PCS in SA+0x200..
PP_FC15 = 7
PCS = 0x200
PCS1 = PCS + PP_FC15 * 8 + 2
PCS_VP = 0x0040
PCS_COUNT = 32
MFS = 0x300
QDT = 0x310

# Physische Ports: Index, Name, Wort 0 im PCS, wie es pixy-mvblli
# schreibt (MSNK mit irq > 0; mit irq = 0 ist es C404). None: setzt der
# Nachbau nicht.
PP_NAMES = (
    (0x0, "FC8   Mastership", 0x8802),
    (0x1, "EFS   Event Ident", 0x9402),
    (0x4, "EF0   Ereignis 0", 0x9802),
    (0x5, "EF1   Ereignis 1", 0x9802),
    (0x6, "MOS", None),
    (0x7, "FC15  Device Status", 0xF842),
    (0x8, "MSRC  Message-Quelle", 0xC81C),
    (0xC, "MSNK  Message-Senke", 0xC424),
)
PCS_TYPES = ("-", "Senke", "Quelle", "?")


def dock(idx, page):
    """Wie tm_dock_offset() im Treiber: vier Docks je 64-Byte-Block, zwei
    Seiten zu 32 Byte."""
    return (idx >> 2) * 64 + page * 32 + (idx & 3) * 8


# IEC 61375-1, Device Status Word
DSW_BITS = (
    (0x8000, "SP", "Sondergeraet"),
    (0x4000, "BA", "Busadministrator"),
    (0x2000, "GW", "Gateway"),
    (0x1000, "MD", "Message-Daten"),
    (0x0080, "LAT", "Leitung A aktiv"),
    (0x0040, "RLD", "redundante Leitung gestoert"),
    (0x0020, "SSD", "Systemstoerung"),
    (0x0010, "SDD", "Geraetestoerung"),
    (0x0008, "ERD", "verlaengerte Antwortzeit"),
    (0x0004, "FRC", "erzwungener Zustand"),
    (0x0002, "DNR", "Geraet nicht bereit"),
    (0x0001, "SER", "reserviert"),
)

IL_NAMES = ("RESET", "CONFIG", "TEST", "RUNNING")


def live_sa(mm):
    for sa in SA_CANDIDATES:
        mcr = struct.unpack_from("<H", mm, TM + sa + MCR)[0]
        mcm = mcr & 7
        if mcr >> 11 == 5 and mcm < len(SA_FOR_MCM) and SA_FOR_MCM[mcm] == sa:
            return sa
    return None


def decode(dsw):
    names = [n for bit, n, _ in DSW_BITS if dsw & bit]
    cls = (dsw >> 8) & 0xF
    s = " ".join(names) if names else "-"
    if cls:
        s += "  Klassenbits %X" % cls
    return s


def pcs_decode(w0):
    """PCS Wort 0: F-Code, Typ, DTI und die Bits QA, NUM, FE."""
    flags = [n for bit, n in ((0x4, "QA"), (0x2, "NUM"), (0x1, "FE"))
             if w0 & bit]
    rest = w0 & 0x0318
    s = "F%-2d %-6s DTI %d %s" % (w0 >> 12, PCS_TYPES[(w0 >> 10) & 3],
                                  (w0 >> 5) & 7, " ".join(flags))
    if rest:
        s += " +%04X" % rest
    return s


def show_ports(rd):
    names = dict((i, (n, ref)) for i, n, ref in PP_NAMES)
    print("Physische Ports (PCS je 4 Worte, Wort 0 entschluesselt):")
    print("   Idx  %-21s %-19s  %-32s %s"
          % ("Port", "PCS", "Wort 0", "Nachbau"))
    for i in range(PCS_COUNT):
        w = [rd(PCS + i * 8 + k * 2) for k in range(4)]
        name, ref = names.get(i, ("", None))
        if not name and not any(w):
            continue
        mark = ""
        if ref is not None:
            mark = "%04X%s" % (ref, "" if w[0] == ref else "  <-- anders")
        print("   %3X  %-21s %s  %-32s %s"
              % (i, name, " ".join("%04X" % x for x in w),
                 pcs_decode(w[0]), mark))
    q = [rd(QDT + k * 2) for k in range(3)]
    print("   QDT  xmit0 %04X  xmit1 %04X  rcve %04X     MFS %04X\n"
          % (q[0], q[1], q[2], rd(MFS)))


def line_mode(dr):
    if not dr & 0x0001:
        return "Zweileitungsbetrieb"
    return "Einleitungsbetrieb Leitung %s" % ("A" if dr & 0x0008 else "B")


def main():
    count = int(sys.argv[1]) if len(sys.argv) > 1 else 5
    pause = float(sys.argv[2]) if len(sys.argv) > 2 else 1.0

    fd = os.open("/dev/mvb0", os.O_RDWR)
    try:
        try:
            mm = mmap.mmap(fd, BAR_SIZE, mmap.MAP_SHARED, mmap.PROT_READ)
        except OSError:
            mm = mmap.mmap(fd, BAR_SIZE, mmap.MAP_SHARED,
                           mmap.PROT_READ | mmap.PROT_WRITE)
    finally:
        os.close(fd)

    sa = live_sa(mm)
    if sa is None:
        print("Kein Registerblock gefunden. Das TM-Fenster ist erst nach dem "
              "ersten open() von /dev/mvblli0 eingerichtet - laeuft die "
              "Anwendung?")
        return 1

    rd = lambda off: struct.unpack_from("<H", mm, TM + sa + off)[0]
    scr, mcr, dr = rd(SCR), rd(MCR), rd(DR)
    print("Service Area TM+0x%05X  SCR %04X (%s)  MCR %04X (mcm %d)  "
          "DR %04X (%s)" % (sa, scr, IL_NAMES[scr & 3], mcr, mcr & 7,
                            dr, line_mode(dr)))
    show_ports(rd)
    print("DSW = Port FC15, Seite laut VP im PCS. Bits nach IEC 61375-1.\n")

    for i in range(count):
        vp = 1 if rd(PCS1) & PCS_VP else 0
        p0 = rd(dock(PP_FC15, 0))
        p1 = rd(dock(PP_FC15, 1))
        dsw = p1 if vp else p0
        print("%s  DSW %04X  %-40s (Seite %d; Seiten %04X/%04X)"
              % (time.strftime("%H:%M:%S"), dsw, decode(dsw), vp, p0, p1))
        if i + 1 < count:
            time.sleep(pause)

    print("\nLegende:")
    for bit, n, text in DSW_BITS:
        print("   %04X  %-3s  %s" % (bit, n, text))
    mm.close()
    return 0


sys.exit(main())
