# pixy-mvb / pixy-mvblli — Nachbau der MVB-Kernelmodule

Ersatz für die beiden Binärmodule von HaslerRail für die Pixy-1000
MVB/PCIe-Karte. Beide sind ABI-gleich zum Original: `libpixymvb.so`,
`libmvbrtp.so` und die Anwendungen laufen unverändert weiter.

| Modul | Aufgabe | Gerät |
|---|---|---|
| `pixy-mvb` | PCI, BAR0, MSI, `mmap`, Interruptverteilung | `/dev/mvb0` |
| `pixy-mvblli` | MVB-Controller, Prozess- und Message-Daten | `/dev/mvblli0` |

`pixy-mvblli` koppelt über `filp_open("/dev/mvb0")` und dessen `K*`-ioctls an
`pixy-mvb` an — **keine Symbolabhängigkeit**, `depends:` ist bei beiden leer,
genau wie beim Original. Deshalb lassen sich Original und Nachbau paarweise
mischen, und genau das ist die Teststrategie.

## Bauen

Ein Quellbaum, zwei Kernelstände. Drei Stellen wählen per
`LINUX_VERSION_CODE` die passende Schreibweise:

| Stelle | 5.10 | ab |
|---|---|---|
| `pixy-mvb.c` mmap | `vma->vm_flags \|= …` | 6.3: `vm_flags_set(vma, …)` |
| beide, `file_operations` | `.llseek = no_llseek` | 6.12: Zeile entfällt, NULL heißt dort „nicht seekbar" |
| beide, `module_init` | `class_create(THIS_MODULE, …)` | 6.4: `class_create(…)` |

Die 5.10-Fähigkeit bleibt mit Absicht erhalten: Das Pixy1000 mit dem
Herstellerkernel ist der einzige Prüfstand, auf dem sich der Nachbau
gegen den Originaltreiber vergleichen lässt (`mvbdiff.py compare`).

### Gegen den Herstellerkernel (5.10.16.rt30.pixy-2)

Voraussetzung ist das Paket `linux-rt-headers-5.10.16.rt30.pixy-2` von
`repo.pixy.ch`. Es enthält den Buildtree unter
`/usr/lib/modules/5.10.16.rt30.pixy-2/build`.

```sh
# Headers auspacken (ohne Installation, irgendwohin)
mkdir -p ~/pixy-ktree && cd ~/pixy-ktree
tar --use-compress-program=unzstd -xf linux-rt-headers-5.10.16.rt30.pixy-2-x86_64.pkg.tar.zst

KDIR=~/pixy-ktree/usr/lib/modules/5.10.16.rt30.pixy-2/build

cd recipes-kernel/pixy-mvb/files
make -C "$KDIR" M="$PWD" modules GCC_PLUGINS_CFLAGS=

cd ../../pixy-mvblli/files
make -C "$KDIR" M="$PWD" modules GCC_PLUGINS_CFLAGS=
```

`GCC_PLUGINS_CFLAGS=` schaltet das für gcc 10.2.0 gebaute
`structleak`-Plugin ab. Das ist ABI-neutral, solange `CONFIG_MODVERSIONS`
und `CONFIG_GCC_PLUGIN_RANDSTRUCT` aus sind — beides ist in dieser
`.config` der Fall. Wer exakt mit der Herstellertoolchain bauen will,
nimmt gcc 10.2.0 und binutils 2.35.1 (Arch-Stand 2022-11-29) und lässt
`GCC_PLUGINS_CFLAGS=` weg.

### Pflichtprüfung vor jedem Geräteeinsatz

```sh
modinfo pixy-mvb.ko    | grep vermagic
modinfo pixy-mvblli.ko | grep vermagic
```

Beide müssen **zeichengleich** sein mit

```
vermagic:       5.10.16.rt30.pixy-2 SMP preempt_rt mod_unload
```

Weicht die Zeile ab, wird nichts geladen. `CONFIG_MODULE_SIG_FORCE` ist
nicht gesetzt, eine Signatur wird also nicht gebraucht; der Kernel wird
lediglich als „tainted" markiert.

### Im Yocto-Build

Beide Rezepte liegen in dieser Layer:

```
recipes-kernel/pixy-mvb/pixy-mvb_3.0.0.bb
recipes-kernel/pixy-mvblli/pixy-mvblli_3.0.0.bb
```

In das Image aufnehmen:

```
IMAGE_INSTALL:append = " pixy-mvb pixy-mvblli"
```

Die Herstellerpakete müssen dann aus dem Image genommen werden, sonst
kollidieren die Modulnamen.

## Ins Wrynose-Image

Der Layer bringt vier Rezepte mit:

| Rezept | Inhalt |
|---|---|
| `pixy-mvb_3.0.0.bb` | Board-Treiber, `/dev/mvb0`, Kopf `pixy-mvb.h`, Regel `40-mvb.rules` |
| `pixy-mvblli_3.0.0.bb` | LLI, `/dev/mvblli0`, Kopf `pixy-mvblli.h`, Regel `41-mvblli.rules` |
| `pixy-mvb-tools_1.0.bb` | die vier Python-Werkzeuge nach `${bindir}` |
| `pixy-mvb-tmreplay_1.0.bb` | `tm_replay` und `tm_replay-6x` samt Speicherabzug |

Ins Image kommen sie über die Packagegroup, eingehängt in
`recipes-extended/images/myboard-image.bb`:

```
IMAGE_INSTALL:append = " packagegroup-pixy-mvb"
```

Das zieht beide Module und die Werkzeuge. `packagegroup-pixy-mvb-tests`
nimmt zusätzlich `tm_replay` mit — eine Logikprüfung ohne Hardware, im
Image bequem, aber nicht nötig; die Zeile steht dort auskommentiert
daneben.

**Ladereihenfolge.** `pixy-mvb` wird über `KERNEL_MODULE_AUTOLOAD`
geladen, `pixy-mvblli` **nicht**: Es öffnet `/dev/mvb0` schon im
`module_init` und scheitert, wenn der Board-Treiber noch fehlt. Es kommt
deshalb über `41-mvblli.rules`, die erst feuert, wenn das Gerät da ist —
derselbe Weg, den der Hersteller geht.

**Die udev-Regeln sind nicht schmückend.** Sie legen `/dev/mvb` und
`/dev/mvblli` an. Fehlen sie, gibt es nur `/dev/mvb0` und `/dev/mvblli0`,
und ein Programm, das den Symlink öffnet, scheitert mit `ENOENT`, bevor
der Treiber etwas davon sieht. Die `remove`-Regel des Herstellers ist
bewusst **nicht** übernommen: Sie ruft `pixy-mvb-module.sh` auf, das die
PCI-Funktion entfernt und den Bus zurücksetzt, womit jedes `rmmod` einen
Neustart kostet.

Auf dem Zielsystem prüfen:

```sh
cat /sys/module/pixy_mvb/srcversion /sys/module/pixy_mvblli/srcversion
ls -l /dev/mvb /dev/mvb0 /dev/mvblli /dev/mvblli0
dmesg | grep -i pixy
tm_replay    /usr/share/pixy-mvb/mvbsnap
tm_replay-6x /usr/share/pixy-mvb/mvbsnap
```

## Auf dem Gerät testen

Der vollständige Ablauf steht in **[`TESTPLAN.md`](TESTPLAN.md)** — vom
`vermagic`-Vergleich über die Referenzaufnahme bis zum Abnahmekriterium,
in der Reihenfolge, in der er ausgeführt werden muss. Er ist so gebaut,
dass er mit einem einzigen Neustart auskommt.

Die drei Dinge, die man vorher wissen muss:

> **Das Image auf dem Gerät wurde nicht selbst gebaut.** Die
> Originalmodule unter `/usr/lib/modules/5.10.16.rt30.pixy-2/extra/`
> werden nie überschrieben; der Nachbau liegt unter `/root/mvb-new` und
> wird mit `insmod` geladen. Rückweg ist ein Neustart.
>
> **`systemctl stop session-1.scope` ist nicht umkehrbar** — `target`
> und `extApp` starten ohne Neustart nicht wieder.
>
> **`IVR0`/`IVR1` niemals von Hand lesen**, solange ein Stack läuft: das
> quittiert Interrupts und stiehlt sie dem LLI.

Der Aufbau in Kurzform:

| Stufe | Konstellation | prüft |
|---|---|---|
| 0 | Herstellerstand läuft | Referenzaufnahme `snap0` |
| 1 | **Nachbau-Board** + Original-LLI | `K*`-ABI, Lese- und Schreibpfad; `snap2` ist der Sollwert |
| 2 | Nachbau + Nachbau | `diff snap2 snap3` muss leer sein |
| 3 | mit der Originalanwendung, eigener Neustart | echter Busverkehr, nur am Prüfgerät |

## Am Gerät belegt

Stand des Gerätetests am Laborbus. Geprüft wurde mit `tools/mvbdiff.py`
gegen die Telegramme, in denen dieses Gerät (`IDU2`) Senke ist.

### Empfangspfad — quantitativ bestätigt

Elf Senkenports empfangen. Entscheidend sind die drei Lebenszeichen von
drei verschiedenen Quellen, weil sich ihr Inhalt nachrechnen lässt:

| Port | Signal | Quelle | Zyklus | gelesen | Zuwachs je 20 s | erwartet |
|---|---|---|---|---|---|---|
| 181 | `IN-XMVBLifeSFLG1` | FLG1N | 64 ms | `C4A9`→`C5E2`→`C719` | +313, +311 | 312,5 |
| 471 | `DDA1-XMVBLifeSig` | DDA1 | 64 ms | `C470`→`C5A8`→`C6E0` | +312, +312 | 312,5 |
| 460 | `NC-XTimeDate` | DIA1 | 64 ms | `1E4184B6`→`…CA`→`…DE` | +20, +20 | 20 (Sekunden) |

Die beiden Lebenszeichen zählen **auf den Schritt genau** einmal je
64-ms-Zyklus hoch, und die Uhr des Diagnoserechners läuft in
Echtzeit mit. Damit ist belegt: Frames werden empfangen, im richtigen
Dock abgelegt, die Seitenumschaltung über `VP` greift, und kein Zyklus
geht verloren.

Nebenbei bestätigt: die Nutzdaten werden unverändert durchgereicht. Die
Lebenszeichen sind big-endian `WORD` nach MVB-Konvention, der Zeitstempel
ein little-endian `u32` der Anwendung — der Treiber dreht an keinem von
beiden.

`freshness` skaliert mit der Zykluszeit: 64 ms → ~29, 256 ms → ~150,
1024 ms → ~646. Die Rechnung `(0xFFFF - tack) << tmo_shift` liefert also
über alle Portklassen hinweg ein vergleichbares Alter.

### Ebenfalls am Gerät bestätigt

* Alle 22 ioctls angesprochen, Rückgaben protokolliert.
* `PD_CONF` mit zwölf Ports über alle fünf Größenklassen; `STSR` ergibt
  den vorausberechneten Wert.
* `write()` auf einen Quellport, zweimal, mit Seitenumschaltung.
* `DISABLE_PORT` wirkt: der abgeschaltete Port liefert danach `EIO`.
* Exklusivität: das zweite `open()` liefert `EBUSY`.
* `MVB_GO` und `MVB_STOP` am laufenden Bus.

### Interrupts — verhalten sich wie beim Original

Gleicher Lauf (`mvbdiff.py --go --no-pd`) mit beiden LLIs auf demselben
Board-Treiber, dessen `dbg_hardirq` damit für beide gleich zählt:

| | Nachbau | Original |
|---|---|---|
| `IMR0` / `IMR1` nach `START` | `0003` / `0880` | `0003` / `0880` |
| `IPR1` vor → nach 20 s | `0000` → `0080` | `0000` → `0080` |
| `ISR0` / `ISR1` | `0000` / `0000` | `0000` / `0000` |
| Interrupts beim Board-Treiber | 0 | 0 |

`FEV` wird also *nach* dem Freigeben der Maske gesetzt, und trotzdem
landet es weder in `ISR` noch als Interrupt beim Treiber — bei beiden
Modulen. Die Vermutung, eine vor dem Freigeben anstehende Quelle
blockiere die Meldung, ist damit widerlegt, die dafür eingebaute
Quittung wieder entfernt. Für die Anwendung ohne Belang: sie arbeitet
im Pollbetrieb (`strace`: zyklisches `MD_GET_STATUS`).

### ioctl-ABI — gegen den Maschinencode nachgezogen

Der Vergleichslauf zeigte `WRITE_DEV_ADDR`: Original `EINVAL`, Nachbau
`ok`. Die Ursache stand nicht im Dekompilat, sondern nur im
Disassembly: in den abgespaltenen Fallfunktionen des ioctl-Verteilers
hat Ghidra die Register falsch zugeordnet. Der Verteiler wurde deshalb
Fall für Fall gegen den Maschinencode gelesen. Befunde:

| ioctl | Original | Nachbau bisher |
|---|---|---|
| `WRITE_DEV_ADDR` | Adresse **als Wert** im Argument, `arg == 0` → `EINVAL` | Zeiger |
| `WRITE_DSW` | **Wert**: oberes Wort Maske, unteres Wert | Zeiger |
| `DISABLE_PORT` | **Wert**; nur auf Quellen, löscht nur die Typbits im PCS, sonst `EIO` | Zeiger; PIT und PCS genullt, jeder Port |
| `MD_GET_STATUS` | prüft das eingelesene Wort, übergibt als selector/reset aber **den Zeiger** (Bits 31..16 / 15..0); schreibt **2 Byte** | Wort ignoriert, alles ausgewählt, nichts zurückgesetzt, 4 Byte |
| `HWINIT` | `ts_type ≠ 1` → `EIO`; Grenzen aus der TM-Größe (`0xfff`/`0x3ff`), nicht vom Aufrufer; `ownership == 1` leert PIT und PCS; `DSW &= ~2` | nichts davon |
| `REC_CONF`/`REC_DEL` | vorhanden, zwei Plätze, DTI 7 im PCS, `IMR0` Bit 6 | `EPERM` |
| `PD_CONF` | leere Liste gültig (`STSR 0x1004`); vor jeder weiteren Konfiguration und nach Fehlern PIT/PCS leeren | leere Liste `EINVAL`, nie geleert |
| `MD_FLUSH_QUEUE` | Argument ≠ 0 → `EINVAL` | ignoriert |
| unbekannte Nummer ≤ 22 | `EINVAL` | `ENOTTY` |

Die ersten drei hätten die Herstelleranwendung auf dem Nachbau
scheitern lassen: sie übergibt Werte, der Nachbau hätte sie als Zeiger
gelesen und `EFAULT` geliefert.

`MD_GET_STATUS` ist ein Fehler im Original und wird **bewusst
nachgebaut**: welche Statusbits die Anwendung sieht und welche dabei
zurückgesetzt werden, hängt an der Stapeladresse ihres Puffers. Die
Anwendung läuft seit Jahren genau so; ein korrigiertes Verhalten wäre
ein anderes.

Die Ereignisaufzeichnung schreibt im Original in Ringpuffer, die
`read()` nie ausliefert. Nachgebaut ist, was nach außen sichtbar ist:
Rückgabewerte, das DTI-Feld im PCS und `IMR0`; der unerreichbare
Ringpuffer nicht.

Der Absturz des Originals bei `PD_CONF` in der ersten Vergleichsrunde
(`Killed`) war eine Folge von `HWINIT` mit `ts_type = 0`: das
schlägt fehl, und `PD_CONF` arbeitet dann auf einer nie angelegten
Traffic-Store-Beschreibung.

`tests/tm_replay.c`, Test 12, prüft alle Punkte der Tabelle.

### Offen

**Message-Daten.** Senden und Empfangen sind am Gerät noch nicht
geprüft.

## Gegenlesen gegen das Dekompilat

Beide Module wurden Funktion für Funktion gegen das Ghidra-Dekompilat der
Originale gehalten. Was dabei gefunden und behoben wurde, in der
Reihenfolge der Tragweite:

### 1. Service Area liegt während der Initialisierung woanders

`mvb_config` im Original adressiert den gesamten Registersatz über
**`TM + 0x3C00`** — den Grundplatz der Service Area — und schiebt
`p_sa` erst nach dem Schreiben von `MCR` an den zur Speichergröße
passenden Platz (`TM + 0xFC00` bei `mcm = 3`). Der MVBC benutzt bis
dahin seine Einschaltbelegung. Deshalb schreibt das Original `SCR`
eingangs auch an *jeden* in Frage kommenden Ort.

Der Nachbau hat von Anfang an über `0xFC00` gearbeitet. Auf der echten
Karte wären damit sämtliche Initialisierungszugriffe ins Leere gegangen
und der Schleifentest hätte fehlgeschlagen — **Stufe 2 wäre gescheitert**.

### 2. `mvb_hardw_config` war unvollständig

Es fehlten das TMO-Feld im `SCR` (43 µs Antwortfenster), das Löschen von
`SLM` im `DR` für Zweileitungsbetrieb und `mvb_set_laa_rld()`. Statt
dessen wurden `STSR` und `TCR` geschrieben, was das Original dort gar
nicht tut. Neu dazugekommen sind `mvb_set_device_status_word()`,
`mvb_set_laa_rld()` und `mvb_reset_rlds()`.

Damit ist auch die offene Frage nach `TCR = 0x0022` beantwortet: das LLI
schreibt `TCR` **nie** — außer Bit `TA2` beim Warten. `RS1|RS2` ist die
Einschaltbelegung des Controllers.

### 3. Der Oberbautreiber meldet sich selbst an

Das originale LLI sucht beim Laden `/dev/mvb0..2` ab und ruft für jede
Karte, die sich öffnen lässt, sofort `mvb_add_board()` auf; das
Abonnement beim Board-Treiber deckt nur *spätere* Ereignisse ab. Der
originale Board-Treiber meldet beim Abonnieren folglich **nichts** über
bereits vorhandene Karten.

Der Nachbau hatte es umgekehrt: der Board-Treiber meldete sofort, das LLI
verließ sich darauf. Das koppelte beide Module aneinander — genau das,
was die paarweise Mischprobe unmöglich gemacht hätte. Beide Seiten sind
jetzt auf das Verhalten des Originals umgestellt.

### 4. `WRITE_CONTROL` war weitgehend falsch

`mvb_ctrl.t_ignore` ist eine **Zeitangabe in Mikrosekunden**, keine
Feldnummer; sie wird über Schwellen auf das TMO-Feld abgebildet
(1…31 → 21 µs, 32…52 → 43 µs, 53…74 → 64 µs, 75…255 → 83 µs, ≥ 256
lässt den Wert stehen). Die Kommandobits `sla`/`slb` wählen den
Leitungsbetrieb, `cla`/`clb` setzen die Fehlerzähler zurück. Nichts
davon war umgesetzt.

### 5. `WRITE_DSW` trägt eine Maske im oberen Wort

Das `uint32_t`-Argument ist `Maske << 16 | Wert`; nur die maskierten Bits
werden verändert. Der Nachbau hat das obere Wort verworfen und den
ganzen Status überschrieben.

### 6. Fehlerzähler je Leitung

`SA + 0x3D0` und `SA + 0x3D4` sind die Fehlerzähler für Leitung A und B.
`READ_STATS` hat sie bisher hart auf null gemeldet.

> **Die Namen `MVBC_ECA`/`MVBC_ECB` sind unsere, nicht die des
> Herstellers.** `mvbc.h` führt genau diesen Bereich als reserviert:
>
> ```c
> VOL TM_TYPE_WORD    dmy__50[4];    /* INT_REGS + 0x50 … 0x56 */
> VOL TM_TYPE_WORD    daor;          /* INT_REGS + 0x58        */
> ```
>
> Der Herstellertreiber liest dort aber in `mvb_handle_counter()` die
> beiden Zähler (`+0x50` lesen = Typ 3, `+0x54` lesen = Typ 4, dazu die
> Löschpfade 0, 5, 6 und 7). Der MVBC02D implementiert an dieser Stelle
> also mehr, als der allgemeine Header dokumentiert. Der Treiber ist die
> belastbarere Quelle dafür, was der Chip kann — aber wer nur `mvbc.h`
> liest, findet die beiden Register nicht.

### 7. `IVR0`/`IVR1` statt `ISR0`/`ISR1`

`mvb_config` leert die Interrupt-**Vektor**register, nicht die
Statusregister. Der Nachbau hatte die falschen beiden erwischt.

### 8. `mvb_wait` benutzt den Zähler des MVBC

Das Original wartet über `TR2`/`TC2`/`TCR.TA2`, nicht über die
Kernel-Uhr; ein Zählwert entspricht acht Schritten. `mvb_wait(2000)`
sind also 16000 Controllertakte, nicht 2 ms. Der Nachbau hat
`usleep_range()` benutzt und damit potenziell deutlich zu kurz gewartet.
Einzige bewusste Abweichung: eine Zählschranke gegen ein Festhängen,
falls der Zähler nicht läuft.

### 9. Fehlercodes und Zustandsprüfungen

| Stelle | Original | Nachbau vorher |
|---|---|---|
| `read()` ohne `MVB_GO` | `ENETDOWN` | `ENOTCONN` bei fehlendem `has_pd` |
| `write()` ohne `private_data` | `ENETDOWN` | `ENODEV` |
| `START` ohne Geräteadresse | `EINVAL` | `EPERM` bei fehlendem `has_pd` |
| `STOP` bei gestopptem Controller | `ENETDOWN` | `EPERM` |
| `RETRIGGER` auf MVBC02D | `EINVAL` | stillschweigend `0` |
| `REC_CONF` / `REC_DEL` | `EPERM` | `ENOSYS` |
| `MD_NSDB` / `BA_NSDB` | `EINVAL` | `ENOSYS` |
| `HWINIT`, `WRITE_DEV_ADDR` bei laufendem Controller | `EFAULT` | keine Prüfung |
| `MD_CONF` bei laufendem Controller | `EALREADY` | keine Prüfung |
| `open()`, Initialisierung schlägt fehl | `EBADF` | `ENODEV` |
| `arg == NULL` | `EINVAL` | `EFAULT` |
| fehlgeschlagenes `copy_to_user` | `EINVAL` | `EFAULT` |
| Abonnement voll / unbekannt | `EFAULT` | `ENOMEM` / `ENXIO` |

### 10. Kleinigkeiten

`has_md` kommt aus `MD_CONF`, nicht aus der Initialisierung.
`READ_STATS` liefert `line_config` und `t_ignore` aus dem gespeicherten
Status, nicht aus `media_type` und dem `SCR`. `mvb_set_device_address`
prüft die Adresse zurück. `cdev_add` registriert 255 Minor je Karte.
`mvb_config` legt zusätzlich `EFS`, `FC8` und die Anfangsbelegung des
Device Status Word (`0x0082`) an.

### Unverändert bestätigt

Alle 22 ioctl-Nummern beider Module stimmen mit den gemessenen Werten des
Dekompilats überein, ebenso die Größen von `PixyMvbBoardCoreID` (16 B),
`PixyMvbBoardGPIO` (32 B), `PixyMvbModuleVerStr` (128 B) und
`PixyMvbBoardUpperDrvSubscribe` (36 B = 9 Doppelwörter). `DAOK = 0x0094`
ist als Konstante des Originals belegt; die am Gerät gemessenen `0x00FF`
stammen also nicht aus dem Treiber. Dock-Adressierung, Index-Vergabe,
Wächterelement, Link_Header und die `STSR`-Berechnung waren bereits
richtig.

### Zugriffsbreite auf den Traffic Store — nachgezählt

Die ganze Nachbildung steht und fällt damit, dass der Traffic Store
**ausschließlich 16 Bit breit** angesprochen wird. Beide Dekompilate
wurden deshalb vollständig nach MMIO-Zugriffsfunktionen durchsucht:

| | `pixy-mvb` | `pixy-mvblli` |
|---|---|---|
| `ioread16` | 3 | 0 |
| `ioread32` | 41 | **1** |
| `iowrite32` | 24 | 0 |
| `ioread8/64`, `iowrite8/16/64`, `memcpy_fromio/toio`, `readl/writel/readw/writew` | 0 | 0 |

Kein einziger Treffer liegt im TM-Pfad:

* Die 65 Zugriffe in `pixy-mvb` verteilen sich restlos auf den
  CoreID-Block (`priv+0x2F8`) und den GPIO-Block (`priv+0x300`).
  `mmapISAaddr` wird berechnet, über `KGET_PISA` herausgegeben und
  **in keiner Breite jemals dereferenziert** — der Board-Treiber fasst
  den Traffic Store gar nicht an.
* Der einzige Zugriff in `pixy-mvblli` steht in `mvb_init_board` direkt
  nach `KGET_PGPIO` und liest das GPIO-DAT-Register, um Bit 0
  (`MVB_PC104n`) zu prüfen. Ebenfalls nicht der Traffic Store.

`pixy-mvblli` hat darüber hinaus **auch keine** `ioread16`/`iowrite16`:
der Traffic Store wird dort über einfache `volatile uint16_t *`
angesprochen, so wie `mvbc.h` es vorgibt. Auf x86 erzeugt das dieselben
16-Bit-Zugriffe wie unsere `ioread16`/`iowrite16` — die Breite stimmt
überein, nur der Weg dahin ist ein anderer.

`mvbc.h` bestätigt das von der Typseite: für den Traffic Store gibt es
genau `TM_TYPE_BYTE` (`unsigned char`), `TM_TYPE_WORD` (`unsigned short`)
und `TM_TYPE_RWORD` (`volatile unsigned short`) — 94 Verwendungen von
`TM_TYPE_WORD`, 15 von `TM_TYPE_BYTE`, **keinen einzigen 32-Bit-Typ**.

Zwei Stellen laden zum Fehlschluss ein und sind beide entschärft:

* Das 4-Byte-Raster der Register ist unter `#pragma pack(2)` als
  Registerwort **plus ausdrückliches Füllwort** ausgeschrieben
  (`scr; dmy__02; mcr; dmy__06; …`) — zwei 16-Bit-Plätze, kein
  32-Bit-Register.
* Die Bitfeld-Unionen deklarieren ihre Felder als `unsigned int x : 1`,
  das Datenglied der Union ist aber `TM_TYPE_WORD w`. Das Objekt ist
  16 Bit breit; der `unsigned int` ist nur die Speicherklasse des
  Bitfelds.

## Bekannte bewusste Eigenheiten

Diese Verhaltensweisen sehen nach Fehlern aus, sind aber vom Original
übernommen. Wer sie „repariert", bricht die Kompatibilität:

- **Beim Senden von Message-Daten werden immer 32 Byte kopiert**, auch
  wenn `SZ` weniger als gültig erklärt. Kurze Pakete tragen dadurch Reste
  der vorigen Belegung des Anwendungspuffers auf den Bus.
- **Die ersten vier Byte von `mvb_port.data` werden beim Senden
  überschrieben** (Link_Header). Nutzdaten beginnen bei Offset 4.
  Empfangsseitig bekommt die Anwendung den Link_Header mitgeliefert.
- **Message-Ziele sind auf Adressen < 256 beschränkt**, obwohl `DD` 12 Bit
  breit ist. Broadcast `0xFFF` ist über die Zeichengeräte-ABI damit nicht
  erreichbar.
- **Der Empfangsring verwirft bei Überlauf stillschweigend** — kein
  Fehler, kein Log, nur das Statusbit 4 von `MD_GET_STATUS`.
- **`mmap` setzt die Seiten nicht auf uncached.** Das hat eine scharfe
  Folge für jedes Werkzeug, das die Register über `/dev/mvb0` liest: die
  CPU holt je Zugriff eine ganze 64-Byte-Cachezeile vom Bus. Die
  Registerbasis liegt zeilenbündig bei BAR `0x204FF80`, also

  | Cachezeile | Register |
  |---|---|
  | `0x204FF80` = `SA+0x380…0x3BF` | SCR MCR DR STSR **FC EC MFR MFRE** MR MR2 DPR DPR2 IPR IMR |
  | `0x204FFC0` = `SA+0x3C0…0x3FF` | ISR0 ISR1 **IVR0 IVR1** DAOR DAOK TCR TR1 TR2 TC1 TC2 |

  Jedes Lesen in der zweiten Zeile – auch von `ISR` oder `TCR` – liest
  `IVR0`/`IVR1` mit. Der Controller gibt dabei seine anstehenden
  Interruptquellen heraus, der Treiber sieht sie nie, und weil der MSI
  auf eine Flanke reagiert, bleibt die Leitung oben: **kein Interrupt
  mehr, Message-Empfang tot bis zum Neustart.** Das ist am Gerät mit dem
  Nachbau *und* mit dem Original passiert, verursacht durch
  `irqstate.py`. Das Werkzeug liest diese Zeile deshalb nicht mehr.
  Lesen in der ersten Zeile ist für den Interrupt harmlos, löscht aber
  die vier Zähler `FC`, `EC`, `MFR`, `MFRE` – genau deshalb musste
  `mvbdiff` sie als flüchtig ausblenden. `mvbdiff` selbst darf den
  vollen Registersatz lesen: es läuft nur, wenn der Herstellerstack
  nicht läuft.
- **`open()` initialisiert den Controller, `close()` baut ihn ab.**

## Nicht implementiert

`PD_NSDB`, `MD_NSDB`, `BA_NSDB` liefern `-ENOSYS`, ebenso `REC_CONF` und
`REC_DEL`. Für dieses Gerät ist das belegt und nicht geraten: der
`PD_CONF`-Pfad berechnet `STSR` aus der Portliste als
`(nächster freier Dock-Index) | Intervallfeld`. Gemessen wurden 74 Ports
(42×32 B, 15×16 B, 8×8 B, 5×4 B, 4×2 B) → Index 219 = 0xDB, Intervallfeld
0x1000 → **`STSR = 0x10DB`**, exakt der Wert im laufenden Gerät. Dazu
passt, dass alle 74 Ports `dti = 0` tragen — `PD_CONF` nullt die
Sink-Zeit-Tabelle, `PD_NSDB` würde sie aus der Datenbank füllen.

Sollte sich das je ändern, meldet sich der Fehler deutlich: `ENOSYS` im
`strace`, keine stille Fehlfunktion.
