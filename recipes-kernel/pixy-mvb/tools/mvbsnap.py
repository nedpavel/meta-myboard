#!/usr/bin/env python3
"""Schnappschuss des MVB Traffic Memory ueber /dev/mvb0.

Nur lesend. IVR0/IVR1 werden an jeder Kandidatenadresse ausgelassen -
deren Lesen quittiert Interrupts und wuerde einem laufenden Stack
Nachrichten stehlen.

Die Service Area wird nicht geraten, sondern aus dem MCR bestimmt: der
MVBC verschiebt sie je nach programmiertem mcm. Solange MCR nicht
geschrieben ist, liegt sie am Grundplatz TM+0x3C00 - deshalb wird der
Registerblock an allen drei Kandidatenadressen ausgegeben. Bleibt nach
einem open() nur der Grundplatz belegt, ist die Initialisierung des
Treibers stehengeblieben.

    mvbsnap.py [Zielverzeichnis]
"""

import mmap
import os
import struct
import sys
import time

BAR_SIZE = 0x4000000          # 64 MiB
ISA = 0x2000000               # ISA-Block im BAR
TM = ISA + 0x40000            # Traffic Memory, 256 KiB bei mcm=3
REG = 0x380                   # Registerblock, relativ zur Service Area
IVR0 = 0x3C8                  # NICHT LESEN
IVR1 = 0x3CC                  # NICHT LESEN

# Lage der Service Area, indiziert mit MCR.mcm
SA_OFFSETS = (0x03C00, 0x07C00, 0x0FC00, 0x0FC00, 0x0FC00)
SA_CANDIDATES = (0x03C00, 0x07C00, 0x0FC00)
MVBC02D = 5                   # MCR >> 11

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


def regblock(mm, sa_off):
    """Registerblock einer Kandidatenadresse, IVR ausgespart."""
    lines = []
    for off in range(REG, 0x400, 4):
        name = REGS.get(off, "")
        if off in (IVR0, IVR1):
            lines.append("SA+0x%03X :  ----  %-5s uebersprungen" % (off, name))
        else:
            lines.append("SA+0x%03X :  %04X  %s"
                         % (off, rd16(mm, TM + sa_off + off), name))
    return lines


def loaded_modules():
    """Welche Treiber waren beim Abzug geladen? Ohne das laesst sich
    hinterher nicht mehr sagen, welche Aufnahme wozu gehoert."""
    lines = []
    try:
        with open("/proc/modules") as f:
            mods = [l.split() for l in f if l.startswith("pixy")]
    except OSError:
        return ["  /proc/modules nicht lesbar"]

    for m in sorted(mods):
        name = m[0]
        info = []
        for attr in ("version", "srcversion"):
            try:
                with open("/sys/module/%s/%s" % (name, attr)) as f:
                    info.append("%s %s" % (attr, f.read().strip()))
            except OSError:
                pass
        lines.append("  %-16s %8s Byte   %s"
                     % (name, m[1], ", ".join(info) if info else "-"))
    return lines or ["  kein pixy-Modul geladen"]


def find_sa(mm):
    """Aktive Service Area aus dem MCR bestimmen."""
    for cand in SA_CANDIDATES:
        mcr = rd16(mm, TM + cand + 0x384)
        if mcr >> 11 != MVBC02D:
            continue
        if SA_OFFSETS[mcr & 7] == cand:
            return cand, mcr
    return None, None


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

    sa_off, mcr = find_sa(mm)
    if sa_off is None:
        sa_off = 0x0FC00
        note = ("Keine gueltige Service Area gefunden - kein MCR mit "
                "Version MVBC02D und passendem mcm.\n"
                "Ersatzweise wird TM+0x0FC00 ausgewertet; die Werte "
                "unten sind dann nicht belastbar.\n")
        print("WARNUNG: keine gueltige Service Area gefunden")
    else:
        note = ("Aktive Service Area: TM+0x%05X   MCR %04X "
                "(Version %d, mcm %d)\n" % (sa_off, mcr, mcr >> 11, mcr & 7))
        print("aktive Service Area: TM+0x%05X (mcm %d)" % (sa_off, mcr & 7))

    SA = TM + sa_off

    ports = [(p, rd16(mm, TM + p * 2)) for p in range(4096)]
    ports = [(p, v) for p, v in ports if v]

    qdt = tuple(rd16(mm, SA + o) for o in (0x310, 0x312, 0x314))

    stamp = time.strftime("%Y-%m-%d %H:%M:%S")
    mods = loaded_modules()

    with open(os.path.join(OUT, "summary.txt"), "w") as f:
        f.write("Aufgenommen: %s\n" % stamp)
        f.write("Geladene Treiber:\n")
        f.write("\n".join(mods))
        f.write("\n\n")
        f.write(note)
        f.write("\nMVBC-Register\n-------------\n")
        f.write("\n".join(regblock(mm, sa_off)))
        f.write("\n\nQDT  xmit0=%04X  xmit1=%04X  rcve=%04X\n" % qdt)
        f.write("MFS  %04X\n" % rd16(mm, SA + 0x300))
        f.write("\nBelegte PIT-Eintraege: %d\n" % len(ports))
        f.write("Port   Dock-Index\n")
        for p, v in ports:
            f.write("%5d   %5d  (0x%04X)\n" % (p, v, v))

    # Die Kandidatenadressen stehen in einer eigenen Datei. Nur an einer
    # davon liegen wirklich Register; die anderen zeigen gewoehnlichen
    # Speicherinhalt, der sich staendig aendert. In summary.txt wuerde
    # das jeden Vergleich unbrauchbar machen.
    with open(os.path.join(OUT, "candidates.txt"), "w") as f:
        f.write("Aufgenommen: %s\n\n" % stamp)
        for cand in SA_CANDIDATES:
            f.write("Registerblock an TM+0x%05X%s\n"
                    % (cand, "   <-- aktiv" if cand == sa_off else
                       "   (inaktiv, Inhalt ist kein Registersatz)"))
            f.write("-" * 52 + "\n")
            f.write("\n".join(regblock(mm, cand)))
            f.write("\n\n")

    mm.close()
    print("belegte Ports: %d" % len(ports))
    print("QDT: xmit0=%04X xmit1=%04X rcve=%04X" % qdt)
    print("geschrieben nach %s" % OUT)


main()
