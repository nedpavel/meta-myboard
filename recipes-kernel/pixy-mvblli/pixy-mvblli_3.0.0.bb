SUMMARY = "MVB Link Layer Interface fuer die Pixy-1000"
DESCRIPTION = "Bedient den MVB-Controller im FPGA und stellt /dev/mvblliN \
bereit. Setzt auf pixy-mvb auf, ohne Symbolabhaengigkeit. Ersatz fuer das \
Binaermodul des Herstellers, ABI-gleich."
LICENSE = "GPL-2.0-only"
LIC_FILES_CHKSUM = "file://${COMMON_LICENSE_DIR}/GPL-2.0-only;md5=801f80980d171dd6425610833a22dbe6"

inherit module

SRC_URI = "file://Makefile \
           file://pixy-mvblli.c \
           file://pixy-mvblli.h \
           file://mvbc-regs.h \
           file://pixy-mvb.h \
           file://41-mvblli.rules \
          "

S = "${UNPACKDIR}"

do_install:append() {
    install -d ${D}${includedir}
    install -m 0644 ${S}/pixy-mvblli.h ${D}${includedir}/pixy-mvblli.h

    install -d ${D}${nonarch_base_libdir}/udev/rules.d
    install -m 0644 ${S}/41-mvblli.rules \
        ${D}${nonarch_base_libdir}/udev/rules.d/41-mvblli.rules
}

FILES:${PN} += "${includedir}/pixy-mvblli.h \
                ${nonarch_base_libdir}/udev/rules.d/41-mvblli.rules \
               "

RDEPENDS:${PN} += "kernel-module-pixy-mvb"

# KEIN KERNEL_MODULE_AUTOLOAD. Das Modul oeffnet /dev/mvb0 bereits im
# module_init und scheitert, wenn pixy-mvb noch nicht geladen ist. Es
# wird deshalb von 41-mvblli.rules nachgezogen, sobald der Board-Treiber
# sein Geraet angelegt hat.

RPROVIDES:${PN} += "kernel-module-pixy-mvblli"
