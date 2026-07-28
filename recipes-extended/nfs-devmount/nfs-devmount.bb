SUMMARY = "NFS dev-share automount for faster development (/mnt/dev)"
DESCRIPTION = "Systemd automount that mounts the build host's NFS scratch share \
at /mnt/dev on first access, so freshly-built artifacts can be dropped onto the \
board without reflashing. Uses automount + nofail + soft so the board still boots \
normally when the cable/host is absent."
LICENSE = "MIT"
LIC_FILES_CHKSUM = "file://${COMMON_LICENSE_DIR}/MIT;md5=0835ade698e0bcf8506ecda2f7b4f302"

SRC_URI = " \
    file://mnt-dev.mount \
    file://mnt-dev.automount \
"
S = "${UNPACKDIR}"

inherit systemd

# Enable only the automount; the .mount is triggered on access to /mnt/dev.
SYSTEMD_SERVICE:${PN} = "mnt-dev.automount"
SYSTEMD_AUTO_ENABLE = "enable"

do_install() {
    install -d ${D}${systemd_system_unitdir}
    install -m 0644 ${S}/mnt-dev.mount     ${D}${systemd_system_unitdir}/mnt-dev.mount
    install -m 0644 ${S}/mnt-dev.automount ${D}${systemd_system_unitdir}/mnt-dev.automount

    # mount point
    install -d ${D}/mnt/dev
}

FILES:${PN} += " \
    ${systemd_system_unitdir}/mnt-dev.mount \
    ${systemd_system_unitdir}/mnt-dev.automount \
    /mnt/dev \
"
