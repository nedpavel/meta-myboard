#!/usr/bin/env python3
"""Schnappschuss des MVB Traffic Memory ueber /dev/mvb0.

Nur lesend. IVR0/IVR1 werden ausgelassen - deren Lesen quittiert
Interrupts und wuerde einem laufenden Stack Nachrichten stehlen.

    mvbsnap.py [Zielverzeichnis]
"""

import mmap
import os
import struct
import sys

BAR_SIZE = 0x4000000          # 64 MiB
ISA = 0x2000000               # ISA-Block im BAR
TM = ISA + 0x40000            # Traffic Memory, 256 KiB bei mcm=3
SA = TM + 0x0FC00             # Service Area
REG = 0x380                   # Registerblock, relativ zur Service Area
IVR0 = 0x3C8                  # NICHT LESEN
IVR1 = 0x3CC                  # NICHT LESEN

REGS = {
    0x380: "SCR",  0x384: "MCR",  0x388: "DR",   0x38C: "STSR",
    0x390: "FC",   0x394: "EC",   0x398: "MFR",  0x39C: "MFRE",
    0x3A0: "MR",   0x3A4: "MR2",  0x3A8: "DPR",  0x3AC: "DPR2",
    0x3B0: "IPR0", 0x3B4: "IPR1", 0x3B8: "IMR0", 0x3BC: "IMR1",
    0x3C0: "ISR0", 0x3C4: "ISR1", 0x3C8: "IVR0", 0x3CC: "IVR1",
    0x3D8: "DAOR", 0x3DC: "DAOK", 0x3E0: "TCR",
    0x3F0: "TR1",  0x3F4: "TR2",  0x3F8: "TC1",  0x3FC: "TC2",
}

OUT = sys.argv[1] if len(sys.argv) > 1 else "/tmp/mvbsnap"


def rd16(mm, off):
    return struct.unpack_from("<H", mm, off)[0]


def dump(mm, path, start, length):
    """Wortweise lesen - der MVBC-Bus ist 16 Bit breit."""
    buf = bytearray(length)
    for i in range(0, length, 2):
        struct.pack_into("<H", buf, i, rd16(mm, start + i))
    with open(path, "wb") as f:
        f.write(buf)
    return length


def main():
    os.makedirs(OUT, exist_ok=True)
    fd = os.open("/dev/mvb0", os.O_RDWR)
    try:
        try:
            mm = mmap.mmap(fd, BAR_SIZE, mmap.MAP_SHARED, mmap.PROT_READ)
        except OSError:
            mm = mmap.mmap(fd, BAR_SIZE, mmap.MAP_SHARED,
                           mmap.PROT_READ | mmap.PROT_WRITE)
    finally:
        os.close(fd)

    n = dump(mm, os.path.join(OUT, "tm_low.bin"), TM, 0x0FF80)
    n += dump(mm, os.path.join(OUT, "tm_high.bin"), TM + 0x10000, 0x30000)
    print("Traffic Memory gesichert: %d Byte (Registerblock ausgespart)" % n)

    lines = []
    for off in range(REG, 0x400, 4):
        name = REGS.get(off, "")
        if off in (IVR0, IVR1):
            lines.append("SA+0x%03X :  ----  %-5s uebersprungen" % (off, name))
        else:
            lines.append("SA+0x%03X :  %04X  %s" % (off, rd16(mm, SA + off), name))

    ports = [(p, rd16(mm, TM + p * 2)) for p in range(4096)]
    ports = [(p, v) for p, v in ports if v]

    qdt = tuple(rd16(mm, SA + o) for o in (0x310, 0x312, 0x314))

    with open(os.path.join(OUT, "summary.txt"), "w") as f:
        f.write("MVBC-Register\n-------------\n")
        f.write("\n".join(lines))
        f.write("\n\nQDT  xmit0=%04X  xmit1=%04X  rcve=%04X\n" % qdt)
        f.write("MFS  %04X\n" % rd16(mm, SA + 0x300))
        f.write("\nBelegte PIT-Eintraege: %d\n" % len(ports))
        f.write("Port   Dock-Index\n")
        for p, v in ports:
            f.write("%5d   %5d  (0x%04X)\n" % (p, v, v))

    mm.close()
    print("belegte Ports: %d" % len(ports))
    print("QDT: xmit0=%04X xmit1=%04X rcve=%04X" % qdt)
    print("geschrieben nach %s" % OUT)


main()
