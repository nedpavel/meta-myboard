SUMMARY = "Pixy MVBIP (LPC/ISA) MVB controller kernel driver"
DESCRIPTION = "Meng Engineering MVBIP char driver: exposes the on-board MVB \
controller's 64 KB traffic memory (physical 0xD0000, FPGA regs at I/O 0x320) \
to user space via mmap of /dev/pixymvbip. Modernised for Linux 6.x."
LICENSE = "GPL-2.0-only"
LIC_FILES_CHKSUM = "file://COPYING;md5=12f884d2ae1ff87c09e5b7ccc2c4ca7e"

inherit module

SRC_URI = " \
    file://Makefile \
    file://pixymvbip.c \
    file://COPYING \
"

S = "${UNPACKDIR}"

# Beim Boot laden (Traffic-Memory + FPGA-Init passieren erst bei open(),
# aber das Modul muss praesent sein, damit /dev/pixymvbip existiert).
KERNEL_MODULE_AUTOLOAD += "pixymvbip"

# oe-core verlangt den kernel-module-Praefix fuer Modulpakete
RPROVIDES:${PN} += "kernel-module-pixymvbip"

# Nur fuer dieses Board sinnvoll (ISA/LPC-MVB); haengt an linux-intel.
COMPATIBLE_MACHINE = "intel-corei7-64|intel-core2-32"
