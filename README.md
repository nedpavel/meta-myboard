# meta-myboard

Custom Yocto/OpenEmbedded layer für das **MyBoard** — ein Bahn-Bediendisplay auf
Basis eines **Intel Atom E3845 (Bay Trail)**, Yocto-Maschine `intel-corei7-64`
(64-bit x86-64; die CPU ist Bay-Trail-64-bit-fähig).

Enthält:

- **`recipes-gui/myboard-gui`** — Qt6-Widgets-Kiosk-GUI (`myboard-gui`), Vollbild
  auf einem 640×480-Panel unter „bare X" (ohne Window-Manager, fbdev-Treiber).
  Inkl. USB-Stick-App-Update (Taste 5 → „Update App").
- **`recipes-gui/myboard-webgui`** — dieselbe Oberfläche als Web-App unter
  `http://<pixy>/gui/` (Firefox-Kiosk am Gerät oder Browser im Netz), mit dem
  Hintergrund-Dienst `myboard-webgui-api`. Umschalten mit
  `oberflaeche web|qt` — siehe [dort](recipes-gui/myboard-webgui/README.md).
- **`recipes-graphics/browser-kiosk`, `recipes-browser/firefox`** — Firefox ESR
  im Kiosk-Modus (`browser-start <URL>`).
- **`recipes-httpd/myboard-lamp`** — LAMP: Apache 2.4 (Port 80) mit PHP über
  php-fpm, MariaDB 11.4 (Port 3306 auch übers Netz, InnoDB), phpMyAdmin unter
  `http://<pixy>/phpmyadmin/`, Perl. Legt die Datenbank `myboard` an.
  Anmeldung übers Netz und in phpMyAdmin als `admin` / `myboard` —
  **nach der Inbetriebnahme ändern:**
  `mariadb -e "ALTER USER 'admin'@'%' IDENTIFIED BY '<neu>';"` (am Gerät als
  root). `streamd` und PHP (als `nobody`) melden sich ohne Passwort über den
  Unix-Socket an, übers Netz geht das nicht.
- **`recipes-httpd/streamd`, `recipes-httpd/myboard-web`** — Datenstrom auf
  TCP-Port 9100 → MariaDB-Tabelle `myboard.werte` (60 Transaktionen/s,
  höchstens 100 000 Zeilen), Anzeige unter `http://<pixy>/`.
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
