SUMMARY = "MyBoard Qt Widgets Kiosk GUI"
DESCRIPTION = "Vollbild Qt-Widgets-Anwendung als Bedienoberflaeche"
LICENSE = "MIT"
LIC_FILES_CHKSUM = "file://${COMMON_LICENSE_DIR}/MIT;md5=0835ade698e0bcf8506ecda2f7b4f302"

# Sucht Dateien auch im files/ Unterverzeichnis des Rezepts
FILESEXTRAPATHS:prepend := "${THISDIR}/files:"

# Quellen liegen lokal im Layer unter files/
SRC_URI = " \
    file://src/main.cpp \
    file://src/mainwindow.cpp \
    file://src/mainwindow.h \
    file://src/canmatrix.cpp \
    file://src/canmatrix.h \
    file://src/canreader.cpp \
    file://src/canreader.h \
    file://src/canopen.cpp \
    file://src/canopen.h \
    file://src/CMakeLists.txt \
    file://src/images.qrc \
    file://src/can.qrc \
    file://src/can_matrix.tsv \
    file://src/ICN_Office.jpg \
    file://src/ICN_Office_2.jpg \
    file://myboard-gui.service \
    file://xinitrc \
"

# Qt6 build braucht diese Klassen
inherit qt6-cmake systemd

# Quellverzeichnis: CMakeLists.txt liegt unter src/
S = "${UNPACKDIR}/src"

DEPENDS = "qtbase"

# systemd-Service registrieren
SYSTEMD_SERVICE:${PN} = "myboard-gui.service"
SYSTEMD_AUTO_ENABLE = "enable"

do_install:append() {
    # xinitrc installieren — startet die App im X-Server
    install -d ${D}${sysconfdir}
    install -m 0755 ${UNPACKDIR}/xinitrc ${D}${sysconfdir}/xinitrc

    # systemd-Service installieren
    install -d ${D}${systemd_system_unitdir}
    install -m 0644 ${UNPACKDIR}/myboard-gui.service \
        ${D}${systemd_system_unitdir}/myboard-gui.service
}

FILES:${PN} += " \
    ${bindir}/myboard-gui \
    ${sysconfdir}/xinitrc \
    ${systemd_system_unitdir}/myboard-gui.service \
"
