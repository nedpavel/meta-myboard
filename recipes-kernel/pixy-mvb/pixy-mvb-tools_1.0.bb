SUMMARY = "Diagnose- und Messwerkzeuge fuer die Pixy-1000 MVB-Karte"
DESCRIPTION = "Python-Werkzeuge, die ueber die Zeichengeraete-ABI von \
pixy-mvb und pixy-mvblli arbeiten - ohne Bibliothek des Herstellers: \
Zustandsaufnahme der Interruptkette, Speicherabzug des Traffic Memory \
und ein Differenztest des Treibers gegen eine Referenzaufnahme."
LICENSE = "GPL-2.0-only"
LIC_FILES_CHKSUM = "file://${COMMON_LICENSE_DIR}/GPL-2.0-only;md5=801f80980d171dd6425610833a22dbe6"

# Die Skripte liegen bei den Treiberquellen, nicht in einem eigenen
# files/ - so gibt es sie nur einmal im Baum.
FILESEXTRAPATHS:prepend := "${THISDIR}/tools:"

SRC_URI = "file://irqstate.py \
           file://mvbdiff.py \
           file://mvbsnap.py \
           file://mvbirqtest.py \
          "

S = "${UNPACKDIR}"

do_install() {
    install -d ${D}${bindir}
    for t in irqstate mvbdiff mvbsnap mvbirqtest; do
        install -m 0755 ${S}/$t.py ${D}${bindir}/$t.py
    done
}

# Reine Python-Skripte, nichts zu uebersetzen.
INHIBIT_DEFAULT_DEPS = "1"

# Nicht allarch, obwohl der Inhalt es waere: die empfohlenen
# Kernelmodule sind maschinenabhaengig, und ein allarch-Paket mit
# solchen Abhaengigkeiten faellt in der QA durch.
PACKAGE_ARCH = "${MACHINE_ARCH}"

# ctypes difflib fcntl glob mmap os struct sys time - aus den Importen der
# vier Skripte. Fehlt zur Laufzeit doch ein Modul, deckt das Metapaket
# "python3" alles ab; das kostet im Image aber deutlich mehr Platz.
RDEPENDS:${PN} = "python3-core \
                  python3-ctypes \
                  python3-difflib \
                  python3-fcntl \
                  python3-mmap \
                  python3-shell \
                 "

# Die Werkzeuge greifen auf /dev/mvb0 und /dev/mvblli0 zu.
RRECOMMENDS:${PN} = "kernel-module-pixy-mvb kernel-module-pixy-mvblli"
