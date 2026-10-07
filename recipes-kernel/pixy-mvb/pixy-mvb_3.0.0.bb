SUMMARY = "Board-Treiber fuer die Pixy-1000 MVB/PC104 Extension Board (PCIe)"
DESCRIPTION = "Stellt /dev/mvbN bereit, bildet BAR0 per mmap ab und verteilt \
die MSI-Interrupts an angemeldete Kernel-Dienste. Ersatz fuer das \
Binaermodul des Herstellers, ABI-gleich."
LICENSE = "GPL-2.0-only"
LIC_FILES_CHKSUM = "file://${COMMON_LICENSE_DIR}/GPL-2.0-only;md5=801f80980d171dd6425610833a22dbe6"

inherit module

SRC_URI = "file://Makefile \
           file://pixy-mvb.c \
           file://pixy-mvb.h \
           file://40-mvb.rules \
          "

S = "${UNPACKDIR}"

do_install:append() {
    install -d ${D}${includedir}
    install -m 0644 ${S}/pixy-mvb.h ${D}${includedir}/pixy-mvb.h

    # Legt /dev/mvb an und setzt die Rechte. Ohne die Regel existiert nur
    # /dev/mvb0, und Programme, die den Symlink oeffnen, scheitern.
    install -d ${D}${nonarch_base_libdir}/udev/rules.d
    install -m 0644 ${S}/40-mvb.rules \
        ${D}${nonarch_base_libdir}/udev/rules.d/40-mvb.rules
}

FILES:${PN} += "${includedir}/pixy-mvb.h \
                ${nonarch_base_libdir}/udev/rules.d/40-mvb.rules \
               "

# Beim Booten automatisch laden. Nur dieses Modul: pixy-mvblli kommt
# ueber die udev-Regel, weil es /dev/mvb0 beim Laden schon braucht.
KERNEL_MODULE_AUTOLOAD += "pixy-mvb"

RPROVIDES:${PN} += "kernel-module-pixy-mvb"
