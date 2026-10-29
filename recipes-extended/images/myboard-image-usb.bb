require recipes-extended/images/myboard-image.bb

SUMMARY = "myboard-image zum Starten vom USB-Stick, ohne Installation"
DESCRIPTION = "Derselbe Inhalt wie myboard-image, aber mit Partitionen ueber \
UUIDs (pixy1000-usb.wks): vom Stick gestartet laeuft alles vom Stick, auch \
wenn auf der CFast ein System steht. Zum Installieren auf die CFast: \
myboard-installer."

WKS_FILE = "pixy1000-usb.wks"

# Nur das Stick-Image; das ext4 aus local.conf braucht es hier nicht
IMAGE_FSTYPES:remove = "ext4"
IMAGE_FSTYPES:append = " wic.bmap"
