SUMMARY = "Web-Oberflaeche des Bediendisplays (Firefox) und ihr Hintergrund-Dienst"
DESCRIPTION = "Dieselbe Bedienoberflaeche wie myboard-gui, als HTML/JavaScript \
unter /gui/ (Apache) - am Geraet im Firefox-Kiosk und ueber das Netz. Der \
Dienst myboard-webgui-api (127.0.0.1:8081, Apache /api/) liest CAN, fragt \
DDS ueber idu2-mvb ab und spielt Updates der Web-App vom USB-Stick ein. \
Das Paket myboard-webgui-kiosk startet Firefox mit /gui/ auf dem \
Bildschirm; es ist nicht aktiviert - umschalten mit 'oberflaeche web|qt'."
LICENSE = "MIT"
LIC_FILES_CHKSUM = "file://${COMMON_LICENSE_DIR}/MIT;md5=0835ade698e0bcf8506ecda2f7b4f302"

# Signaltabelle und Wagenbilder stammen aus der Qt-App - eine Quelle fuer
# beide Oberflaechen, keine Kopie im Layer
FILESEXTRAPATHS:prepend := "${THISDIR}/../myboard-gui/files/src:"

SRC_URI = " \
    file://myboard-webgui-api.py \
    file://myboard-webgui-api.service \
    file://myboard-webgui-kiosk.service \
    file://myboard-webgui-apache.conf \
    file://oberflaeche \
    file://www/index.html \
    file://www/app.css \
    file://www/app.js \
    file://can_matrix.tsv \
    file://ICN_Office.jpg \
    file://ICN_Office_2.jpg \
"
S = "${UNPACKDIR}"

inherit allarch systemd

PACKAGES =+ "${PN}-kiosk"
SYSTEMD_PACKAGES = "${PN} ${PN}-kiosk"
SYSTEMD_SERVICE:${PN} = "myboard-webgui-api.service"
SYSTEMD_AUTO_ENABLE:${PN} = "enable"
# Qt bleibt die Oberflaeche beim Booten; der Kiosk wird nur per
# 'oberflaeche web' eingeschaltet
SYSTEMD_SERVICE:${PN}-kiosk = "myboard-webgui-kiosk.service"
SYSTEMD_AUTO_ENABLE:${PN}-kiosk = "disable"

WEBGUI_DIR = "${datadir}/myboard-webgui"

do_install() {
    install -d ${D}${bindir}
    install -m 0755 ${UNPACKDIR}/myboard-webgui-api.py ${D}${bindir}/myboard-webgui-api
    install -m 0755 ${UNPACKDIR}/oberflaeche ${D}${bindir}/oberflaeche

    install -d ${D}${WEBGUI_DIR}/www/img
    install -m 0644 ${UNPACKDIR}/can_matrix.tsv ${D}${WEBGUI_DIR}/
    for f in index.html app.css app.js; do
        install -m 0644 ${UNPACKDIR}/www/$f ${D}${WEBGUI_DIR}/www/
    done
    install -m 0644 ${UNPACKDIR}/ICN_Office.jpg ${UNPACKDIR}/ICN_Office_2.jpg \
        ${D}${WEBGUI_DIR}/www/img/

    # /gui/ und /api/ in Apache
    install -d ${D}${sysconfdir}/apache2/conf.d
    install -m 0644 ${UNPACKDIR}/myboard-webgui-apache.conf \
        ${D}${sysconfdir}/apache2/conf.d/myboard-webgui.conf

    install -d ${D}${systemd_system_unitdir}
    install -m 0644 ${UNPACKDIR}/myboard-webgui-api.service \
        ${UNPACKDIR}/myboard-webgui-kiosk.service ${D}${systemd_system_unitdir}/
}

FILES:${PN} = " \
    ${bindir}/myboard-webgui-api \
    ${WEBGUI_DIR} \
    ${sysconfdir}/apache2/conf.d/myboard-webgui.conf \
    ${systemd_system_unitdir}/myboard-webgui-api.service \
"
FILES:${PN}-kiosk = " \
    ${bindir}/oberflaeche \
    ${systemd_system_unitdir}/myboard-webgui-kiosk.service \
"

# http.server: netclient; socketserver: netserver; socket/tempfile: io;
# tarfile: compression. Apache mit mod_proxy: myboard-lamp
RDEPENDS:${PN} = "myboard-lamp python3-core python3-netclient python3-netserver \
                  python3-json python3-io python3-compression python3-threading"
RDEPENDS:${PN}-kiosk = "${PN} browser-kiosk"
