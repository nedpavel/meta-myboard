# Konfiguration fuer das Pixy: MariaDB-Socket, Cookie-Anmeldung,
# Temp-Verzeichnis - siehe files/config.inc.php
FILESEXTRAPATHS:prepend := "${THISDIR}/files:"
SRC_URI += "file://config.inc.php"

do_install:append() {
    install -m 0644 ${UNPACKDIR}/config.inc.php ${D}${datadir}/${BPN}/config.inc.php
}
