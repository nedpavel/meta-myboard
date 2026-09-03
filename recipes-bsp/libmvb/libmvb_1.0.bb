SUMMARY = "MVB (MVBC02/MVBIP) userspace library and diagnostic tool"
DESCRIPTION = "libmvb greift ueber das vom pixymvbip-Treiber gemappte \
64-KB-Traffic-Memory auf den MVB-Controller zu: MVBC-Register, \
Prozessdaten-Ports (Klasse 1) und die TM-Anbindung fuer Message-Daten. \
Enthaelt ausserdem 'mvbtool' zur Diagnose auf dem Board."

# Eigener Code unter MIT. HINWEIS: mvbc.h stammt vom Hersteller
# (ABB/Adtranz -> Pixy/HaslerRail) und traegt keine eigene Lizenzangabe;
# die Weitergabe ist mit dem Lieferanten zu klaeren.
LICENSE = "MIT"
LIC_FILES_CHKSUM = "file://${COMMON_LICENSE_DIR}/MIT;md5=0835ade698e0bcf8506ecda2f7b4f302"

SRC_URI = " \
    file://libmvb.c \
    file://libmvb.h \
    file://mvbc.h \
    file://mvbtool.c \
    file://Makefile \
"

S = "${UNPACKDIR}"

# Ohne den Treiber gibt es kein /dev/pixymvbip
RDEPENDS:${PN} += "kernel-module-pixymvbip"

do_compile() {
    oe_runmake
}

do_install() {
    oe_runmake install DESTDIR=${D} \
        prefix=${prefix} libdir=${libdir} \
        includedir=${includedir} bindir=${bindir}
}

FILES:${PN} += "${libdir}/libmvb.so.*"
FILES:${PN}-dev += "${libdir}/libmvb.so ${includedir}/libmvb.h ${includedir}/mvbc.h"

COMPATIBLE_MACHINE = "intel-corei7-64|intel-core2-32"
