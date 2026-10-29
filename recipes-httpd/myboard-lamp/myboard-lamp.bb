SUMMARY = "LAMP-Konfiguration des Pixy 1000: Apache, MariaDB, PHP, phpMyAdmin"
DESCRIPTION = "Bindet die Teile des LAMP-Stacks zusammen: Apache mit PHP \
ueber php-fpm und Web-Wurzel /var/www/localhost/html, MariaDB mit der \
Datenbank icn (Vorgabe des Applikationsingenieurs aus icn_setup.sql, dazu \
die Tabellen fuer DDS-Meldungen, Prozessdaten, Hardkeys und streamd) und \
den Benutzern icn, streamd, tasten, nobody (PHP) und admin (Netz); \
root mit Passwort SBB-ICN."
LICENSE = "MIT"
LIC_FILES_CHKSUM = "file://${COMMON_LICENSE_DIR}/MIT;md5=0835ade698e0bcf8506ecda2f7b4f302"

SRC_URI = " \
    file://00-myboard.conf \
    file://myboard.cnf \
    file://db-setup.sql \
    file://myboard-db-setup \
    file://myboard-db-setup.service \
    file://myboard-lamp.tmpfiles \
"
S = "${UNPACKDIR}"

inherit allarch systemd

SYSTEMD_SERVICE:${PN} = "myboard-db-setup.service"
SYSTEMD_AUTO_ENABLE = "enable"

# Datenbank icn des Applikationsingenieurs (icn_setup.sql). Sie enthaelt die
# Signale aus der Busbeschreibung des Kunden und liegt deshalb nicht im
# (oeffentlichen) Layer, sondern daneben; den Pfad setzt local.conf:
#   ICN_SETUP_SQL = "${TOPDIR}/../private/icn_setup.sql"
# Fehlt die Datei, legt db-setup.sql die Datenbank icn nur mit den eigenen
# Tabellen an.
ICN_SETUP_SQL ??= ""

# Neu bauen, wenn sich icn_setup.sql aendert
python () {
    import os
    pfad = d.getVar("ICN_SETUP_SQL")
    if pfad:
        d.appendVarFlag("do_install", "file-checksums",
                        " %s:%s" % (pfad, os.path.exists(pfad)))
}

do_install() {
    install -d ${D}${sysconfdir}/apache2/conf.d
    install -m 0644 ${UNPACKDIR}/00-myboard.conf ${D}${sysconfdir}/apache2/conf.d/

    # ueberschreibt die Vorgaben von /etc/my.cnf (meta-oe): Netz, InnoDB
    install -d ${D}${sysconfdir}/my.cnf.d
    install -m 0644 ${UNPACKDIR}/myboard.cnf ${D}${sysconfdir}/my.cnf.d/

    install -d ${D}${datadir}/myboard-lamp
    install -m 0644 ${UNPACKDIR}/db-setup.sql ${D}${datadir}/myboard-lamp/
    if [ -z "${ICN_SETUP_SQL}" ]; then
        bbwarn "ICN_SETUP_SQL nicht gesetzt - Datenbank icn ohne die Tabellen des Applikationsingenieurs"
    elif [ ! -f "${ICN_SETUP_SQL}" ]; then
        bbwarn "icn_setup.sql ${ICN_SETUP_SQL} fehlt - Datenbank icn ohne die Tabellen des Applikationsingenieurs"
    else
        install -m 0644 "${ICN_SETUP_SQL}" ${D}${datadir}/myboard-lamp/icn_setup.sql
    fi

    install -d ${D}${libexecdir}
    install -m 0755 ${UNPACKDIR}/myboard-db-setup ${D}${libexecdir}/

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
    ${libexecdir}/myboard-db-setup \
    ${systemd_system_unitdir}/myboard-db-setup.service \
    ${nonarch_libdir}/tmpfiles.d/myboard-lamp.conf \
"

# mariadb-setupdb legt beim ersten Start die Grunddatenbank an
# (install_db.service), mariadb-client bringt 'mariadb' fuer die Einrichtung
RDEPENDS:${PN} = "apache2 php-fpm mariadb-server mariadb-client mariadb-setupdb \
                  phpmyadmin"
