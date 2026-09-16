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
          "

S = "${UNPACKDIR}"

do_install:append() {
    install -d ${D}${includedir}
    install -m 0644 ${S}/pixy-mvblli.h ${D}${includedir}/pixy-mvblli.h
}

FILES:${PN} += "${includedir}/pixy-mvblli.h"

# pixy-mvb muss vorher geladen sein - /dev/mvb0 wird beim Modulstart geoeffnet
RDEPENDS:${PN} += "kernel-module-pixy-mvb"
KERNEL_MODULE_AUTOLOAD += "pixy-mvblli"

RPROVIDES:${PN} += "kernel-module-pixy-mvblli"
