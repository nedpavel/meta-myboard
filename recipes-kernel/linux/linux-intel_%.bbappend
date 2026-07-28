# This kernel's scripts/kconfig/mconf-cfg.sh (and nconf-cfg.sh) reference
# $CROSS_CURSES_LIB under `set -eu` with no default, but kernel.bbclass never
# exports it. That aborts the ncurses detection before it ever runs
# pkg-config or the header fallback, so `mconf`/`nconf` link with no curses
# library at all -- "undefined reference to `wrefresh'" in do_menuconfig.
EXTRA_OEMAKE:append = ' CROSS_CURSES_LIB=""'

# Board-specific kernel config fragments, kept as permanent files rather
# than interactive menuconfig changes so they survive `bitbake -c clean`.
FILESEXTRAPATHS:prepend := "${THISDIR}/${PN}:"

SRC_URI:append:intel-core2-32 = " \
    file://mydevice.cfg \
    file://can-sja1000.cfg \
    file://ftdi-serial.cfg \
    file://cfast-sata.cfg \
    file://audio-alc662.cfg \
    file://touch-penmount.cfg \
    file://network-i210.cfg \
    "
