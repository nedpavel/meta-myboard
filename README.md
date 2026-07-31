# meta-myboard

Custom Yocto/OpenEmbedded layer für das **MyBoard** — ein Bahn-Bediendisplay auf
Basis eines **Intel Atom E3845 (Bay Trail)**, Yocto-Maschine `intel-corei7-64`
(64-bit x86-64; die CPU ist Bay-Trail-64-bit-fähig).

Enthält:

- **`recipes-gui/myboard-gui`** — Qt6-Widgets-Kiosk-GUI (`myboard-gui`), Vollbild
  auf einem 640×480-Panel unter „bare X" (ohne Window-Manager, fbdev-Treiber).
  Inkl. USB-Stick-App-Update (Taste 5 → „Update App").
- **`conf/machine`** — Maschinendefinition.
- **`recipes-extended/images/myboard-image.bb`** — das Ziel-Image (mit tzdata,
  Zeitzone Europe/Zurich, NTP-Sync, NFS-Dev-Mount, CAN, xorg-conf).
- **`recipes-kernel/linux`** — Kernel-Config-Fragmente (u.a. CAN/SJA1000, NFS,
  HID).
- **`recipes-graphics/xorg-conf`** — fbdev-X-Konfiguration inkl. deaktiviertem
  Bildschirm-Blanking/DPMS.
- **`recipes-connectivity`, `recipes-core`, `recipes-extended`** — NFS-Dev-Share,
  Zeit-Sync, CAN-Netzwerk, modprobe/modules-load, psplash, udev-Regeln.

## Aufbau der Build-Umgebung

Dieser Layer wird **allein** versioniert. Die (großen) Upstream-Layer werden
separat geklont und auf feste Revisionen gepinnt — siehe **[SETUP.md](SETUP.md)**.

## Schnellstart (wenn die Umgebung schon steht)

```bash
cd ~/Embedded
source openembedded-core/oe-init-build-env build
bitbake myboard-image          # volles Image
bitbake myboard-gui            # nur die GUI (für NFS-Deploy)
```
