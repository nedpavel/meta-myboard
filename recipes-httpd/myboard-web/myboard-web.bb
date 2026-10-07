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
    file://daten.php \
    file://myboard-web.tmpfiles \
"
S = "${UNPACKDIR}"

inherit systemd

# Ohne Webserver und PHP-Laufzeit nutzlos
RDEPENDS:${PN} = "nginx php-fpm"

# Die Seiten liegen in einem eigenen Verzeichnis, NICHT direkt in
# /var/www/localhost/html. Grund: RPM beansprucht automatisch jedes
# Verzeichnis, in dem ein Paket Dateien ablegt ("All packages own the
# directories their files are in", package_rpm.bbclass) — und
# /var/www/localhost[/html] gehoert bereits dem nginx-Paket. do_rootfs
# brach deshalb ab:
#   "file /var/www/localhost conflicts between attempted installs of
#    myboard-web and nginx"
# Beim Boot werden die Seiten per tmpfiles-Symlink eingehaengt.
PAGEDIR = "${datadir}/myboard-web"

do_install() {
    install -d ${D}${PAGEDIR}
    install -m 0644 ${UNPACKDIR}/index.php  ${D}${PAGEDIR}/
    install -m 0644 ${UNPACKDIR}/upload.php ${D}${PAGEDIR}/
    install -m 0644 ${UNPACKDIR}/daten.php  ${D}${PAGEDIR}/

    # Datenverzeichnis und Verlinkung der Seiten zur Laufzeit herstellen.
    install -d ${D}${nonarch_libdir}/tmpfiles.d
    install -m 0644 ${UNPACKDIR}/myboard-web.tmpfiles \
        ${D}${nonarch_libdir}/tmpfiles.d/myboard-web.conf
}

FILES:${PN} = " \
    ${PAGEDIR} \
    ${nonarch_libdir}/tmpfiles.d/myboard-web.conf \
"
