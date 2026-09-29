# Offline-Prüfung der MVB-Treiberlogik

`tm_replay` übersetzt **`pixy-mvblli.c` unverändert** im Userspace und lässt
die Treiberfunktionen gegen einen echten Abzug des Traffic Memory laufen.
Der Abzug stammt vom laufenden Gerät unter dem Herstellertreiber und
enthält 74 konfigurierte Ports, die drei Message-Ringe und die Service Area.

Damit lässt sich alles prüfen, was nur liest oder rechnet — ohne Hardware,
ohne Reboot und ohne Risiko.

```sh
make check
```

Die Kernel-Attrappen in `fakelinux/` ersetzen `ioread16`/`iowrite16` durch
Zugriffe auf ein Byte-Array und stubben Sperren, Warteschlangen und
Zeichengeräte weg. Der Treibercode selbst wird **nicht** verändert oder
kopiert — `tm_replay.c` bindet ihn per `#include` ein und kommt so auch an
die `static`-Funktionen.

## Was geprüft wird

| Test | Inhalt |
|---|---|
| 1 | Alle 74 Ports mit `apd_get_port()` lesen und gegen eine unabhängig gerechnete Referenz halten; falsche Längen müssen Fehler 3 liefern, unbekannte Ports Fehler 8 |
| 2 | Die drei Message-Ringe: Länge und genau **ein** Wächterelement je Ring |
| 3 | Der Empfangsdispatcher darf auf der leeren Queue nichts holen und **nichts schreiben** |
| 4 | Die Dock-Index-Vergabe aus `PD_CONF` gegen die gemessene: Indexmengen je Größenklasse und `STSR == 0x10DB` |
| 5 | Prozessdaten schreiben und zurücklesen, VP-Umschaltung, Schreiben auf eine Senke muss scheitern |
| 6 | Message senden: Link_Header gegen die Bytes aus dem echten Bustrace, Broadcast, Wächterwanderung |
| 7 | Message empfangen: Frame übernehmen, Wächter weiterrücken, fremden Protokolltyp verwerfen |
| 8 | Adressen der physischen Ports gegen die Werte aus dem Dekompilat |

Stand: **361 Prüfungen, 0 Fehler.**

## Was damit *nicht* geprüft ist

- Alles, was echte Hardware braucht: PCI, `ioremap`, MSI, die
  Fenstererkennung über `BASR0`/`BASR1`, der Initialisierungslauf des
  MVBC und der `0xA55A`-Schleifentest.
- Zeitverhalten, Nebenläufigkeit, Interruptkontext.
- `pixy-mvb.c` als Ganzes — dort steckt fast keine Logik, die sich ohne
  Karte ausführen ließe.

Ein grüner Lauf heißt also: die Rechenwege stimmen. Ob der Controller
darauf reagiert, sagt erst das Gerät.
