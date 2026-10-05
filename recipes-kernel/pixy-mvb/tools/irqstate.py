#!/usr/bin/env python3
"""Alles, was ueber die Zustellung eines MVB-Interrupts entscheidet - nur
lesend.

    irqstate.py [Datei]

Gedacht fuer den Vergleich "Anwendung laeuft" gegen "Anwendung
gestoppt" gegen "eigener Testlauf". Darf parallel zum Herstellerstack
laufen: /dev/mvb0 ist nicht exklusiv, und IVR0/IVR1 werden nicht gelesen
(das wuerde dem LLI Interrupts stehlen).

Festgehalten wird die ganze Kette vom MVBC bis zur CPU:

  MVBC     IPR/IMR/ISR, SCR, MCR am lebenden Registerblock
  ISA      BASR0/BASR1/BCR (Interruptnummer im unteren Byte)
  GPIO     alle acht Register, auch IMR/ICR/IER des GPIO-Blocks
  PCI      COMMAND (Bus Master), MSI-Capability (Enable, Adresse,
           Daten, Maske, Pending)
  Kernel   pixy-mvb in /proc/interrupts, Modulparameter
"""

import glob
import mmap
import os
import struct
import sys
import time

BOARD = "/dev/mvb0"
BAR_SIZE = 0x4000000
GPIO = 0x1000000
ISA = 0x2000000
TM = ISA + 0x40000
SA_CANDIDATES = (0x03C00, 0x07C00, 0x0FC00)
SA_FOR_MCM = (0x03C00, 0x07C00, 0x0FC00, 0x0FC00, 0x0FC00)
REG = 0x380

MVBC_REGS = ((0x380, "SCR"), (0x384, "MCR"), (0x388, "DR"),
             (0x3B0, "IPR0"), (0x3B4, "IPR1"), (0x3B8, "IMR0"),
             (0x3BC, "IMR1"), (0x3C0, "ISR0"), (0x3C4, "ISR1"),
             (0x3E0, "TCR"))
GPIO_REGS = ("DAT", "ODR", "DIR", "RES", "IMR", "ICR1", "ICR2", "IER")


def live_sa(mm):
    for sa in SA_CANDIDATES:
        mcr = struct.unpack_from("<H", mm, TM + sa + REG + 4)[0]
        mcm = mcr & 7
        if mcr >> 11 == 5 and mcm < len(SA_FOR_MCM) and SA_FOR_MCM[mcm] == sa:
            return sa
    return None


def pci_device():
    for d in glob.glob("/sys/bus/pci/devices/*"):
        try:
            v = open(d + "/vendor").read().strip()
            p = open(d + "/device").read().strip()
        except OSError:
            continue
        if v == "0x1204" and p == "0xec30":
            return d
    return None


def pci_lines(d):
    out = []
    try:
        cfg = open(d + "/config", "rb").read()
    except OSError as e:
        return ["PCI      config nicht lesbar (%s)" % e]
    cmd, sts = struct.unpack_from("<HH", cfg, 4)
    out.append("PCI      %s  COMMAND %04X (Bus Master %s, INTx %s)  STATUS %04X"
               % (os.path.basename(d), cmd, "an" if cmd & 4 else "AUS",
                  "aus" if cmd & 0x400 else "an", sts))
    ptr = cfg[0x34] & 0xFC
    seen = 0
    while ptr and seen < 48 and ptr + 2 <= len(cfg):
        cid, nxt = cfg[ptr], cfg[ptr + 1]
        if cid == 0x05:
            ctl = struct.unpack_from("<H", cfg, ptr + 2)[0]
            is64 = bool(ctl & 0x80)
            maskable = bool(ctl & 0x100)
            lo = struct.unpack_from("<I", cfg, ptr + 4)[0]
            if is64:
                hi = struct.unpack_from("<I", cfg, ptr + 8)[0]
                data_off = ptr + 12
            else:
                hi = 0
                data_off = ptr + 8
            data = struct.unpack_from("<H", cfg, data_off)[0]
            line = ("PCI      MSI @%02X  Enable %s  Vektoren %d/%d  Adresse "
                    "%08X%08X  Daten %04X"
                    % (ptr, "an" if ctl & 1 else "AUS",
                       1 << ((ctl >> 4) & 7), 1 << ((ctl >> 1) & 7),
                       hi, lo, data))
            if maskable:
                mask, pend = struct.unpack_from("<II", cfg, data_off + 4)
                line += "  Maske %08X  Pending %08X" % (mask, pend)
            out.append(line)
        ptr = nxt & 0xFC
        seen += 1
    return out


def irq_line():
    try:
        for ln in open("/proc/interrupts"):
            if ln.rstrip().endswith("pixy-mvb"):
                f = ln.split()
                return "Kernel   /proc/interrupts %s pixy-mvb %d" % (
                    f[0], sum(int(x) for x in f[1:] if x.isdigit()))
    except OSError:
        pass
    return "Kernel   pixy-mvb nicht in /proc/interrupts"


def params():
    out = []
    for f in sorted(glob.glob("/sys/module/pixy_mvb*/parameters/*")):
        if "/dbg_" in f:
            continue
        try:
            out.append("%s=%s" % (f.split("/")[3] + "." + os.path.basename(f),
                                  open(f).read().strip()))
        except OSError:
            pass
    return "Kernel   Parameter " + (" ".join(out) or "keine lesbar")


def capture():
    """Zustand als Liste von Textzeilen."""
    lines = ["irqstate %s" % time.strftime("%Y-%m-%d %H:%M:%S")]
    fb = os.open(BOARD, os.O_RDONLY)
    try:
        mm = mmap.mmap(fb, BAR_SIZE, mmap.MAP_SHARED, mmap.PROT_READ)
    finally:
        os.close(fb)
    try:
        sa = live_sa(mm)
        if sa is None:
            lines.append("MVBC     kein Registerblock erkannt")
        else:
            vals = ["%s %04X" % (n, struct.unpack_from(
                "<H", mm, TM + sa + o)[0]) for o, n in MVBC_REGS]
            lines.append("MVBC     @%04X  %s" % (sa, "  ".join(vals)))
        lines.append("ISA      BASR0 %04X  BASR1 %04X  BCR %04X"
                     % struct.unpack_from("<HHH", mm, ISA + 0x320))
        g = struct.unpack_from("<8I", mm, GPIO)
        lines.append("GPIO     " + "  ".join(
            "%s %08X" % (n, v) for n, v in zip(GPIO_REGS, g)))
    finally:
        mm.close()
    d = pci_device()
    lines += pci_lines(d) if d else ["PCI      1204:EC30 nicht gefunden"]
    lines.append(irq_line())
    lines.append(params())
    return lines


def main():
    text = "\n".join(capture()) + "\n"
    sys.stdout.write(text)
    if len(sys.argv) > 1:
        with open(sys.argv[1], "w") as f:
            f.write(text)


if __name__ == "__main__":
    main()
