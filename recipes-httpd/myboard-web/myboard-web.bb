SUMMARY = "Web pages for the MyBoard data stream"
DESCRIPTION = "PHP-Seiten fuer das Pixy 1000: upload.php nimmt den \
Datenstrom des sendenden Geraets per HTTP POST entgegen, index.php zeigt \
den aktuellen Wert und den Verlauf an. Ausgeliefert von nginx, ausgefuehrt \
von php-fpm."
LICENSE = "MIT"
LIC_FILES_CHKSUM = "file://${COMMON_LICENSE_DIR}/MIT;md5=0835ade698e0bcf8506ecda2f7b4f302"

SRC_URI = " \
    file://index.php \
    file://upload.php \
    file://myboard-web.tmpfiles \
"
S = "${UNPACKDIR}"

inherit systemd

# Ohne Webserver und PHP-Laufzeit nutzlos
RDEPENDS:${PN} = "nginx php-fpm"

WWWDIR = "${localstatedir}/www/localhost/html"

do_install() {
    install -d ${D}${WWWDIR}
    install -m 0644 ${UNPACKDIR}/index.php  ${D}${WWWDIR}/
    install -m 0644 ${UNPACKDIR}/upload.php ${D}${WWWDIR}/

    # Das Datenverzeichnis wird zur Laufzeit angelegt — mit Besitzer
    # "nobody", weil php-fpm unter diesem Benutzer schreibt.
    install -d ${D}${nonarch_libdir}/tmpfiles.d
    install -m 0644 ${UNPACKDIR}/myboard-web.tmpfiles \
        ${D}${nonarch_libdir}/tmpfiles.d/myboard-web.conf
}

FILES:${PN} = " \
    ${WWWDIR}/index.php \
    ${WWWDIR}/upload.php \
    ${nonarch_libdir}/tmpfiles.d/myboard-web.conf \
"
