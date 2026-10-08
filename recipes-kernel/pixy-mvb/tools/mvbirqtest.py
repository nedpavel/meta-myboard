#!/usr/bin/env python3
"""Misst, ob der MVBC Interrupts ausloest und wer sie bekommt.

Der Framezaehler des MVBC saettigt bei 0xFFFF und loest dabei FEV aus.
Ist er einmal voll und wird nicht geleert, meldet sich FEV nie wieder -
dann ist ein Ausbleiben von Interrupts kein Fehler, sondern Folge des
Altzustands. Dieses Skript stellt deshalb einen definierten Anfang her:

  1. Zaehler der Treiber ablesen
  2. /dev/mvblli0 oeffnen
  3. HWINIT aufrufen, das leert die Zaehler des MVBC
  4. warten, bis der Framezaehler ueberlaufen muss
  5. Zaehler erneut ablesen

Danach steht fest, ob FEV ausgeloest hat und wo die Meldung geblieben
ist.

    mvbirqtest.py [Wartezeit in Sekunden, Vorgabe 240]

Gelesen wird nur die erste Cachezeile des Registerblocks (FC, EC, IPR,
IMR). ISR0/ISR1 liegen in der zweiten, zusammen mit IVR0/IVR1: jeder
Zugriff dort quittiert anstehende Interrupts am Treiber vorbei, und bis
zum Neustart kommt keiner mehr (ABI.md, "Cachezeilen"). Ob eine Quelle
ansteht, zeigt IPR & IMR ebenso.
"""

import ctypes
import fcntl
import glob
import mmap
import os
import struct
import sys
import time

BAR_SIZE = 0x4000000
ISA = 0x2000000
TM = ISA + 0x40000
# Registerblock relativ zur Service Area, nur aus der ersten Cachezeile
MCR = 0x384
FC = 0x390
EC = 0x394
IPR0 = 0x3B0
IPR1 = 0x3B4
IMR0 = 0x3B8
IMR1 = 0x3BC

# Lage der Service Area je mcm. Nach close() (RESET, mcm 0) steht der
# Registerblock am Grundplatz 0x3C00, nach open() (mcm 3) bei 0xFC00.
SA_CANDIDATES = (0x03C00, 0x07C00, 0x0FC00)
SA_FOR_MCM = (0x03C00, 0x07C00, 0x0FC00, 0x0FC00, 0x0FC00)

# _IOW('L', 21, 16) - PixyMvblliConfigLpTs ist 16 Byte gross
IOCTL_HWINIT = 0x40104C15

# Obergrenze fuer "FC wurde geleert" direkt nach HWINIT
FC_CLEARED_MAX = 0x1000

WAIT = int(sys.argv[1]) if len(sys.argv) > 1 else 240


def counters():
    """Alle dbg_*-Zaehler beider Module."""
    out = {}
    for p in sorted(glob.glob("/sys/module/pixy_mvb*/parameters/dbg_*")):
        mod = p.split("/")[3]
        name = os.path.basename(p)
        try:
            with open(p) as f:
                out["%s/%s" % (mod, name)] = f.read().strip()
        except OSError:
            pass
    return out


def show(label, c):
    print("%s:" % label)
    for k in sorted(c):
        print("   %-34s %s" % (k, c[k]))


def live_sa(mm):
    """Platz des Registerblocks: MCR traegt Version 5 (MVBC02D) und sein
    mcm verweist auf genau diesen Platz. None, solange das TM-Fenster
    nicht eingerichtet ist (erst das erste open() nach dem Booten tut
    das)."""
    for sa in SA_CANDIDATES:
        mcr = struct.unpack_from("<H", mm, TM + sa + MCR)[0]
        mcm = mcr & 7
        if mcr >> 11 == 5 and mcm < len(SA_FOR_MCM) and SA_FOR_MCM[mcm] == sa:
            return sa
    return None


def regs(mm):
    """FC, EC, IPR0, IPR1, IMR0, IMR1 - oder None ohne Registerblock."""
    sa = live_sa(mm)
    if sa is None:
        return None
    rd = lambda o: struct.unpack_from("<H", mm, TM + sa + o)[0]
    return rd(FC), rd(EC), rd(IPR0), rd(IPR1), rd(IMR0), rd(IMR1)


def fmt(r):
    if r is None:
        return "kein Registerblock erkannt (TM-Fenster nicht eingerichtet?)"
    fc, ec, ipr0, ipr1, imr0, imr1 = r
    return ("FC %04X  EC %04X  IPR0 %04X IPR1 %04X  IMR0 %04X IMR1 %04X"
            "  anstehend+frei %04X/%04X"
            % (fc, ec, ipr0, ipr1, imr0, imr1, ipr0 & imr0, ipr1 & imr1))


def main():
    fd_board = os.open("/dev/mvb0", os.O_RDWR)
    try:
        mm = mmap.mmap(fd_board, BAR_SIZE, mmap.MAP_SHARED, mmap.PROT_READ)
    finally:
        os.close(fd_board)

    before = counters()
    show("Zaehler vor dem Oeffnen", before)

    print("\nRegister vor dem Oeffnen:")
    print("   " + fmt(regs(mm)))

    fd = os.open("/dev/mvblli0", os.O_RDWR)
    print("\n/dev/mvblli0 geoeffnet")

    # PixyMvblliConfigLpTs: pb_mwd, ownership, ts_type, prt_addr_max,
    # prt_indx_max, auto_reset_rld. ts_type muss 1 sein, sonst EIO -
    # dieselbe Belegung wie in mvbdiff.py, Schritt HWINIT.
    conf = ctypes.create_string_buffer(struct.pack("<QBBHHB x",
                                                   0, 1, 1, 0xFFF, 0xFFF, 0),
                                       16)
    try:
        fcntl.ioctl(fd, IOCTL_HWINIT, conf)
        print("HWINIT ausgefuehrt")
    except OSError as e:
        print("HWINIT abgelehnt: %s" % e)

    r = regs(mm)
    print("   danach: " + fmt(r))
    # Am laufenden Bus zaehlt FC zwischen HWINIT und dem Lesen weiter
    # (am Laborbus ~2500 Frames/s, gemessen FC 002C) - geleert heisst
    # also "klein", nicht "null".
    if r is None or r[0] >= FC_CLEARED_MAX:
        print("   >>> FC wurde NICHT geleert - ohne das ist der Rest "
              "nicht aussagekraeftig")

    print("\n%d Sekunden warten, FC muss dabei ueberlaufen ..." % WAIT)
    step = max(WAIT // 8, 15)
    t = 0
    while t < WAIT:
        time.sleep(min(step, WAIT - t))
        t += step
        print("   +%4ds   %s" % (t, fmt(regs(mm))))

    os.close(fd)
    print("\n/dev/mvblli0 geschlossen")

    after = counters()
    print()
    show("Zaehler nach dem Lauf", after)

    print("\nVeraenderung:")
    for k in sorted(set(before) | set(after)):
        a, b = before.get(k, "-"), after.get(k, "-")
        if a != b:
            print("   %-34s %s -> %s" % (k, a, b))

    mm.close()


main()
