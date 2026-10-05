SUMMARY = "Xorg configuration for the kiosk display"
DESCRIPTION = "Installiert die aktive modesetting-Konfiguration sowie eine \
deaktivierte fbdev-Variante als Rueckfalloption. Beide schalten \
Bildschirm-Blanking und DPMS ab (Kiosk-Betrieb). \
Auf dem Pixy 1000 ist modesetting zwingend: fbdev scheitert dort an der \
DRM-Framebuffer-Emulation (FBIOPUTCMAP) und liefert einen schwarzen Schirm."
LICENSE = "MIT"
LIC_FILES_CHKSUM = "file://${COMMON_LICENSE_DIR}/MIT;md5=0835ade698e0bcf8506ecda2f7b4f302"

FILESEXTRAPATHS:prepend := "${THISDIR}/files:"
SRC_URI = " \
    file://20-modesetting.conf \
    file://20-fbdev.conf.disabled \
    file://50-touchscreen.conf \
"
S = "${UNPACKDIR}"

do_install() {
    install -d ${D}${sysconfdir}/X11/xorg.conf.d
    # Aktiv: modesetting (DRM/i915) — auf dem Pixy 1000 verifiziert
    install -m 0644 ${UNPACKDIR}/20-modesetting.conf \
        ${D}${sysconfdir}/X11/xorg.conf.d/20-modesetting.conf
    # Inaktiv: fbdev als Rueckfalloption. Xorg liest nur *.conf,
    # die Endung .disabled haelt die Datei aus dem Spiel.
    install -m 0644 ${UNPACKDIR}/20-fbdev.conf.disabled \
        ${D}${sysconfdir}/X11/xorg.conf.d/20-fbdev.conf.disabled
    # Touchscreen-Kalibrierung (Transformationsmatrix fuer libinput)
    install -m 0644 ${UNPACKDIR}/50-touchscreen.conf \
        ${D}${sysconfdir}/X11/xorg.conf.d/50-touchscreen.conf
}

FILES:${PN} = " \
    ${sysconfdir}/X11/xorg.conf.d/20-modesetting.conf \
    ${sysconfdir}/X11/xorg.conf.d/20-fbdev.conf.disabled \
    ${sysconfdir}/X11/xorg.conf.d/50-touchscreen.conf \
"
