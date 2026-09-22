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
SA = TM + 0x0FC00
FC = 0x390
EC = 0x394
ISR1 = 0x3C4
IMR0 = 0x3B8
IMR1 = 0x3BC

# _IOW('L', 21, 16) - PixyMvblliConfigLpTs ist 16 Byte gross
IOCTL_HWINIT = 0x40104C15

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


def regs(mm):
    rd = lambda o: struct.unpack_from("<H", mm, SA + o)[0]
    return rd(FC), rd(EC), rd(ISR1), rd(IMR0), rd(IMR1)


def main():
    fd_board = os.open("/dev/mvb0", os.O_RDWR)
    try:
        mm = mmap.mmap(fd_board, BAR_SIZE, mmap.MAP_SHARED, mmap.PROT_READ)
    finally:
        os.close(fd_board)

    before = counters()
    show("Zaehler vor dem Oeffnen", before)

    fc, ec, isr1, imr0, imr1 = regs(mm)
    print("\nRegister vor dem Oeffnen:")
    print("   FC %04X  EC %04X  ISR1 %04X  IMR0 %04X  IMR1 %04X"
          % (fc, ec, isr1, imr0, imr1))

    fd = os.open("/dev/mvblli0", os.O_RDWR)
    print("\n/dev/mvblli0 geoeffnet")

    # PixyMvblliConfigLpTs: Zeiger, 2 Byte, 2 x uint16, 1 Byte
    conf = ctypes.create_string_buffer(struct.pack("<QBBHHB x",
                                                   0, 0, 0, 0, 0, 0), 16)
    try:
        fcntl.ioctl(fd, IOCTL_HWINIT, conf)
        print("HWINIT ausgefuehrt")
    except OSError as e:
        print("HWINIT abgelehnt: %s" % e)

    fc, ec, isr1, _, _ = regs(mm)
    print("   danach: FC %04X  EC %04X  ISR1 %04X" % (fc, ec, isr1))
    if fc != 0:
        print("   >>> FC wurde NICHT geleert - ohne das ist der Rest "
              "nicht aussagekraeftig")

    print("\n%d Sekunden warten, FC muss dabei ueberlaufen ..." % WAIT)
    step = max(WAIT // 8, 15)
    t = 0
    while t < WAIT:
        time.sleep(min(step, WAIT - t))
        t += step
        fc, ec, isr1, _, _ = regs(mm)
        print("   +%4ds   FC %04X   EC %04X   ISR1 %04X" % (t, fc, ec, isr1))

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
