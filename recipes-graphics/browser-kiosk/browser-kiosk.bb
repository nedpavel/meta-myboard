SUMMARY = "Kiosk browser session for the MyBoard display"
DESCRIPTION = "Startskript und X-Sitzung, um eine Webseite im Vollbild \
anzuzeigen — anstelle der Qt-Oberflaeche. Verwendet Firefox (ESR, aus \
meta-browser/meta-firefox) im Kiosk-Modus und matchbox-window-manager; \
letzterer wird gebraucht, weil ohne Window-Manager ein GTK-Fenster seine \
Wunschgroesse behaelt und den Bildschirm nicht fuellt. Dazu Policies und \
Voreinstellungen fuer den Kiosk-Betrieb."
LICENSE = "MIT"
LIC_FILES_CHKSUM = "file://${COMMON_LICENSE_DIR}/MIT;md5=0835ade698e0bcf8506ecda2f7b4f302"

SRC_URI = " \
    file://browser-start \
    file://browser-xinitrc \
    file://policies.json \
    file://kiosk.js \
"
S = "${UNPACKDIR}"

RDEPENDS:${PN} = "firefox matchbox-wm xinit"

# Firefox liegt unter ${libdir}/firefox (firefox.inc). Policies liest es aus
# distribution/policies.json, Voreinstellungen aus defaults/pref/*.js.
FIREFOX_DIR = "${libdir}/firefox"

do_install() {
    install -d ${D}${bindir}
    install -m 0755 ${UNPACKDIR}/browser-start ${D}${bindir}/browser-start
    install -d ${D}${sysconfdir}
    install -m 0755 ${UNPACKDIR}/browser-xinitrc ${D}${sysconfdir}/browser-xinitrc
    install -d ${D}${FIREFOX_DIR}/distribution ${D}${FIREFOX_DIR}/defaults/pref
    install -m 0644 ${UNPACKDIR}/policies.json ${D}${FIREFOX_DIR}/distribution/policies.json
    install -m 0644 ${UNPACKDIR}/kiosk.js ${D}${FIREFOX_DIR}/defaults/pref/kiosk.js
}

FILES:${PN} = " \
    ${bindir}/browser-start \
    ${sysconfdir}/browser-xinitrc \
    ${FIREFOX_DIR}/distribution/policies.json \
    ${FIREFOX_DIR}/defaults/pref/kiosk.js \
"
