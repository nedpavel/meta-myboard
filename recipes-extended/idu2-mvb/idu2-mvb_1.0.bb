SUMMARY = "IDU am MVB: Lebenszeichen, Prozessdaten in MariaDB, DDS-Abfrage an DIA1"
DESCRIPTION = "Haelt das Display als IDU2 (Geraet 240, Vorgabe) oder IDU1 \
(Geraet 238) am MVB, zaehlt sein Lebenszeichen im Sekundentakt hoch, liest \
die Sink-Ports der Rolle und schreibt die Prozessdaten nach MariaDB, und \
fuehrt auf Anfrage der Bedienoberflaeche (Unix-Socket /run/idu2-mvb.sock) \
die DDS-Abfrage an DIA1 aus. Eine udev-Regel startet den Dienst, sobald \
/dev/mvblli0 da ist."
LICENSE = "MIT"
LIC_FILES_CHKSUM = "file://${COMMON_LICENSE_DIR}/MIT;md5=0835ade698e0bcf8506ecda2f7b4f302"

SRC_URI = "file://idu2-mvb.py \
           file://idu2-mvb.service \
           file://42-idu2-mvb.rules \
           file://idu2-mvb.default \
           file://quellen.conf \
           file://mvbtrak2tsv.py \
          "

S = "${UNPACKDIR}"

inherit allarch python3native

# Busbeschreibung des Fahrzeugs ("MVB (trak).txt"), aus der die
# Signalliste fuer die Prozessdaten entsteht. Sie ist ein Dokument des
# Kunden und liegt deshalb nicht im (oeffentlichen) Layer, sondern
# daneben; den Pfad setzt local.conf:
#   MVB_BUSBESCHREIBUNG = "${TOPDIR}/../private/mvb-trak.txt"
# Ohne die Datei laeuft idu2-mvb wie bisher, nur ohne Prozessdaten.
MVB_BUSBESCHREIBUNG ??= ""

# Neu bauen, wenn sich die Busbeschreibung aendert
python () {
    import os
    pfad = d.getVar("MVB_BUSBESCHREIBUNG")
    if pfad:
        d.appendVarFlag("do_compile", "file-checksums",
                        " %s:%s" % (pfad, os.path.exists(pfad)))
}

do_compile() {
    rm -f ${B}/mvb-signale.tsv
    if [ -z "${MVB_BUSBESCHREIBUNG}" ]; then
        bbwarn "MVB_BUSBESCHREIBUNG nicht gesetzt - idu2-mvb ohne Prozessdaten"
    elif [ ! -f "${MVB_BUSBESCHREIBUNG}" ]; then
        bbwarn "Busbeschreibung ${MVB_BUSBESCHREIBUNG} fehlt - idu2-mvb ohne Prozessdaten"
    else
        ${PYTHON} ${S}/mvbtrak2tsv.py "${MVB_BUSBESCHREIBUNG}" \
            > ${B}/mvb-signale.tsv
    fi
}

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

    # feste Werte der Source-Signale (Software-Version, Fahrzeugname ...)
    install -d ${D}${sysconfdir}/idu2-mvb
    install -m 0644 ${S}/quellen.conf ${D}${sysconfdir}/idu2-mvb/quellen.conf

    if [ -f ${B}/mvb-signale.tsv ]; then
        install -d ${D}${datadir}/idu2-mvb
        install -m 0644 ${B}/mvb-signale.tsv ${D}${datadir}/idu2-mvb/
    fi
}

FILES:${PN} += "${systemd_system_unitdir}/idu2-mvb.service \
                ${nonarch_base_libdir}/udev/rules.d/42-idu2-mvb.rules \
                ${datadir}/idu2-mvb \
               "

CONFFILES:${PN} = "${sysconfdir}/default/idu2-mvb \
                   ${sysconfdir}/idu2-mvb/quellen.conf"

# errno fcntl json os select signal socket struct sys time; socket liegt in
# python3-io, cp850 fuer die Klartexte der DDS-Meldungen in python3-codecs.
# PyMySQL, threading und datetime fuer DDS-Meldungen und Prozessdaten in
# MariaDB (Tabellen legt myboard-lamp an); fehlt MariaDB, laeuft der Dienst
# trotzdem.
RDEPENDS:${PN} = "python3-core python3-fcntl python3-io python3-json \
                  python3-codecs python3-threading python3-datetime \
                  python3-pymysql"
