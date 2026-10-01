SUMMARY = "Kiosk browser session for the MyBoard display"
DESCRIPTION = "Startskript und X-Sitzung, um eine Webseite im Vollbild \
anzuzeigen — anstelle der Qt-Oberflaeche. Verwendet surf (WebKitGTK) und \
matchbox-window-manager; letzterer wird gebraucht, weil ohne \
Window-Manager ein GTK-Fenster seine Wunschgroesse behaelt und den \
Bildschirm nicht fuellt."
LICENSE = "MIT"
LIC_FILES_CHKSUM = "file://${COMMON_LICENSE_DIR}/MIT;md5=0835ade698e0bcf8506ecda2f7b4f302"

SRC_URI = " \
    file://browser-start \
    file://browser-xinitrc \
"
S = "${UNPACKDIR}"

RDEPENDS:${PN} = "surf matchbox-wm xinit"

do_install() {
    install -d ${D}${bindir}
    install -m 0755 ${UNPACKDIR}/browser-start ${D}${bindir}/browser-start
    install -d ${D}${sysconfdir}
    install -m 0755 ${UNPACKDIR}/browser-xinitrc ${D}${sysconfdir}/browser-xinitrc
}

FILES:${PN} = " \
    ${bindir}/browser-start \
    ${sysconfdir}/browser-xinitrc \
"
