SUMMARY = "TCP receiver for continuous data streams"
DESCRIPTION = "Nimmt einen dauerhaft offenen Datenstrom per TCP (Port 9100) \
zeilenweise entgegen und legt ihn fuer die Anzeige ab. Ergaenzt upload.php: \
ueber PHP-FPM laesst sich eine stehende Verbindung nicht verarbeiten, weil \
FPM einen Request erst vollstaendig entgegennimmt."
DEPENDS = "sqlite3"

LICENSE = "MIT"
LIC_FILES_CHKSUM = "file://${COMMON_LICENSE_DIR}/MIT;md5=0835ade698e0bcf8506ecda2f7b4f302"

SRC_URI = " \
    file://streamd.c \
    file://streamd.service \
"
S = "${UNPACKDIR}"

inherit systemd

SYSTEMD_SERVICE:${PN} = "streamd.service"
SYSTEMD_AUTO_ENABLE = "enable"

# Schreibt in dasselbe Datenverzeichnis wie upload.php
RDEPENDS:${PN} += "myboard-web"

do_compile() {
    ${CC} ${CFLAGS} ${LDFLAGS} -O2 -Wall -Wextra \
        ${UNPACKDIR}/streamd.c -o ${B}/streamd -lsqlite3
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
