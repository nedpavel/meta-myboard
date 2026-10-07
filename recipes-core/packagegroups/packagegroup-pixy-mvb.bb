SUMMARY = "MVB-Unterstuetzung fuer die Pixy-1000: Treiber und Messwerkzeuge"
DESCRIPTION = "Fasst die beiden nachgebauten Kernelmodule und die \
Diagnosewerkzeuge zusammen. Ohne Anwendung und ohne Bibliothek des \
Herstellers - gedacht fuer ein Image, mit dem am Bus gemessen wird."
LICENSE = "MIT"

inherit packagegroup

PACKAGES = "${PN} ${PN}-tests"

RDEPENDS:${PN} = "\
    kernel-module-pixy-mvb \
    kernel-module-pixy-mvblli \
    pixy-mvb-tools \
    "

# Die Offline-Pruefung der Treiberlogik. Getrennt, weil sie keine
# Hardware anfasst und auf dem Build-Host dasselbe Ergebnis liefert -
# im Image ist sie nur bequem, nicht noetig.
RDEPENDS:${PN}-tests = "pixy-mvb-tmreplay"
