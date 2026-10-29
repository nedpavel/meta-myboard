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
  `http://<pixy>/phpmyadmin/`, Perl. Alle Daten liegen in der Datenbank
  `icn`: die Vorgabe des Applikationsingenieurs (`icn_setup.sql` mit
  `icn_datafields`, `icn_menu`, `icn_dp_colors`, `icn_internal_var` und
  Benutzer `icn` / `SBB-ICN`; liegt wie die Busbeschreibung in
  `~/Embedded/private/`, siehe [SETUP.md](SETUP.md)) und unsere Tabellen
  `dds_meldungen`, `dds_stand`, `mvb_prozessdaten`, `tasten`, `werte`.
  `root` hat das Passwort `SBB-ICN` (am Gerät selbst geht root weiter ohne
  Passwort über den Unix-Socket). Übers Netz als `admin` / `myboard` —
  **nach der Inbetriebnahme ändern:**
  `mariadb -e "ALTER USER 'admin'@'%' IDENTIFIED BY '<neu>';"` (am Gerät als
  root). `streamd`, `tasten` und PHP (als `nobody`) melden sich ohne
  Passwort über den Unix-Socket an, übers Netz geht das nicht.
- **`recipes-httpd/streamd`, `recipes-httpd/myboard-web`** — Datenstrom auf
  TCP-Port 9100 → MariaDB-Tabelle `icn.werte` (60 Transaktionen/s,
  höchstens 100 000 Zeilen), Anzeige unter `http://<pixy>/`.
- **`recipes-extended/idu2-mvb`** — das Display am MVB, als IDU2 (Vorgabe) oder
  mit `--rolle IDU1` als IDU1: Lebenszeichen, Prozessdaten, DDS-Abfrage an
  DIA1. Optionen in `OPTIONEN` (`/etc/default/idu2-mvb`).
  - Prozessdaten: Die Sink-Ports der Rolle laut Busbeschreibung werden im
    Takt ihrer Zykluszeit gelesen (kein Interrupt, Polling, rund 300
    Lesungen/s) und jede Lesung je Signal in `icn.mvb_prozessdaten`
    geschrieben — mit Beschreibung, Datentyp (bool, uint8/16/32, int16,
    float, datetime, string), `gelesen` und `geaendert`; Texte zusätzlich je
    Byte als `<Name>_ArrByteN` wie in `icn_datafields`, mit der sie sich über
    den Signalnamen verknüpfen lassen. Die Tabelle ist
    eine MEMORY-Tabelle (schont die CFast, Inhalt nach Neustart neu
    angelegt). Die Signalliste entsteht beim Bauen aus
    `~/Embedded/private/mvb-trak.txt` (siehe [SETUP.md](SETUP.md)).
  - Softkeys: Seite 7 der Qt- und der Web-Oberfläche (Taste `7`) schickt die
    Befehle aus Port 492 (Türen-Test, Visiteur, Licht/HLK, Klima-Prüflauf)
    und die Lüfterbefehle als Puls von 1 s; die Liste liefert idu2-mvb
    (`softkeys`/`softkey` über den Socket), dazu die Rückmeldungen aus
    Port 290.
  - Bildnummer: FLG2N nimmt einen Softkey nur an, wenn `IDU2-XScreenNum`
    (Port 498) das Bild nennt, zu dem er gehört — 100 Visiteur, 120
    Türen-Test, 200 Zugbegleiter (Licht, HLK, Temperatur, Reservation),
    2710 Klima-Prüflauf; die Lüfter haben keine. idu2-mvb setzt sie bei
    jedem Softkey selbst, von der Oberfläche wie aus der Datenbank. War eine
    andere Nummer gesetzt, folgt der Softkey nach `--bild-wartezeit`
    (Vorgabe 0,15 s), das Loslassen der Applikation ebenso viel später —
    die Pulslänge bleibt. Sicher wären rund 1,5 s: Port 498 holt der
    Busmaster nur alle 1024 ms ab.
  - Source-Ports: alle drei der Rolle (IDU2 491/492/498, IDU1 481/488/489),
    gesendet im Takt ihrer Zykluszeit und nach jeder Änderung, jedes Senden
    ebenfalls in `mvb_prozessdaten`. Werte: Vorgabe 0 (Befehle nicht
    gedrückt), fest in `/etc/idu2-mvb/quellen.conf`, zur Laufzeit über den
    Socket
    (`{"befehl": "setzen", "signal": ..., "wert": ..., "dauer": s}`) oder
    von der Applikation direkt in der Tabelle: `UPDATE icn.mvb_prozessdaten
    SET wert = 3 WHERE name = 'IDU2-XScreenNum'` (Texte in `wert_text`).
    idu2-mvb prüft die Zeilen mit `richtung = 'schreiben'` alle 10 ms
    (`--db-takt`), sendet den Port sofort und schreibt den gesendeten Wert
    mit `roh` zurück; ungültige Werte ersetzt es durch den gesendeten.
  - DDS: Jede Seite landet zusätzlich in `icn.dds_meldungen` (Stand in
    `icn.dds_stand`): neuer Cache_index von DIA1 → Inhalt ersetzt,
    Blättern → Seiten kommen dazu.
  - `--ohne-pd` / `--ohne-db` schalten Prozessdaten / alle
    Datenbankeinträge ab.
- **`recipes-extended/myboard-tasten`** — Hardkeys der Front (32 Tasten,
  Belegung laut INC-100 FW User Manual 5.11): je Druck eine Zeile in
  `icn.tasten` mit Beschriftung, Position P41,n, Zeit und Haltedauer.
  Liest die Tastatur direkt vom Kernel, die Oberflächen bekommen die Tasten
  weiterhin. Ansehen z. B. mit
  `mariadb -e "SELECT gedrueckt, taste, position, dauer_ms FROM icn.tasten ORDER BY id DESC LIMIT 20"`.
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
bitbake myboard-installer      # USB-Stick, der myboard-image auf die CFast schreibt
bitbake myboard-gui            # nur die GUI (für NFS-Deploy)
```

## Installation auf die CFast (Installer-Stick)

`bitbake myboard-installer` baut `myboard-installer-intel-corei7-64.rootfs.wic.gz`
(baut `myboard-image` mit und packt es hinein). Auf den Stick schreiben:

```bash
cd ~/Embedded/build/tmp/deploy/images/intel-corei7-64
sudo bmaptool copy myboard-installer-intel-corei7-64.rootfs.wic.gz /dev/sdX
# oder: zcat myboard-installer-intel-corei7-64.rootfs.wic.gz | sudo dd of=/dev/sdX bs=4M conv=fsync status=progress
```

Am Pixy vom Stick booten (UEFI). Der Installer zeigt auf dem Bildschirm das
Ziel — die eine SATA-Platte, also die CFast — und zählt 10 s herunter;
**jede Taste bricht ab**. Dann schreibt er das System, liest es zurück und
vergleicht die Prüfsumme vom Bauen, verschiebt die GPT-Kopie ans Ende der
Karte und meldet „USB-Stick jetzt abziehen“. Sobald der Stick draußen ist,
startet das Pixy von der CFast neu. Die Schritte stehen zusätzlich auf der
seriellen Konsole (`ttyS0`, Zeilen `myboard-installieren: …`).

Die CFast wird **ganz** überschrieben, auch `/data`, die Datenbank und
Änderungen unter `/etc` (z. B. `quellen.conf`). Der Stick hat eigene
Partitionsbezeichnungen (`instboot`, `installer`), damit der Kernel nicht
das `rootfs` der CFast mit dem des Sticks verwechselt.

**Nur vom Stick starten, ohne Installation:** `bitbake myboard-image-usb`
baut `myboard-image-usb-intel-corei7-64.rootfs.wic.gz` — derselbe Inhalt wie
`myboard-image`, aber alle Partitionen über UUIDs, die bei jedem Bau neu
entstehen (`root=PARTUUID=…`, fstab mit `UUID=…`), und mit den Bezeichnungen
`usbboot`, `usbroot`, `usbdata`. So läuft alles vom Stick, auch wenn auf der
CFast ein System steht. Das Image von `myboard-image` selbst ist nicht für
den Stick gedacht: Es bindet `/boot` und `/data` als `/dev/sda1` und
`/dev/sda3` ein und `/` über `PARTLABEL=rootfs` — vom Stick gestartet wären
das die Partitionen der CFast.
