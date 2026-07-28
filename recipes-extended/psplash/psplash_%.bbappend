FILESEXTRAPATHS:prepend := "${THISDIR}/files:"

SPLASH_IMAGES = "file://psplash-poky-img.png;outsuffix=default"

SRC_URI:append = " file://psplash-poky-img.png"
