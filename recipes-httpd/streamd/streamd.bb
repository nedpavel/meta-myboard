SUMMARY = "TCP receiver for continuous data streams"
DESCRIPTION = "Nimmt einen dauerhaft offenen Datenstrom per TCP (Port 9100) \
zeilenweise entgegen, legt den aktuellen Wert fuer die Anzeige ab und \
schreibt den Verlauf in MariaDB (Datenbank icn, Tabelle werte), \
60 Transaktionen pro Sekunde. Ergaenzt upload.php: ueber PHP-FPM laesst \
sich eine stehende Verbindung nicht verarbeiten, weil FPM einen Request \
erst vollstaendig entgegennimmt."
DEPENDS = "mariadb"

LICENSE = "MIT"
LIC_FILES_CHKSUM = "file://${COMMON_LICENSE_DIR}/MIT;md5=0835ade698e0bcf8506ecda2f7b4f302"

SRC_URI = " \
    file://streamd.c \
    file://streamd.service \
"
S = "${UNPACKDIR}"

inherit systemd pkgconfig useradd

SYSTEMD_SERVICE:${PN} = "streamd.service"
SYSTEMD_AUTO_ENABLE = "enable"

# Eigener Systembenutzer: Anmeldung an MariaDB ueber unix_socket als
# Datenbank-Benutzer streamd (angelegt von myboard-db-setup), kein root
USERADD_PACKAGES = "${PN}"
USERADD_PARAM:${PN} = "--system --no-create-home --home-dir /nonexistent \
                       --shell /bin/false --user-group streamd"

# Datenbank, Tabelle und Benutzer kommen aus myboard-lamp
RDEPENDS:${PN} += "myboard-lamp"

do_compile() {
    ${CC} ${CFLAGS} ${LDFLAGS} -O2 -Wall -Wextra \
        $(pkg-config --cflags libmariadb) \
        ${UNPACKDIR}/streamd.c -o ${B}/streamd \
        $(pkg-config --libs libmariadb)
}

do_install() {
    install -d ${D}${bindir}
    install -m 0755 ${B}/streamd ${D}${bindir}/streamd

    install -d ${D}${systemd_system_unitdir}
    install -m 0644 ${UNPACKDIR}/streamd.service \
        ${D}${systemd_system_unitdir}/streamd.service
}

FILES:${PN} = " \
    ${bindir}/streamd \
    ${systemd_system_unitdir}/streamd.service \
"
