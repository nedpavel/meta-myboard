SUMMARY = "CAN network configuration"
LICENSE = "MIT"
LIC_FILES_CHKSUM = "file://${COMMON_LICENSE_DIR}/MIT;md5=0835ade698e0bcf8506ecda2f7b4f302"

SRC_URI = " \
    file://can0.network \
    file://10-eth0.network \
"
S = "${UNPACKDIR}"


do_install() {
    install -d ${D}${sysconfdir}/systemd/network
    install -m 0644 ${S}/can0.network      ${D}${sysconfdir}/systemd/network/
    install -m 0644 ${S}/10-eth0.network   ${D}${sysconfdir}/systemd/network/
}

FILES:${PN} += " \
    ${sysconfdir}/systemd/network/can0.network \
    ${sysconfdir}/systemd/network/10-eth0.network \
"
