SUMMARY = "systemd-timesyncd NTP config: sync clock from the build host"
DESCRIPTION = "The board has no RTC battery and no internet. It syncs its clock \
from the directly-connected build host (192.168.1.202) via systemd-timesyncd, \
which talks to a local chrony NTP server on the host over the dev cable."
LICENSE = "MIT"
LIC_FILES_CHKSUM = "file://${COMMON_LICENSE_DIR}/MIT;md5=0835ade698e0bcf8506ecda2f7b4f302"

SRC_URI = "file://10-myboard-ntp.conf"
S = "${UNPACKDIR}"

do_install() {
    install -d ${D}${sysconfdir}/systemd/timesyncd.conf.d
    install -m 0644 ${UNPACKDIR}/10-myboard-ntp.conf \
        ${D}${sysconfdir}/systemd/timesyncd.conf.d/10-myboard-ntp.conf
}

FILES:${PN} = "${sysconfdir}/systemd/timesyncd.conf.d/10-myboard-ntp.conf"

# The systemd-timesyncd daemon ships with the 'systemd' package (already in the
# image) and is enabled by the systemd preset; this recipe only adds the config
# drop-in, so no extra RDEPENDS is needed.
