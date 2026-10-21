# Firefox fuer browser-kiosk: nur das, was der Kiosk braucht.
#
# - x11-only statt wayland: der Kiosk laeuft unter Xorg mit
#   matchbox-window-manager (browser-xinitrc). "wayland" schaltet das Rezept
#   selbst ein, sobald die Distribution Wayland kennt; ohne :remove bliebe
#   das Wayland-Toolkit aktiv.
# - ohne webrtc: keine Video-/Audiotelefonie im Kiosk, spart Bauzeit.
# - disable-sandboxed-libraries: sonst wird wasi-sdk (eigener Clang fuer
#   WebAssembly) mitgebaut - Stunden auf dem Build-Rechner. Dafuer fehlt
#   die zusaetzliche Sandbox fuer einige Bibliotheken; fuer die lokalen,
#   eigenen Seiten des Kiosks vertretbar.
PACKAGECONFIG:remove = "wayland webrtc"
PACKAGECONFIG:append = " x11-only disable-sandboxed-libraries"

# mach nimmt ohne Angabe alle Kerne und beachtet PARALLEL_MAKE nicht. Auf
# dem Build-Rechner (4 Kerne, 15 GB RAM) bleibt mit 3 Jobs Luft fuer den
# Speicher der Rust- und Link-Schritte.
FIREFOX_JOBS ?= "3"
do_compile() {
    oe_cargo_fix_env
    mozilla_run_mach build -j ${FIREFOX_JOBS}
}
