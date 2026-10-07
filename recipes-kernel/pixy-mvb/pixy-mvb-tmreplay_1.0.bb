SUMMARY = "Offline-Pruefung der MVB-Treiberlogik (tm_replay)"
DESCRIPTION = "Uebersetzt pixy-mvblli.c unveraendert im Userspace und laesst \
die Treiberfunktionen gegen einen echten Abzug des Traffic Memory laufen. \
Braucht weder Kernel noch Hardware und prueft nur Logik - PCI, MSI, \
Fenstererkennung und der MVBC-Anlauf bleiben aussen vor. Nuetzlich als \
schneller Nachweis, dass ein Bau in Ordnung ist."
LICENSE = "GPL-2.0-only"
LIC_FILES_CHKSUM = "file://${COMMON_LICENSE_DIR}/GPL-2.0-only;md5=801f80980d171dd6425610833a22dbe6"

# Das Abspielwerk liegt in tests/, die Treiberquelle beim LLI-Rezept.
# Beides bleibt dort, wo es hingehoert; hier wird nur darauf gezeigt.
FILESEXTRAPATHS:prepend := "${THISDIR}/../../tests:${THISDIR}/../pixy-mvblli/files:"

SRC_URI = "file://tm_replay.c \
           file://fakelinux \
           file://mvbsnap \
           file://pixy-mvblli.c \
           file://pixy-mvblli.h \
           file://mvbc-regs.h \
           file://pixy-mvb.h \
          "

S = "${UNPACKDIR}"

# Der Treiber waehlt an drei Stellen per LINUX_VERSION_CODE zwischen der
# Schreibweise fuer 5.10 und der fuer 6.x. Gebaut werden beide Staende,
# damit auf dem Zielsystem nachpruefbar ist, dass sie dieselben
# Pruefungen bestehen.
do_compile() {
    ${CC} ${CFLAGS} ${LDFLAGS} -I ${S}/fakelinux -I ${S} \
        -o tm_replay ${S}/tm_replay.c
    ${CC} ${CFLAGS} ${LDFLAGS} -I ${S}/fakelinux -I ${S} \
        -DFAKE_KERNEL_CODE='KERNEL_VERSION(6,12,0)' \
        -o tm_replay-6x ${S}/tm_replay.c
}

do_install() {
    install -d ${D}${bindir}
    install -m 0755 tm_replay ${D}${bindir}/tm_replay
    install -m 0755 tm_replay-6x ${D}${bindir}/tm_replay-6x

    # Der Abzug ist die Eingabe des Tests - ohne ihn laeuft er nicht.
    install -d ${D}${datadir}/pixy-mvb/mvbsnap
    install -m 0644 ${S}/mvbsnap/tm_low.bin ${S}/mvbsnap/tm_high.bin \
        ${D}${datadir}/pixy-mvb/mvbsnap/
    install -m 0644 ${S}/mvbsnap/ports.txt ${D}${datadir}/pixy-mvb/mvbsnap/
}

FILES:${PN} += "${datadir}/pixy-mvb/mvbsnap"

# Aufruf auf dem Zielsystem:
#   tm_replay    /usr/share/pixy-mvb/mvbsnap
#   tm_replay-6x /usr/share/pixy-mvb/mvbsnap
# Beide muessen dieselbe Zahl Pruefungen ohne Fehler melden.
