SUMMARY = "Installer-Skript: schreibt myboard-image vom USB-Stick auf die CFast"
DESCRIPTION = "Laeuft nur im Image myboard-installer (USB-Stick): sucht die \
CFast, zeigt 10 s Countdown, schreibt das mitgelieferte System, prueft es \
und startet neu, sobald der Stick abgezogen ist. Das System selbst packt \
das Image-Rezept dazu."
LICENSE = "MIT"
LIC_FILES_CHKSUM = "file://${COMMON_LICENSE_DIR}/MIT;md5=0835ade698e0bcf8506ecda2f7b4f302"

SRC_URI = " \
    file://myboard-installieren \
    file://myboard-installieren.service \
"
S = "${UNPACKDIR}"

inherit allarch systemd

SYSTEMD_SERVICE:${PN} = "myboard-installieren.service"
SYSTEMD_AUTO_ENABLE = "enable"

do_install() {
    install -d ${D}${sbindir}
    install -m 0755 ${S}/myboard-installieren ${D}${sbindir}/

    install -d ${D}${systemd_system_unitdir}
    install -m 0644 ${S}/myboard-installieren.service ${D}${systemd_system_unitdir}/
}

RDEPENDS:${PN} = "bash coreutils gzip pv util-linux-sfdisk util-linux-blockdev"
