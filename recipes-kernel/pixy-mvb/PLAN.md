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

## Stand

| Phase | Stand |
|---|---|
| 1 | erledigt, am Gerät bestätigt |
| 2 | 2.1, 2.2, Ringbelegung, Schließen am Gerät bestätigt; 2.3 Message-Daten und Interruptbetrieb gegengelesen und behoben. Interrupts im Produktivzustand gemessen (irq=7, ~1/s); offen: Gegenversuch Board-Treiber |
| 3 | für den bisherigen Testumfang erledigt (Lauf 3) |
| 4 | vorbereitet: `mvbdiff --md` sendet niedrig/hoch, Port 256, Flush, Senden nach Flush |
| 5 | offen |
