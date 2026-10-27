SUMMARY = "LAMP-Konfiguration des Pixy 1000: Apache, MariaDB, PHP, phpMyAdmin"
DESCRIPTION = "Bindet die Teile des LAMP-Stacks zusammen: Apache mit PHP \
ueber php-fpm und Web-Wurzel /var/www/localhost/html, MariaDB mit der \
Datenbank myboard (Tabelle werte fuer streamd) und den Benutzern streamd, \
nobody (PHP) und admin (Netz, phpMyAdmin)."
LICENSE = "MIT"
LIC_FILES_CHKSUM = "file://${COMMON_LICENSE_DIR}/MIT;md5=0835ade698e0bcf8506ecda2f7b4f302"

SRC_URI = " \
    file://00-myboard.conf \
    file://myboard.cnf \
    file://db-setup.sql \
    file://myboard-db-setup.service \
    file://myboard-lamp.tmpfiles \
"
S = "${UNPACKDIR}"

inherit allarch systemd

SYSTEMD_SERVICE:${PN} = "myboard-db-setup.service"
SYSTEMD_AUTO_ENABLE = "enable"

do_install() {
    install -d ${D}${sysconfdir}/apache2/conf.d
    install -m 0644 ${UNPACKDIR}/00-myboard.conf ${D}${sysconfdir}/apache2/conf.d/

    # ueberschreibt die Vorgaben von /etc/my.cnf (meta-oe): Netz, InnoDB
    install -d ${D}${sysconfdir}/my.cnf.d
    install -m 0644 ${UNPACKDIR}/myboard.cnf ${D}${sysconfdir}/my.cnf.d/

    install -d ${D}${datadir}/myboard-lamp
    install -m 0644 ${UNPACKDIR}/db-setup.sql ${D}${datadir}/myboard-lamp/

    install -d ${D}${systemd_system_unitdir}
    install -m 0644 ${UNPACKDIR}/myboard-db-setup.service ${D}${systemd_system_unitdir}/

    install -d ${D}${nonarch_libdir}/tmpfiles.d
    install -m 0644 ${UNPACKDIR}/myboard-lamp.tmpfiles \
        ${D}${nonarch_libdir}/tmpfiles.d/myboard-lamp.conf
}

FILES:${PN} = " \
    ${sysconfdir}/apache2/conf.d/00-myboard.conf \
    ${sysconfdir}/my.cnf.d/myboard.cnf \
    ${datadir}/myboard-lamp \
    ${systemd_system_unitdir}/myboard-db-setup.service \
    ${nonarch_libdir}/tmpfiles.d/myboard-lamp.conf \
"

# mariadb-setupdb legt beim ersten Start die Grunddatenbank an
# (install_db.service), mariadb-client bringt 'mariadb' fuer die Einrichtung
RDEPENDS:${PN} = "apache2 php-fpm mariadb-server mariadb-client mariadb-setupdb \
                  phpmyadmin"
