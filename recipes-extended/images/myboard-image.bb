require recipes-extended/images/core-image-full-cmdline.bb

# myboard-cfast.wks uses bootimg-pcbios with loader-bios=grub (legacy
# PC-BIOS GRUB, not grub-efi/UEFI). The wic-native sysroot needs the plain
# "grub" recipe's native grub-mkimage/grub-bios-setup, which isn't pulled
# in automatically by the machine's default WKS_FILE_DEPENDS.
WKS_FILE_DEPENDS:append = " grub-native grub"

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
    xorg-conf-myboard \
    "
IMAGE_INSTALL:append = " udev-rules-myboard"
IMAGE_INSTALL:append = " nfs-utils"
IMAGE_INSTALL:append = " nfs-devmount"
IMAGE_INSTALL:append = " timesync-myboard"
IMAGE_INSTALL:append = " tzdata"

# Board timezone = build host's zone (Europe/Zurich). NTP/timesyncd only syncs
# the UTC clock; the timezone is a separate static setting that controls how the
# local time (and thus the GUI clock) is displayed.
ROOTFS_POSTPROCESS_COMMAND += "set_board_timezone;"
set_board_timezone() {
    ln -sf /usr/share/zoneinfo/Europe/Zurich ${IMAGE_ROOTFS}${sysconfdir}/localtime
    echo "Europe/Zurich" > ${IMAGE_ROOTFS}${sysconfdir}/timezone
}
IMAGE_INSTALL:append = " \	
	ethtool \
	iproute2 \
	iproute2-tc \
	iptables \
	can-utils \
	libftdi \
	alsa-utils \
	alsa-state \
	alsa-plugins \
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
	python3-can \
	watchdog \
	sja1000-modprobe \
    	can-modules-load \
    	can-network \
    	can-utils \
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
