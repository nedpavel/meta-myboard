SUMMARY = "MVB-Unterstuetzung fuer die Pixy-1000: Treiber und Messwerkzeuge"
DESCRIPTION = "Fasst die beiden nachgebauten Kernelmodule und die \
Diagnosewerkzeuge zusammen. Ohne Anwendung und ohne Bibliothek des \
Herstellers - gedacht fuer ein Image, mit dem am Bus gemessen wird."
LICENSE = "MIT"

inherit packagegroup

PACKAGES = "${PN} ${PN}-tests"

# kernel-module-* bringt nur die .ko (und fuer pixy-mvb den Eintrag in
# modules-load.d). Die udev-Regeln 40-mvb.rules und 41-mvblli.rules liegen
# in den Grundpaketen pixy-mvb und pixy-mvblli - ohne sie gibt es weder
# /dev/mvb und /dev/mvblli noch das automatische Laden von pixy_mvblli.
RDEPENDS:${PN} = "\
    kernel-module-pixy-mvb \
    kernel-module-pixy-mvblli \
    pixy-mvb \
    pixy-mvblli \
    pixy-mvb-tools \
    idu2-mvb \
    "

# Die Offline-Pruefung der Treiberlogik. Getrennt, weil sie keine
# Hardware anfasst und auf dem Build-Host dasselbe Ergebnis liefert -
# im Image ist sie nur bequem, nicht noetig.
RDEPENDS:${PN}-tests = "pixy-mvb-tmreplay"
