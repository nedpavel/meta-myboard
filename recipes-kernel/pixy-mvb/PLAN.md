# Plan: verbleibende Abweichungen zum Original ausmerzen

Stand nach dem Vergleichslauf `abi-orig` gegen `abi-neu`
(`pixy-mvblli` `3FAE4A7A…` gegen `54631CFD…`).

## Ausgangslage

**Rückgabewerte: 39 von 39 gleich.** Jeder getestete ioctl liefert
denselben Code wie das Original, einschließlich aller Fehlerpfade.
Das Original läuft mit gültigem `HWINIT` auch durch `PD_CONF`.

Die Speicher- und Registerunterschiede des Laufs zerfallen in:

| Gruppe | Was | Bewertung |
|---|---|---|
| Rauschen | `FC`, `EC`, `ECA`, `TC2` | Zähler des laufenden Busses bzw. Endwert der letzten Warteschleife. Kein Befund – das Werkzeug hätte sie nie vergleichen dürfen. |
| Vorgeschichte | Message-Ringe ab `0x8004`, `la_data`, `la_pcs` schon bei `00_open` | Nicht auswertbar: der Traffic Memory wird zwischen den Läufen nicht gelöscht, beide Treiber starteten auf verschiedenen Resten. |
| **echt** | `la_pcs` Index 32 (Port 491, Quelle) nach `PD_CONF` | **Fehler im Nachbau**, siehe Phase 2. |
| unklar | `DR` Bit 9 (`0x0200`) beim Original in 3 von 13 Abzügen, beim Nachbau nie | vermutlich ein Statusbit des Busses; wird gemessen. |

## Phase 1 – Messwerkzeug: „UNTERSCHIED“ muss einen echten Unterschied bedeuten

Ohne Gerät. Alles in `tools/mvbdiff.py`.

1. **Sauberer Start.** Vor jedem Lauf den gesamten Traffic Memory über
   `/dev/mvb0` nullen, in 16-Bit-Zugriffen, den Registerblock
   `SA+0x380…0x3FF` ausgenommen. Nur bei gestopptem Controller und nur,
   wenn kein anderer Prozess `/dev/mvblli0` offen hat. Abschaltbar mit
   `--no-wipe`.
2. **Flüchtige Register ausblenden.** `FC`, `EC`, `ECA`, `ECB`, `MFR`,
   `MFRE`, `IPR0`, `IPR1`, `TC1`, `TC2` und `DR` Bit 9 werden
   ausgegeben, aber nicht verglichen. Ebenso die Zählerzeilen im
   Protokoll.
3. **Unterschiede lesbar machen.** Wortweise mit beiden Werten und
   übersetzter Stelle: PIT-Eintrag mit Port, PCS mit Index, Port und
   Wortnummer, Datenpuffer mit Dock, Seite und Port, Service Area mit
   physischem Port bzw. QDT-Eintrag. Wiederholt sich derselbe
   Unterschied im nächsten Schritt, steht dort nur „wie vorher“.
4. **`DR`-Stichprobe.** Nach dem Öffnen und vor dem Schließen je 2000
   Lesungen von `DR`, als Häufigkeit je Wert protokolliert. Der
   Vergleich stellt die Anteile von Bit 9 nebeneinander.

## Phase 2 – Portkonfiguration korrigieren, Rest systematisch prüfen

Ohne Gerät.

1. **`PD_CONF` wie `lp_ts_open_port`:**
   - Quellen werden **passiv** eingetragen, nur das Senkenbit wird
     gesetzt; aktiv wird eine Quelle erst beim ersten `write()`
     (`apd_put_port`). Der Nachbau macht sie bisher sofort zur Quelle
     und sendet nach `START` Nullen für Ports, die die Anwendung noch
     nicht beschrieben hat.
   - PCS-Wort 0 wird gelesen und nur in F-Code und Typ geändert
     (`& 0xf3fd`), Wörter 1–3 und die Datenpuffer bleiben unberührt.
   - Bereits belegter PIT-Eintrag oder bereits typisierter Index: Fehler.
   - Ungültige Größe: F-Code bleibt stehen, kein Fehler.
2. `write()` auf eine Senke: interner Code 9 statt 8 (nach außen
   weiterhin `EIO`).
3. **Funktion für Funktion gegen das Disassembly**, wie beim
   ioctl-Verteiler: `read`/`write`, `mvb_config`, `mvb_init_board`,
   `mvb_deinit_board`, `mvb_md_q_init`/`install_q`, `mvb_sndp`,
   `mvb_md_dispatcher`, `lm_*`, `poll`, `open`/`release`. Ergebnis ist
   eine Liste aller Funktionen des Originals mit Status *gleich* /
   *abweichend, behoben* / *bewusst nicht nachgebaut, mit Grund*.
4. Jeder Fund bekommt eine Prüfung in `tests/tm_replay.c`.

## Phase 3 – Gerätelauf ohne Bus

```sh
python3 mvbdiff.py run /root/p3-neu
python3 mvbdiff.py run /root/p3-orig      # nach Wechsel auf das Original
python3 mvbdiff.py compare /root/p3-orig /root/p3-neu
```

Ziel: vollständig gleich. Was abweicht, geht zurück in Phase 2.

## Phase 4 – Message-Daten ohne Partner

`write()` auf Message-Daten legt den Frame in die Sendequeue; ohne
`START` bleibt er dort. Queue und Ring lassen sich damit zwischen beiden
Treibern byteweise vergleichen (`--md`, ohne `--go`). Für den Empfang
braucht es danach einen Partner am Bus.

## Phase 5 – Betrieb und Aufräumen

- Ein `--go`-Lauf mit dem Nachbau: Empfang nach der `PD_CONF`-Änderung
  erneut bestätigen, Quelle 491 erst nach dem ersten `write()` aktiv.
- `dbg_*`-Zähler aus beiden Modulen entfernen.
- udev-Regeln wieder in Kraft setzen (Bind-Mounts lösen oder Neustart).

## Ergebnis Phase 1 am Gerät (`p1-orig` gegen `p1-neu`)

Mit gelöschtem Traffic Memory und ohne Rauschen bleiben **genau zwei
Unterschiede** – beide echt, beide erklärt:

| Stelle | Original | Nachbau | Ursache |
|---|---|---|---|
| Message-Ringe, 449 Datenzeiger (7 + 221 + 221) ab `00_open` | LLR *k* → Puffer *k* | LLR *k* → Puffer *k−1* | `mvb_md_install_q` gibt auch dem Wächter einen Puffer (bleibt unbenutzt) und leert alle Puffer. Folgezeiger waren gleich. |
| PCS 32 (Port 491, Quelle) Wort 0 nach `PD_CONF` | `0x1000` | `0x1800` | wie vorhergesagt: Quelle erst beim ersten `write()` aktiv |

Alles andere ist gleich: alle Register außer den flüchtigen in allen
13 Schritten, PIT, PCS der übrigen Ports, Datenpuffer, Service Area.

**`DR` Bit 9 ist geklärt:** bei beiden Treibern gesetzt, in 6–22 % der
Lesungen (Original 130 und 432 von 2000, Nachbau 261 und 378). Ein
Statusbit des Busses, kein Unterschied. Bleibt ausgeblendet.

**Neu aufgefallen:** Nach dem Löschen waren im Lauf des Nachbaus 8 Worte
sofort wieder ungleich null, im Lauf des Originals keines. Das schreibt
der Controller selbst – vermutlich hinterlässt der zuvor geschlossene
Treiber ihn in einem anderen Zustand. `mvbdiff` protokolliert jetzt
Lage und Wert dieser Worte, das `SCR` beim Löschen und einen Abzug nach
`close()` (`99_zu`); damit wird `mvb_deinit_board` in Phase 3 direkt
vergleichbar.

## Ergebnis Lauf 2 (`p2-orig` gegen `p2-neu`)

**Alle 14 Speicherabzüge bis einschließlich `99_ende` gleich, alle
Register gleich.** Die beiden Korrekturen aus Lauf 1 sind am Gerät
bestätigt.

Einziger Unterschied: der Zustand **nach `close()`**. Das Original setzt
in `mvb_deinit` `SCR = 0`; der Controller geht in RESET, der
Registerblock springt an den Grundplatz `TM + 0x3C00` zurück (gemessen
bei `0x3F80…`: `SCR 0700`, `MCR 2880`, `DR 0008`, `TCR 0022`). Der
Nachbau stoppte nur (`IL = CONFIG`) und ließ den Controller konfiguriert
und hörend am Bus. Das waren auch die „8 Worte nach dem Löschen“: sie
stammten jeweils vom zuvor geschlossenen *Original*.

Behoben: `mvb_deinit_board` in der Reihenfolge des Originals (BCR-
Interruptnummer löschen, Quellen abmelden, Dienst austragen,
`mvb_stop`, `SCR = 0`); `mvb_stop` löscht wie im Original nur Bit 1 des
IL-Felds.

Zwei Fehler im Werkzeug, ebenfalls behoben:
- Das Löschen sparte nur den Registerblock bei `0xFF80` aus. Nach dem
  `close()` des Originals liegt er bei `0x3F80` und bekam Nullen ab
  (folgenlos, weil `open()` ihn neu aufsetzt). Jetzt bleiben alle drei
  möglichen Plätze unberührt; der lebende Block wird an `MCR` erkannt
  (Version 5, `mcm` passend zum Platz).
- Der Protokollvergleich lief Zeile gegen Zeile und geriet durch die
  zusätzlichen Zeilen außer Tritt. Jetzt wird ausgerichtet, und der
  Vorzustand (vom zuvor geschlossenen Treiber) steht getrennt.

## Ergebnis Lauf 3 (`p3-orig` gegen `p3-neu`): ALLES GLEICH

Nachbau `6A74FC76…` gegen Original `3FAE4A7A…`, beide auf demselben
Board-Treiber, mit gelöschtem Traffic Memory:

- 39 von 39 Rückgabewerten gleich,
- alle 15 Speicherabzüge gleich, von `00_open` bis `99_zu` nach `close()`,
- alle Register gleich, auch am Grundplatz nach dem RESET,
- Vorzustand beider Läufe identisch (`SCR@3C00 0700`, Registerblock am
  Grundplatz) – beide Treiber hinterlassen den Controller gleich.

Damit ist belegt: Für alles, was `mvbdiff` ohne Bus und ohne
Message-Daten anspricht – Initialisierung, alle 22 ioctls samt
Fehlerpfaden, Portkonfiguration, Prozessdaten schreiben,
Ereignisaufzeichnung, Schließen – ist der Nachbau vom Original nicht zu
unterscheiden.

**Nicht abgedeckt** und deshalb Gegenstand von 2.3 und Phase 4:
Message-Daten (`write`/`read`/`poll`, Sendequeue, Empfangsdispatcher),
`read()` im Betrieb, der Interruptpfad und der Board-Treiber `pixy-mvb.ko`
selbst (beide Läufe nutzten den Nachbau).

## 2.3 Gegenlesen: Message-Daten und Interruptbetrieb

Gegen Dekompilat und, wo Ghidra unsicher war, Disassembly. Befunde,
alle behoben und in `tm_replay` geprüft (484 Prüfungen, 0 Fehler):

| Stelle | Original | Nachbau bisher |
|---|---|---|
| Modulparameter | heißt **`irq`**, Vorgabe **7** | hieß `mvb_irq`, Vorgabe 0 – ein `options pixy_mvblli irq=…` hätte das Laden verhindert |
| BCR | Interruptnummer nur bei `irq > 0` geschrieben | immer, bei 0 als 0 |
| `DTI1`/`RQE` | nur bei `irq > 0` angemeldet (`mvb_md_q_init`) | immer |
| `MSNK` Bit 5 | nur bei `irq > 0` | immer |
| `MD_FLUSH_QUEUE` | `MR = 0x0800`, QDT-Sendeeinträge 0, Ringe bleiben | kein `MR`, Ringe neu aufgebaut |
| Empfangsdispatcher | **ein** Frame je Aufruf | Schleife über alle |
| `rq_overflow` | wird nie gesetzt | bei vollem Softwarering Bit 2 |
| `poll()` | bei `irq = 0` nie lesbar; sonst je DTI1-Meldung einmal `POLLIN\|POLLRDNORM` (Zähler modulo 16); nie `POLLOUT` | `POLLOUT` immer, `POLLIN` solange Frames im Ring |
| DTI1-Dienst | nur bei `irq ≠ 0`: Dispatcher, Zähler, wecken | immer Dispatcher |

Gleich befunden: `write()`, `read()`, `mvb_sndp` (QDT beim ersten
Senden, Link_Header, Wächter, EF0/EF1 nach `q_tq_priority`, `MR`),
Interruptdienst (`IVR1` vor `IVR0`, Schranke 20, danach 0).

### Interrupts: Messung im Produktivzustand

Nach Neustart, Herstellerstack läuft (beide Original-Module):

```
/sys/module/pixy_mvblli/parameters/irq = 7     (keine modprobe-Optionen)
/proc/interrupts  pixy-mvb  292 -> 303 in 10 s  (PCI-MSI, ~1/s)
```

Damit ist belegt: **Vorgabe `irq = 7` ist der Produktivbetrieb**, und dort
kommen Interrupts an – vermutlich DTI1, also eingehende Messages der
laufenden Anwendung.

**Nicht** belegt ist, dass der nachgebaute Board-Treiber Interrupts
verliert. In unseren Läufen lief keine Anwendung, niemand schickte
Messages, und die einzige anstehende Quelle (FEV) kam unter *beiden*
LLIs nie im `ISR` an – es gab nichts zuzustellen. Die Probe-Funktionen
beider Board-Treiber sind gleich bis auf die Interrupt-Flags (behoben:
`IRQF_SHARED` wie im Original statt `IRQF_ONESHOT`, das auf PREEMPT_RT
den Handler im harten Interruptkontext laufen ließe) und den
Meldungstext.

Entscheiden kann das nur der Gegenversuch unter gleichen Bedingungen:
derselbe `mvbdiff --go`-Lauf einmal mit Original-Board-Treiber, einmal
mit dem Nachbau darunter, beide Male mit dem originalen LLI.
`mvbdiff` zählt dafür jetzt `/proc/interrupts` über die 60 s Betrieb.

### Gegenversuch ohne Anwendung (irq-A / irq-B)

`mvbdiff --go --no-pd` mit originalem LLI, einmal auf dem
Original-Board-Treiber (A), einmal auf dem Nachbau (B): **beide +0
Interrupts in 60 s**, Register identisch (`IPR1 0080`, `ISR1 0000`). Ohne
Anwendung gibt es keine Interruptquelle. Der Board-Treiber ist damit
entlastet, aber nicht bewiesen.

Woher die Interrupts im Produktivbetrieb kommen, zeigt der Produktivabzug:
die Anwendung hat dreimal dieselbe Anfrage an Gerät 6 (Funktion 210)
gesendet und jedes Mal genau eine Antwort erhalten. Unaufgefordert
schickt niemand – in Lauf A war MD eingerichtet und der Bus lief, es kam
nichts. **`mvbdiff --ping`** spielt diese Anfrage byteweise nach und
wartet auf die Antwort; die löst DTI1 aus. Damit sind Interruptzustellung
und Message-Empfang ohne laufende Anwendung prüfbar.

### Beobachtung: nach `systemctl stop session-1.scope` keine Interrupts mehr

Auch mit beiden Original-Modulen. Zwei Erklärungen sind möglich:

1. **Die Anwendung ist die Quelle** – sie fragt an, die Antworten lösen
   DTI1 aus. Ohne Anwendung keine Antworten, also keine Interrupts; die
   Zustellung selbst ist in Ordnung.
2. **Die Anwendung schaltet etwas frei**, das unsere Läufe nicht
   nachbilden (GPIO-, ISA- oder PCI/MSI-Zustand, ein nicht genutzter
   ioctl). Dann wäre die Zustellung nach dem Stoppen abgeschaltet.

Prüfung: `irqstate.py` hält die ganze Kette fest (MVBC IPR/IMR/ISR, ISA
BCR, alle GPIO-Register, PCI COMMAND und MSI-Capability, Kernelzähler),
nur lesend, auch bei laufender Anwendung. Einmal mit laufender Anwendung,
einmal nach dem Stoppen, und `mvbdiff --go` schreibt denselben Zustand
nach `START` ins Protokoll. Unterscheiden sich die Zustände nicht, bleibt
Erklärung 1 – und `--ping` belegt sie, indem es die Quelle ohne Anwendung
erzeugt.

### Ergebnis: Kette identisch, Interrupts nur in der Anlaufphase

`irqstate.py` bei laufender Anwendung und `mvbdiff --go` mit
Original-Modulen nach `START`: **identisch** – MVBC-Masken, `BCR 2677`
(Interrupt 7; `2600` nur bei geschlossenem LLI), alle GPIO-Register,
PCI COMMAND, MSI. Die Anwendung schaltet nichts frei.

Auch bei laufender Anwendung blieb der Zähler über 34 s bei 331; kurz
nach dem Booten waren es 292 → 303 in 10 s. Interrupts kommen vor allem
in der Anlaufphase.

Erster `--ping` empfing nichts: der nachgespielte Frame (SZ 6) war die
*Antwort* der Anwendung auf einen Anruf von Gerät 6 (SZ 27, MTC 0x80),
nicht eine Anfrage. `--ping` sendet jetzt den Anruf selbst und
protokolliert zusätzlich, ob der Controller ihn gesendet hat (QDT
xmit_q1).

**Entscheidender Test** bleibt die Anwendung selbst auf den
nachgebauten Modulen. Dafür vor dem Stoppen ihre Startparameter
festhalten (`systemctl status session-1.scope`, `/proc/<pid>/cmdline`,
`environ`, `cwd`) und sie nach dem Modultausch von Hand starten.

### `--ping` mit Verbindungsaufbau: nicht gesendet

QDT xmit_q1 `0000 → 2050` beim Einhängen, danach unverändert: der
Controller hat den Frame nie auf den Bus gebracht. Messages sendet ein
MVB-Gerät erst, wenn der Busmaster es abfragt. Ein möglicher Grund liegt
im Testablauf selbst: `mvbdiff` schrieb zuvor `0x0011` ins untere Byte
des DSW, das der Busmaster sieht. Seitdem wird das DSW nach dem Test
zurückgeschrieben.

Künstliche Interruptquellen hängen damit an Annahmen über den Bus, die
sich ohne Busanalyse nicht prüfen lassen. Der aussagekräftigere Test ist
die Anwendung selbst auf den nachgebauten Modulen.

### Anwendung auf den nachgebauten Modulen

Aus `systemctl status session-1.scope`: `getty@tty1` meldet root an
(`login -- root`), die Login-Shell startet `startx`, `.xinitrc` startet
`xterm -e /opt/pixy/toolkit/scripts/toolkit/system.sh run mode pad`, das
wiederum `/opt/project/start.sh` mit `./target` und `extApp 200`. Die
Kette lässt sich mit `systemctl restart getty@tty1` neu anstoßen – wenn
das Autologin dort eingerichtet ist.

Ablauf: erst mit den Original-Modulen (Kontrolle, dass der Neustart so
funktioniert, und Referenz für die Interrupts beim Hochfahren), dann
dasselbe mit beiden Nachbauten.

### Neustart über getty@tty1: Anwendung läuft, Anzeige nicht

`systemctl restart getty@tty1` bringt die ganze Kette wieder hoch
(Xorg, xterm, system.sh, target, extApp). Mit den Original-Modulen:
`target` hält `/dev/mvblli0`, `SCR 87C7`, `BCR 2677`, Interrupts kommen.
Bild und Touch reagieren danach aber nicht – ein X-/Eingabeproblem nach
dem Sitzungsneustart, unabhängig von den MVB-Treibern. Für den Test
genügt der MVB-Zustand: `irqstate.py` zeigt jetzt zusätzlich die drei
Quellports der Anwendung (491 trägt offenbar ihr Lebenszeichen) und zwei
Lebenszeichen-Senken. Zwei Aufnahmen im Abstand von Sekunden zeigen, ob
die Anwendung schreibt und empfängt.

### Läufe O und N: Anwendung hat das LLI nicht geöffnet

Beide Läufe sagen nichts über die Treiber, weil die neu gestartete
Anwendung `/dev/mvblli0` in keinem der beiden geöffnet hat:
`SCR@3C00 0700` (Reset), `BCR 2600`, `fuser` leer, Interrupts +0.

* **O (Originale):** Das alte `target` hielt das Gerät nach
  `terminate-session` noch. Das neue bekam zweimal EBUSY
  (`4447.99`, `4474.54`, 27 s Abstand); das alte schloss erst bei
  `4527.23`. Danach kam innerhalb des Messfensters kein weiterer Versuch.
  Beim Hochfahren lief es genauso ab: EBUSY `195.09`, Freigabe `195.77`,
  erfolgreiches `open()` erst `292.97`, also rund 97 s später. Die
  Anwendung versucht es also erneut, aber mit langem Abstand.
  90 s Wartezeit reichen nicht.
* **N (Nachbau):** keine Meldung des LLI, also auch kein EBUSY. Damit hat
  die Anwendung entweder gar nicht versucht zu öffnen, oder sie ist an
  etwas gescheitert, bevor sie `open()` aufruft.

Beim Gegenlesen dazu gefunden: Der Nachbau meldete eine andere Kennung als
das Original. Er gab `version=3.0.0` statt `1.0.3` zurück, und
`sw_version` im Statusblock stand auf `pixy-mvblli v3.0.0 - MVB Link L…`
statt `pixy-mvblli-V1.0.3-03.12.24`, das das Original mit `"%s-V%s-%s"`
in `mvb_init_board` bildet. Prüft die Bibliothek eines von beiden, lehnt
sie den Nachbau ab. Beides ist jetzt angeglichen, ebenso Beschreibung und
Kernelmeldungen ("MVB ESD controller of board /dev/mvblli0 initialized",
"Cannot open device /dev/mvblli0, already in use"). Damit greift dasselbe
`grep` bei beiden Modulsätzen.

**Wahrscheinliche Ursache für Lauf N: die udev-Regeln.** Die echten
Regeln (unter dem Bind-Mount gelesen) tun beim `add` mehr als nur
`modprobe`:

```
40-mvb.rules:   SUBSYSTEM=="pixy-mvb", ATTR{board_type}=="MVB",
                ENV{ID_BUS}="$attr{pci_id}", SYMLINK+="mvb", MODE="0666"
                remove: RUN+="/usr/local/bin/pixy-mvb-module.sh $env{ID_BUS}"
41-mvblli.rules: SUBSYSTEM=="pixy-mvb", ATTR{board_type}=="MVB",
                RUN+="/usr/bin/modprobe pixy_mvblli"
                SUBSYSTEM=="pixy-mvblli", SYMLINK+="mvblli", MODE="0666"
```

Die Symlinks `/dev/mvb` und `/dev/mvblli` legt nur udev an. In Lauf N
waren beide Regeln beim `insmod` durch die leere Datei ersetzt. Damit
fehlten die Symlinks, und eine Anwendung, die `/dev/mvblli` öffnet,
scheitert an ENOENT, bevor der Treiber etwas davon sieht. Das passt zu
"keine einzige Meldung". `mvbdiff` öffnet `/dev/mvblli0` direkt und war
deshalb nie betroffen. Der Nachbau liefert Klassennamen (`pixy-mvb`,
`pixy-mvblli`) und Attribute (`board_type`, `pci_id`) so, dass die
Regeln greifen.

Ein Start schon beim Booten geht nicht: `/` ist read-only, und an das
Bootmenü kommt man nicht heran. Der Test läuft deshalb zur Laufzeit. Nach
dem Modulwechsel kommen die Regeln wieder zurück, dann folgt
`udevadm trigger --action=add` für beide Subsysteme.

### Boot mit beiden Nachbauten: Anwendung läuft

`install`-Zeilen in `/etc/modprobe.d/zz-mvb-nachbau.conf`, Module unter
`/usr/local/lib/mvb-nachbau/`. Nach dem Reboot geladen: `9DD4743F…`
(pixy-mvb) und `19962DCA…` (pixy-mvblli). Keine Kernel-Panik.

* udev legt `/dev/mvb` und `/dev/mvblli` an, `target` und `extApp`
  halten `/dev/mvblli0`. Das `open()` kam bei 15,9 s, gleich beim ersten
  Versuch.
* Registerstand gleich dem Produktivstand mit Originalen: `SCR 87C7`,
  `MCR 2803`, `DR 150D`, `IMR0 0003`, `IMR1 0880`, `TCR 0022`, `BCR 2677`.
* Prozessdaten fließen: Quelle 491 zählt in 10 s von `0x7C` auf `0x86`
  hoch, die Senken 181 und 471 ändern sich, und ihre tacks sind frisch.
* 208 Interrupts, alle vom Nachbau bearbeitet:
  `dti1 72 + dti2 121 + fev 15 = 208`, `other 0`. Danach kommen keine
  mehr (208 → 208 in 10 s), wie beim Original nach dem Anlauf.

### Interrupt bleibt hängen, beim Original genauso

Beobachtung an der Anzeige: Beim ersten Aufruf des DDS-Untermenüs kommen
die MVB-Messages an. Beim zweiten Aufruf erscheint nur noch ihre Anzahl.

Folgeaufnahmen mit dem Nachbau: `IPR0 4303`, `IPR1 0680`, der Zähler
bleibt dauerhaft bei 208. DTI1 (Message empfangen), DTI2 und FEV stehen
an und sind freigegeben, aber es kommt kein Interrupt mehr. Bei `irq=7`
holt nur DTI1 Messages ab, also ist der Empfang tot.

**Das Original zeigt denselben Zustand.** In `zustand-laeuft.txt`
(Originale, Produktivbetrieb) stehen `IPR0 0302` und `IPR1 0080`, der
Zähler bei 331. In einem anderen Lauf mit Originalen
(`zustand-orig-neustart.txt`) liefen die Interrupts weiter, bis 7464 bei
`IPR 0000`. Der Stillstand tritt also nicht immer ein.

Erklärung: Über `BCR 0x77` liegen beide Interruptausgänge des MVBC auf
derselben ISA-Leitung 7, und die Karte macht daraus einen MSI, der auf
eine Flanke reagiert. Board-Treiber und LLI quittieren wie das Original:
IVR1 leeren, dann IVR0, Ende. Kommt eine IVR1-Quelle hinzu, während IVR0
geleert wird, fällt die gemeinsame Leitung nie mehr ab. Ohne neue Flanke
gibt es keinen Interrupt, und es gibt niemanden, der IVR noch einmal
liest. Der Board-Treiber des Originals quittiert auf der Karte nichts
(Disassembly `irq_handler`, `pixy-mvb.c:1229`), daran liegt es also
nicht.

Abhilfe (Abweichung vom Original, Parameter `irq_rearm`, Vorgabe 1, zur
Laufzeit änderbar): IVR1 und IVR0 so lange wiederholt leeren, bis ein
ganzer Durchgang über beide nichts mehr findet. Höchstens 8 Durchgänge.
`dbg_rearm` zählt die Durchgänge, die noch etwas fanden. Mit
`irq_rearm=0` verhält sich der Nachbau wie das Original. Test 16 in
`tm_replay` stellt das Rennen nach.

### Die Ursache war das Messwerkzeug

`dbg_rearm = 0` – das Rennen oben ist am Gerät **nicht** aufgetreten, die
Erklärung war falsch. Der Lauf mit `irq_rearm` zeigt stattdessen:

* 728 Interrupts, davon 576 DTI1. Die Anwendung bekam die MVB-Messages
  und zeigte sie an. **Der Message-Empfang des Nachbaus arbeitet.**
* Die erste Aufnahme ist sauber: `IPR0 0000`, `IPR1 0000`.
* Ab der zweiten Aufnahme steht der Zähler bei 728 und `IPR0` trägt
  `0302`, später `4303`. Der Betreiber sieht es an der Anzeige: Die
  Messages kamen, **bis die Abfrage über SSH lief**, danach nicht mehr.

Damit ist `irqstate.py` selbst der Auslöser. Eine seiner Leseoperationen
lässt die Interruptleitung oben hängen, die nächste Flanke fällt aus, und
da niemand IVR mehr liest, bleibt es so. Jede frühere Messung, die
„Interrupts nur in der Anlaufphase" zeigte – auch die mit den
Originalmodulen (`zustand-laeuft.txt`, 331 bei `IPR0 0302`) – ist
dieselbe Selbstvergiftung. Der Befund „das Original hat denselben
Fehler" bleibt richtig, nur ist es kein Fehler im Original, sondern die
Wirkung des Werkzeugs auf beide.

**Welche Leseoperation, ist geklärt - es sind die Cachezeilen.** `mmap`
bildet das BAR gecacht ab (Original wie Nachbau), also holt die CPU je
Zugriff 64 Byte vom Bus. Die Registerbasis liegt zeilenbündig bei BAR
`0x204FF80`:

| Cachezeile | Register |
|---|---|
| `0x204FF80` = `SA+0x380…0x3BF` | SCR MCR DR STSR **FC EC MFR MFRE** MR MR2 DPR DPR2 IPR IMR |
| `0x204FFC0` = `SA+0x3C0…0x3FF` | ISR0 ISR1 **IVR0 IVR1** DAOR DAOK TCR TR1 TR2 TC1 TC2 |

`irqstate.py` las `ISR0` (+0x3C0), `ISR1` (+0x3C4) und `TCR` (+0x3E0) -
alle drei in der Zeile von IVR. Jeder dieser Zugriffe holt `IVR0` und
`IVR1` mit und nimmt dem Treiber die anstehenden Quellen weg.

Zweiter, unabhängiger Beleg für die zeilenweise Übertragung: In der
ersten Zeile liegen `FC`, `EC`, `MFR`, `MFRE`, die beim Lesen verfallen.
Genau diese vier musste `mvbdiff` von Anfang an als flüchtig ausblenden,
obwohl es sie nie einzeln gelesen hat.

Behoben im Werkzeug, nicht im Treiber: `irqstate.py` liest aus der Zeile
`SA+0x3C0…0x3FF` nichts mehr. Die erste Zeile bleibt drin - sie ist für
den Interrupt harmlos und kostet nur die Statistikzähler der Anwendung.

### Aufsicht gegen die verlorene Flanke - gebaut, aber nicht übernommen

Für den Fall wurde eine Aufsicht gebaut (Commit `4e7e2ab`): alle
`irq_watchdog` Millisekunden nachsehen, ob etwas Freigegebenes in IPR
ansteht, ohne dass der Dienst läuft, dann die Quittung nachholen und
nötigenfalls die Maske kurz löschen, damit die Leitung fällt und neu
steigt. Test 17 prüft alle vier Fälle.

**Nicht übernommen.** Zwei Gründe:

1. Jeder beobachtete Stillstand ist auf `irqstate.py` zurückzuführen.
   Ohne Leser auf `/dev/mvb0` lief der Interrupt durch (728 Stück, 576
   DTI1, Messages in der Anzeige). Es gibt keinen Beleg für einen
   Ausfall von selbst, und das Original läuft seit Jahren ohne jede
   Vorkehrung.
2. Die Aufsicht müsste IPR0/IPR1 zyklisch lesen. Das Original liest
   diese Register **nie**. Da gerade die Nachbarschaft von IPR, ISR und
   IVR im Verdacht steht, die Leitung hängen zu lassen, wäre die
   Aufsicht möglicherweise selbst die Ursache des Fehlers, den sie
   heilen soll.

Der Code bleibt in `4e7e2ab` liegen. Wird je ein Stillstand ohne
fremden Leser beobachtet, ist er in einer Minute wieder eingehängt. Für
die Suche nach der schädlichen Leseoperation braucht `irqstate.py
--bisect` diesen Bau; die Warnung im Werkzeug bleibt in jedem Fall.

## Offizieller Rebuild

Beide Module sind nachgebaut, am Gerät im Produktivbetrieb bestätigt und
damit der Stand, der den Lieferanten ersetzt:

| | srcversion | Größe | Stand |
|---|---|---|---|
| `pixy-mvb.ko` | `9DD4743F2973EC003DE83D5` | 360 712 B | Board-Treiber |
| `pixy-mvblli.ko` | `EE452ED1FD9856506534C13` | 612 744 B | LLI, `md5 893e5014…` |

Nachgewiesen am Gerät, mit der unveränderten Hersteller-Anwendung auf den
Original-Bibliotheken:

* Beide Module laden beim Booten über `/etc/modprobe.d/zz-mvb-nachbau.conf`.
* udev legt `/dev/mvb` und `/dev/mvblli` an, `target` und `extApp` öffnen
  das Gerät beim ersten Versuch.
* Registerstand gleich dem Produktivstand mit den Originalen: `SCR 87C7`,
  `MCR 2803`, `DR 150D`, `IMR0 0003`, `IMR1 0880`, `TCR 0022`, `BCR 2677`.
* Prozessdaten in beide Richtungen: Quelle 491 zählt im Sekundentakt,
  die Senken 181 und 471 kommen frisch an.
* Message-Daten über den Interrupt: 576 DTI1, in der Anzeige sichtbar.
* Der Interrupt-Stillstand tritt mit den Originalen genauso auf und ist
  in beiden Fällen Folge eines fremden Lesers auf `/dev/mvb0`.

Abweichungen vom Original, beide bewusst und abschaltbar:

| Parameter | Vorgabe | Wirkung |
|---|---|---|
| `irq_rearm` | 1 | IVR1/IVR0 wiederholt leeren, bis ein Durchgang leer bleibt. Am Gerät nie ausgelöst (`dbg_rearm = 0`), schließt aber ein echtes Rennen. `0` = Original. |
| `dbg_*` | — | Nur lesbare Zähler. Sie haben die Fehlersuche entschieden und kosten nichts; sie bleiben drin. |

## Stand

| Phase | Stand |
|---|---|
| 1 | erledigt, am Gerät bestätigt |
| 2 | erledigt. Anwendung läuft auf beiden Nachbauten, Prozess- und Message-Daten fließen (576 DTI1) |
| 3 | für den bisherigen Testumfang erledigt (Lauf 3) |
| 4 | vorbereitet: `mvbdiff --md` sendet niedrig/hoch, Port 256, Flush, Senden nach Flush. Durch den Lauf mit der Anwendung überholt: Messages laufen im Betrieb über den Interrupt |
| 5 | Module im Betrieb, Ladeweg über `modprobe.d` eingerichtet, Dokumentation steht. Offen: `--ping` gegen ein zweites Gerät, Portierung auf einen neueren Kernel |
