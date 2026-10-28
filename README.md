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
- **`recipes-extended/idu2-mvb`** — das Display am MVB, als IDU2 (Vorgabe) oder
  mit `--rolle IDU1` als IDU1: Lebenszeichen, Prozessdaten, DDS-Abfrage an
  DIA1. Optionen in `OPTIONEN` (`/etc/default/idu2-mvb`).
  - Prozessdaten: Die Sink-Ports der Rolle laut Busbeschreibung werden im
    Takt ihrer Zykluszeit gelesen (kein Interrupt, Polling, rund 300
    Lesungen/s) und jede Lesung je Signal in `myboard.mvb_prozessdaten`
    geschrieben — mit Beschreibung, Datentyp (bool, uint8/16/32, int16,
    float, datetime, string), `gelesen` und `geaendert`. Die Tabelle ist
    eine MEMORY-Tabelle (schont die CFast, Inhalt nach Neustart neu
    angelegt). Die Signalliste entsteht beim Bauen aus
    `~/Embedded/private/mvb-trak.txt` (siehe [SETUP.md](SETUP.md)).
  - Softkeys: Seite 7 der Qt- und der Web-Oberfläche (Taste `7`) schickt die
    Befehle aus Port 492 (Türen-Test, Visiteur, Licht/HLK, Klima-Prüflauf)
    und die Lüfterbefehle als Puls von 1 s; die Liste liefert idu2-mvb
    (`softkeys`/`softkey` über den Socket), dazu die Rückmeldungen aus
    Port 290.
  - Source-Ports: alle drei der Rolle (IDU2 491/492/498, IDU1 481/488/489),
    gesendet im Takt ihrer Zykluszeit und nach jeder Änderung, jedes Senden
    ebenfalls in `mvb_prozessdaten`. Werte: Vorgabe 0 (Befehle nicht
    gedrückt), fest in `/etc/idu2-mvb/quellen.conf`, zur Laufzeit über den
    Socket
    (`{"befehl": "setzen", "signal": ..., "wert": ..., "dauer": s}`).
  - DDS: Jede Seite landet zusätzlich in `myboard.dds_meldungen` (Stand in
    `myboard.dds_stand`): neuer Cache_index von DIA1 → Inhalt ersetzt,
    Blättern → Seiten kommen dazu.
  - `--ohne-pd` / `--ohne-db` schalten Prozessdaten / alle
    Datenbankeinträge ab.
- **`recipes-extended/myboard-tasten`** — Hardkeys der Front (32 Tasten,
  Belegung laut INC-100 FW User Manual 5.11): je Druck eine Zeile in
  `myboard.tasten` mit Beschriftung, Position P41,n, Zeit und Haltedauer.
  Liest die Tastatur direkt vom Kernel, die Oberflächen bekommen die Tasten
  weiterhin. Ansehen z. B. mit
  `mariadb -e "SELECT gedrueckt, taste, position, dauer_ms FROM myboard.tasten ORDER BY id DESC LIMIT 20"`.
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
