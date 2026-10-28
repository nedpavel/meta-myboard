SUMMARY = "Hardkeys der Front des Pixy 1000 nach MariaDB"
DESCRIPTION = "Liest die 32 Tasten um den Bildschirm (USB-Tastatur des \
System-Controllers, Belegung laut INC-100 FW User Manual 5.11) direkt vom \
Kernel und schreibt je Druck eine Zeile nach myboard.tasten - mit \
Beschriftung, Position P41,n, Zeit und Haltedauer. Die Oberflaechen \
bekommen die Tasten weiterhin."
LICENSE = "MIT"
LIC_FILES_CHKSUM = "file://${COMMON_LICENSE_DIR}/MIT;md5=0835ade698e0bcf8506ecda2f7b4f302"

SRC_URI = " \
    file://myboard-tasten.py \
    file://myboard-tasten.service \
"
S = "${UNPACKDIR}"

inherit allarch systemd useradd

SYSTEMD_SERVICE:${PN} = "myboard-tasten.service"
SYSTEMD_AUTO_ENABLE = "enable"

# Eigener Systembenutzer wie bei streamd: Anmeldung an MariaDB ueber
# unix_socket als Datenbank-Benutzer tasten (angelegt von myboard-db-setup)
USERADD_PACKAGES = "${PN}"
USERADD_PARAM:${PN} = "--system --no-create-home --home-dir /nonexistent \
                       --shell /bin/false --user-group tasten"

do_install() {
    install -d ${D}${bindir}
    install -m 0755 ${S}/myboard-tasten.py ${D}${bindir}/myboard-tasten

    install -d ${D}${systemd_system_unitdir}
    install -m 0644 ${S}/myboard-tasten.service ${D}${systemd_system_unitdir}/
}

# Tabelle und Benutzer kommen aus myboard-lamp
RDEPENDS:${PN} = "python3-core python3-pymysql myboard-lamp"
