SUMMARY = "Xorg configuration for the kiosk display"
DESCRIPTION = "Installiert die aktive fbdev-Konfiguration sowie eine \
deaktivierte modesetting-Variante als Rueckfalloption. Beide schalten \
Bildschirm-Blanking und DPMS ab (Kiosk-Betrieb)."
LICENSE = "MIT"
LIC_FILES_CHKSUM = "file://${COMMON_LICENSE_DIR}/MIT;md5=0835ade698e0bcf8506ecda2f7b4f302"

FILESEXTRAPATHS:prepend := "${THISDIR}/files:"
SRC_URI = " \
    file://20-fbdev.conf \
    file://20-modesetting.conf.disabled \
"
S = "${UNPACKDIR}"

do_install() {
    install -d ${D}${sysconfdir}/X11/xorg.conf.d
    # Aktiv: fbdev (konservativ, funktioniert mit DRM_FBDEV_EMULATION)
    install -m 0644 ${UNPACKDIR}/20-fbdev.conf \
        ${D}${sysconfdir}/X11/xorg.conf.d/20-fbdev.conf
    # Inaktiv: modesetting als Rueckfalloption. Xorg liest nur *.conf,
    # die Endung .disabled haelt die Datei aus dem Spiel.
    install -m 0644 ${UNPACKDIR}/20-modesetting.conf.disabled \
        ${D}${sysconfdir}/X11/xorg.conf.d/20-modesetting.conf.disabled
}

FILES:${PN} = " \
    ${sysconfdir}/X11/xorg.conf.d/20-fbdev.conf \
    ${sysconfdir}/X11/xorg.conf.d/20-modesetting.conf.disabled \
"
