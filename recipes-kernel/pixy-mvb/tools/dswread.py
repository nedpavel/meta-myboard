#!/usr/bin/env python3
"""Geraetestatuswort (DSW) direkt aus dem Traffic Memory lesen - ohne
/dev/mvblli0 zu oeffnen.

READ_DSW braucht /dev/mvblli0, und das haelt die Herstelleranwendung
exklusiv offen (ein zweites open() liefert EBUSY). Das DSW steht aber als
Datum des physischen Ports FC15 im Traffic Memory, und das ist ueber
/dev/mvb0 lesbar - nicht exklusiv, also im laufenden Betrieb, auf dem
Herstellerimage wie auf dem eigenen. Es ist genau der Wert, den der
Busmaster mit seiner Statusabfrage (F-Code 15) bekommt.

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
PCS1 = 0x200 + PP_FC15 * 8 + 2
PCS_VP = 0x0040


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
