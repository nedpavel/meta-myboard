SUMMARY = "IDU2 am MVB: Lebenszeichen ab dem Start, DDS-Abfrage an DIA1"
DESCRIPTION = "Haelt das Display als IDU2 (Geraet 240) am MVB, zaehlt sein \
Lebenszeichen auf Port 491 im Sekundentakt hoch und fuehrt auf Anfrage der \
Bedienoberflaeche (Unix-Socket /run/idu2-mvb.sock) die DDS-Abfrage an DIA1 \
aus. Eine udev-Regel startet den Dienst, sobald /dev/mvblli0 da ist."
LICENSE = "MIT"
LIC_FILES_CHKSUM = "file://${COMMON_LICENSE_DIR}/MIT;md5=0835ade698e0bcf8506ecda2f7b4f302"

SRC_URI = "file://idu2-mvb.py \
           file://idu2-mvb.service \
           file://42-idu2-mvb.rules \
           file://idu2-mvb.default \
          "

S = "${UNPACKDIR}"

inherit allarch

do_install() {
    install -d ${D}${bindir}
    install -m 0755 ${S}/idu2-mvb.py ${D}${bindir}/idu2-mvb.py

    # Kein Enable ueber systemd: gestartet wird ueber die Geraete-Unit,
    # die 42-idu2-mvb.rules fuer /dev/mvblli0 anlegt
    install -d ${D}${systemd_system_unitdir}
    install -m 0644 ${S}/idu2-mvb.service \
        ${D}${systemd_system_unitdir}/idu2-mvb.service

    install -d ${D}${nonarch_base_libdir}/udev/rules.d
    install -m 0644 ${S}/42-idu2-mvb.rules \
        ${D}${nonarch_base_libdir}/udev/rules.d/42-idu2-mvb.rules

    install -d ${D}${sysconfdir}/default
    install -m 0644 ${S}/idu2-mvb.default ${D}${sysconfdir}/default/idu2-mvb
}

FILES:${PN} += "${systemd_system_unitdir}/idu2-mvb.service \
                ${nonarch_base_libdir}/udev/rules.d/42-idu2-mvb.rules \
               "

CONFFILES:${PN} = "${sysconfdir}/default/idu2-mvb"

# errno fcntl json os select signal socket struct sys time; socket liegt in
# python3-io, cp850 fuer die Klartexte der DDS-Meldungen in python3-codecs
RDEPENDS:${PN} = "python3-core python3-fcntl python3-io python3-json \
                  python3-codecs"
