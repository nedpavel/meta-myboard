SUMMARY = "USB-Stick, der myboard-image auf die CFast des Pixy 1000 schreibt"
DESCRIPTION = "Bootet per UEFI vom USB-Stick, zeigt 10 s Countdown (jede \
Taste bricht ab), schreibt das mitgelieferte myboard-image auf die CFast, \
prueft es per Pruefsumme und startet neu, sobald der Stick abgezogen ist. \
Bauen: bitbake myboard-installer; das .wic.gz kommt auf den Stick wie bisher \
das von myboard-image."
LICENSE = "MIT"

inherit core-image

IMAGE_INSTALL = "packagegroup-core-boot myboard-installieren"
IMAGE_LINGUAS = ""

# local.conf haengt ssh-server und package-management an jedes Image; der
# Installer braucht nur die Konsole (Anmeldung als root auf tty2 fuer die
# Fehlersuche). Nur lesbar: der Stick darf am Ende einfach abgezogen werden.
EXTRA_IMAGE_FEATURES = "allow-empty-password empty-root-password allow-root-login"
IMAGE_FEATURES += "read-only-rootfs"

IMAGE_FSTYPES = "wic.gz wic.bmap"
IMAGE_FSTYPES:remove = "ext4"

WKS_FILE = "pixy1000-installer.wks"
EFI_PROVIDER = "grub-efi"
WKS_FILE_DEPENDS:append = " grub-efi"

# wie in myboard-image
IMAGE_INSTALL:remove = "ovmf"
RRECOMMENDS:${PN}:remove = "ovmf"
RDEPENDS:${PN}:remove = "ovmf"

# Das System, das auf die CFast kommt: das fertige Image aus
# DEPLOY_DIR_IMAGE, dazu Groesse und Pruefsumme des entpackten Images fuer
# die Kontrolle nach dem Schreiben. Die Abhaengigkeit auf do_image_complete
# geht in die Signatur von do_rootfs ein: aendert sich das System, wird
# auch der Installer neu gebaut.
INSTALLER_SYSTEM ?= "myboard-image"
do_rootfs[depends] += "${INSTALLER_SYSTEM}:do_image_complete"
ROOTFS_POSTPROCESS_COMMAND += "installer_system_einpacken"

installer_system_einpacken () {
    bild=${DEPLOY_DIR_IMAGE}/${INSTALLER_SYSTEM}-${MACHINE}.rootfs.wic.gz
    ziel=${IMAGE_ROOTFS}${datadir}/myboard-installer
    install -d $ziel
    install -m 0644 "$bild" $ziel/myboard-image.wic.gz
    gzip -dc "$bild" | sha256sum | cut -d' ' -f1 > $ziel/myboard-image.sha256
    gzip -dc "$bild" | wc -c > $ziel/myboard-image.groesse
    basename "$(readlink -f "$bild")" .wic.gz > $ziel/myboard-image.version
}
