SUMMARY = "Network configuration for the Pixy 1000 (two Intel I225-IT ports)"
DESCRIPTION = "systemd-networkd-Konfiguration fuer die beiden Ethernet-Ports. \
Gematcht wird ueber den PCI-Pfad, nicht ueber den Interface-Namen — auf dem \
Pixy 1000 gibt es kein 'eth0', systemd vergibt vorhersagbare Namen. \
X2 (01:00.0) = Betriebsnetz, X3 (02:00.0) = Entwicklung/NFS."
LICENSE = "MIT"
LIC_FILES_CHKSUM = "file://${COMMON_LICENSE_DIR}/MIT;md5=0835ade698e0bcf8506ecda2f7b4f302"

SRC_URI = " \
    file://10-betriebsnetz.network \
    file://20-devnet.network \
"
S = "${UNPACKDIR}"

do_install() {
    install -d ${D}${sysconfdir}/systemd/network
    install -m 0644 ${S}/10-betriebsnetz.network ${D}${sysconfdir}/systemd/network/
    install -m 0644 ${S}/20-devnet.network       ${D}${sysconfdir}/systemd/network/
}

FILES:${PN} += " \
    ${sysconfdir}/systemd/network/10-betriebsnetz.network \
    ${sysconfdir}/systemd/network/20-devnet.network \
"
