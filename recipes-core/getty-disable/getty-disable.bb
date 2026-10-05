SUMMARY = "Disable getty on tty1 (reserved for the Qt kiosk GUI)"
DESCRIPTION = "Installiert eine systemd-Preset-Regel, die getty@tty1 \
deaktiviert. tty1 gehoert der Qt-Oberflaeche; liefen beide, beendete \
systemd myboard-gui per SIGTERM und der Bildschirm blieb schwarz."
LICENSE = "MIT"
LIC_FILES_CHKSUM = "file://${COMMON_LICENSE_DIR}/MIT;md5=0835ade698e0bcf8506ecda2f7b4f302"

SRC_URI = "file://10-no-getty-tty1.preset"
S = "${UNPACKDIR}"

do_install() {
    install -d ${D}${systemd_unitdir}/system-preset
    install -m 0644 ${UNPACKDIR}/10-no-getty-tty1.preset \
        ${D}${systemd_unitdir}/system-preset/10-no-getty-tty1.preset
}

FILES:${PN} = "${systemd_unitdir}/system-preset/10-no-getty-tty1.preset"
