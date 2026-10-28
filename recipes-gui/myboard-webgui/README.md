# myboard-webgui — die Bedienoberfläche als Web-App

Dieselben Seiten und Tasten wie die Qt-Oberfläche `myboard-gui`, als
HTML/CSS/JavaScript in Firefox. Am Gerät im Kiosk, im Netz mit jedem Browser:

    http://192.168.1.200/gui/

```
Firefox  ── /gui/  ──► Apache :80 ── /usr/share/myboard-webgui/www   (statisch)
         ── /api/  ──► Apache :80 ── 127.0.0.1:8081 myboard-webgui-api
                                      ├─ CAN   can0 roh, Dekodierung wie CanMatrix/CANopen
                                      ├─ DDS   /run/idu2-mvb.sock (wie DdsClient)
                                      └─ USB   Update der Web-App vom Stick
```

Der Browser kann CAN, den Socket von idu2-mvb und USB nicht selbst
ansprechen, PHP-FPM darf es auch nicht (läuft als `nobody`, ohne AF_CAN).
Deshalb der Dienst `myboard-webgui-api` (Python, nur Standardbibliothek,
root). Er läuft immer, auch wenn am Bildschirm die Qt-Oberfläche ist.

## Umschalten am Gerät

Qt bleibt beim Booten die Vorgabe. Die Wahl bleibt über einen Neustart erhalten:

```sh
oberflaeche web      # Firefox-Kiosk mit /gui/ auf dem Bildschirm
oberflaeche qt       # zurück zu myboard-gui
oberflaeche status
```

`myboard-webgui-kiosk.service` und `myboard-gui.service` schließen sich
gegenseitig aus (beide `Xorg :0 vt1`).

## Bedienung

Wie in Qt: `1`–`7` öffnen die Seiten, `C`/`Esc` führt zurück; ▲/▼ wählen den
Halbzug, auf der DDS-Seite die Meldung (▼ auf der letzten Zeile holt die
nächste Seite, ▲ auf der ersten die vorige aus dem Zwischenspeicher); auf
„Update App" `1` = Ja, `2` = Nein. Touch und Maus über die Knöpfe.

**Softkeys** (Seite 7, in Qt und Web gleich): Befehle an den Zug — Türen-Test,
Visiteur, eigener / ganzer Zug, Klima-Prüflauf, Lüfter —, darüber die
Rückmeldungen von FLG2N (Port 290). Welche Softkeys es gibt, sagt idu2-mvb je
nach Rolle (IDU2 alle 42, IDU1 nur die 8 Lüfter). Antippen, oder ▲▼◄► wählen
und `E` (Enter) drücken; jeder Druck ist ein Puls von 1 s auf dem Source-Port
(`--softkey-dauer` von idu2-mvb). Ohne Anmeldung kann das jeder im Netz —
diese Befehle wirken auf echte Anlagen im Zug.

**Ohne Anmeldung:** Jeder im Netz kann die Seiten bedienen, also auch eine
DDS-Abfrage oder ein Update auslösen — nur für das abgeschlossene Labornetz.
idu2-mvb bearbeitet eine DDS-Abfrage nach der anderen; ein zweiter Browser
bekommt solange „Abfrage läuft bereits".

## Update der Web-App vom USB-Stick

Im Wurzelverzeichnis des Sticks ein `*.tar.gz`, das `www/index.html` enthält:

```sh
# im Layer; die Wagenbilder kommen aus der Qt-App und gehoeren mit ins Archiv
mkdir -p /tmp/upd/www/img
cp recipes-gui/myboard-webgui/files/www/* /tmp/upd/www/ 2>/dev/null
cp recipes-gui/myboard-gui/files/src/ICN_Office*.jpg /tmp/upd/www/img/
tar czf webgui-update.tar.gz -C /tmp/upd www
```

„Update App" → `1` entpackt
neben das laufende Verzeichnis und tauscht dann (`www` → `www.alt`,
`www.neu` → `www`); die Seite lädt neu. Zurück zur vorigen Fassung:

```sh
cd /usr/share/myboard-webgui && mv www www.kaputt && mv www.alt www
```

## API

| Endpunkt | Inhalt |
|---|---|
| `GET /api/can/strom` | Server-Sent Events: erst die ganze Tabelle, dann höchstens 4×/s die geänderten Zeilen |
| `GET /api/can/stand` | ganze CAN-Tabelle als JSON |
| `POST /api/dds` `{"seite": N, "neu": bool}` | an idu2-mvb, Antwort unverändert |
| `GET /api/softkeys` | Softkeys der Rolle mit Zustand und Rückmeldungen (idu2-mvb) |
| `POST /api/softkey` `{"signal": "IDU2-BTestSound"}` | Softkey drücken (Puls); nur Signale der Softkey-Liste |
| `GET /api/usb` | `{"stick": bool}` |
| `POST /api/update` | Web-App vom Stick einspielen |
| `GET /api/info` | Angaben für die Info-Seite |

## Testen ohne Gerät

```sh
python3 files/myboard-webgui-api.py --port 8099 --www files/www \
    --matrix ../myboard-gui/files/src/can_matrix.tsv --can-quelle aufnahme.log
# Browser: http://127.0.0.1:8099/gui/   (der Dienst liefert /gui/ dann selbst)
```

`--can-quelle` spielt ein candump-Log ab (`(ts) can0 123#DEADBEEF`), statt
can0 zu lesen. Die CAN-Texte sind gegen die Qt-Dekodierung geprüft (gleiche
Ausgabe für alle 273 IDs der Matrix und alle CANopen-Fälle).
