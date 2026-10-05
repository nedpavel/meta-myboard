require recipes-extended/images/core-image-full-cmdline.bb

# --- Boot: UEFI (Pixy 1000, Apollo Lake) -----------------------------------
# Das Pixy 1000 bootet 64-bit-UEFI; ein Legacy-PC-BIOS-Image wuerde dort
# nicht starten. Gebootet wird ueber den Fallback-Pfad EFI/BOOT/bootx64.efi,
# weil auf dem Geraet keine EFI-Variablen verfuegbar sind (efivarfs fehlt).
#
# Das alte INC-100 (Legacy-BIOS) wird auf dem Branch 'main' weitergepflegt
# und nutzt dort myboard-cfast.wks mit bootimg-pcbios.
WKS_FILE = "pixy1000-efi.wks"
EFI_PROVIDER = "grub-efi"
WKS_FILE_DEPENDS:append = " grub-efi"

#IMAGE_FSTYPES:append = " img.bz2"

#IMAGE_TYPEDEP:img = "wic"

#IMAGE_CMD:img () {
#	install -m 0644 "${IMGDEPLOYDIR}/${IMAGE_LINK_NAME}.wic" \
#			"${IMGDEPLOYDIR}/${IMAGE_LINK_NAME}.img"
#}


IMAGE_INSTALL:remove = "ovmf"
RRECOMMENDS:${PN}:remove = "ovmf"
RDEPENDS:${PN}:remove = "ovmf"

RRECOMMENDS:${PN}:remove:x86 = "ovmf"
RRECOMMENDS:${PN}:remove:i686 = "ovmf"

RDEPENDS:${PN}:remove:x86 = "ovmf"
RDEPENDS:${PN}:remove:i686 = "ovmf"


IMAGE_INSTALL:append = " kernel-modules"
IMAGE_INSTALL:append = " \
    xf86-video-fbdev \
    xf86-video-modesetting \
    xorg-conf-myboard \
    "
IMAGE_INSTALL:append = " udev-rules-myboard"
# MVB vorerst deaktiviert (Zielgeraet Pixy 1000 ohne MVB-Nutzung).
# Die Rezepte bleiben im Layer erhalten — zum Reaktivieren genuegt es,
# die folgende Zeile wieder einzukommentieren. Achtung: der Treiber
# pixymvbip ist auf die LPC/ISA-Anbindung des INC-100 ausgelegt; das
# Pixy 1000 bindet MVB ueber den PCIe-Interface-Converter an.
# IMAGE_INSTALL:append = " pixymvbip libmvb"
IMAGE_INSTALL:append = " nfs-utils"
IMAGE_INSTALL:append = " nfs-devmount"
IMAGE_INSTALL:append = " timesync-myboard"
IMAGE_INSTALL:append = " tzdata"
# Webserver (aus meta-openembedded/meta-webserver — die Layer muss in
# bblayers.conf eingetragen sein, siehe build-conf/bblayers.conf).
# php-fpm fuehrt die PHP-Seiten aus; ohne ihn liefert nginx sie als Text.
# myboard-web bringt die Seiten (Datenstrom-Empfang + Anzeige) mit.
IMAGE_INSTALL:append = " nginx php-fpm php-cli myboard-web"

# Browser-Betrieb: surf (WebKitGTK) im Vollbild anstelle der Qt-Oberflaeche,
# zu starten mit "browser-start". matchbox-wm wird gebraucht, weil ein
# GTK-Fenster ohne Window-Manager den Bildschirm nicht fuellt.
IMAGE_INSTALL:append = " surf matchbox-wm browser-kiosk"

# Board timezone = build host's zone (Europe/Zurich). NTP/timesyncd only syncs
# the UTC clock; the timezone is a separate static setting that controls how the
# local time (and thus the GUI clock) is displayed.
# Auf tty1 laeuft die Qt-Oberflaeche (Xorg :0 vt1). Lief dort zusaetzlich
# ein getty, beanspruchten zwei Dienste dieselbe Konsole und systemd
# beendete myboard-gui per SIGTERM (ExecMainStatus=15, Result=success,
# keine Neustarts) — der Bildschirm blieb schwarz, Xorg startete nie,
# und es entstand nicht einmal ein Xorg.0.log. Von Hand funktionierte
# derselbe Aufruf, weil dann kein Konflikt bestand.
# Dasselbe Problem gab es bereits beim INC-100.
ROOTFS_POSTPROCESS_COMMAND += "disable_getty_tty1;"
disable_getty_tty1() {
    rm -f ${IMAGE_ROOTFS}${sysconfdir}/systemd/system/getty.target.wants/getty@tty1.service
}

ROOTFS_POSTPROCESS_COMMAND += "set_board_timezone;"
set_board_timezone() {
    ln -sf /usr/share/zoneinfo/Europe/Zurich ${IMAGE_ROOTFS}${sysconfdir}/localtime
    echo "Europe/Zurich" > ${IMAGE_ROOTFS}${sysconfdir}/timezone
}
IMAGE_INSTALL:append = " \	
	ethtool \
	curl \
	iproute2 \
	iproute2-tc \
	iptables \
	libftdi \
	alsa-utils \
	alsa-state \
	alsa-plugins \
	# Touchscreen-Kalibrierung: unter X11 bedient libinput den PenMount,
	# NICHT tslib. tslib wirkt nur bei Anwendungen, die direkt auf den
	# Framebuffer gehen — ts_calibrate bliebe hier also wirkungslos.
	# xinput_calibrator ermittelt die Transformationsmatrix, die in
	# /etc/X11/xorg.conf.d/50-touchscreen.conf eingetragen wird.
	xinput-calibrator \
	tslib \
	tslib-calibrate \
	tslib-tests \
	systemd \
	systemd-analyze \
	systemd-extra-utils \
	udev \
	nano \
	e2fsprogs \
	e2fsprogs-e2fsck \
	e2fsprogs-mke2fs \
	util-linux \
	volatile-binds \
	i2c-tools \
	usbutils \
	pciutils \
	lsof \
	strace \
	gdb \
	python3 \
	python3-pyserial \
	watchdog \
	network-pixy1000 \
    	myboard-gui \
    	xserver-xorg \
    	xf86-input-evdev \
    	xf86-input-libinput \
    	xinit \
    	qtbase \
    	qtbase-plugins \
    	xcb-util-cursor \
    	xcb-util \
    	xcb-util-image \
    	xcb-util-keysyms \
    	xcb-util-renderutil \
    	xcb-util-wm \
    	libxcb \
    	libx11 \
    	libxext \
    	libxrender \
    	libxi \
    	libxkbcommon \
    	xserver-xorg-extension-glx \
    	xserver-xorg-extension-dri2 \
    	fontconfig \
    	ttf-dejavu-sans \
	libgl-mesa \
	libegl-mesa \
	mesa \
	mesa-megadriver \
	libgles2-mesa \
	libepoxy \
	"


SUMMARY = "MyBoard Image"
