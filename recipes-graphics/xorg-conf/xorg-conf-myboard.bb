SUMMARY = "Xorg fbdev configuration for the kiosk display"
LICENSE = "MIT"
LIC_FILES_CHKSUM = "file://${COMMON_LICENSE_DIR}/MIT;md5=0835ade698e0bcf8506ecda2f7b4f302"

FILESEXTRAPATHS:prepend := "${THISDIR}/files:"
SRC_URI = "file://20-fbdev.conf"
S = "${UNPACKDIR}"

do_install() {
    install -d ${D}${sysconfdir}/X11/xorg.conf.d
    install -m 0644 ${UNPACKDIR}/20-fbdev.conf \
        ${D}${sysconfdir}/X11/xorg.conf.d/20-fbdev.conf
}

FILES:${PN} = "${sysconfdir}/X11/xorg.conf.d/20-fbdev.conf"
