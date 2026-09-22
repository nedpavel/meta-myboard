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

## Auf dem Gerät testen

> **Vorher lesen.** Das Image auf dem Gerät wurde nicht selbst gebaut.
> Die Originalmodule unter `/usr/lib/modules/5.10.16.rt30.pixy-2/extra/`
> werden **nie** überschrieben. Eigene Module liegen unter `/root` und
> werden mit `insmod` geladen. Rückweg ist in jedem Fall ein Reboot.
>
> `session-1.scope` (`target` + `extApp`) lässt sich stoppen, aber nicht
> wieder starten. Jeder Test kostet also einen Reboot. Deshalb in der
> unten stehenden Reihenfolge arbeiten und pro Reboot so viel wie möglich
> abhaken.

### Vorbereitung

```sh
# Originale sichern (nur lesen, nicht verschieben)
mkdir -p /root/mvb-orig
cp /usr/lib/modules/5.10.16.rt30.pixy-2/extra/pixy-mvb.ko.xz     /root/mvb-orig/
cp /usr/lib/modules/5.10.16.rt30.pixy-2/extra/pixy-mvblli.ko.xz  /root/mvb-orig/

# Nachbau daneben legen
mkdir -p /root/mvb-new
# pixy-mvb.ko und pixy-mvblli.ko hierher kopieren

# Original entpacken, damit es mit insmod geladen werden kann
cd /root/mvb-orig && unxz -k pixy-mvb.ko.xz && unxz -k pixy-mvblli.ko.xz
```

### Stufe 1 — eigener Board-Treiber unter dem **originalen** LLI

Das ist das beste Prüfgestell, das es gibt: das Original-LLI koppelt über
ioctl und merkt nicht, wer darunter liegt. Läuft der Stack damit an, ist
die gesamte `K*`-ABI korrekt.

```sh
systemctl stop session-1.scope        # ab hier kein Rückweg ohne Reboot
rmmod pixy_mvblli
rmmod pixy_mvb

insmod /root/mvb-new/pixy-mvb.ko
insmod /root/mvb-orig/pixy-mvblli.ko

dmesg | tail -30
ls -l /dev/mvb0 /dev/mvblli0
```

Erwartet im `dmesg`:

```
pixy-mvb v3.0.0 - Pixy-1000 MVB/PC104 Extension Board Driver
pixy-mvb 0000:..:...: CoreID magic 'PIXY' (0x50495859), BAR0 67108864 bytes
pixy-mvb 0000:..:...: MVB Extension Board detected in bus slot ..., Fw version code ...
pixy-mvb 0000:..:...: Created device entry /dev/mvb0
pixy-mvb 0000:..:...: Configuring single MSI interrupt...
pixy-mvb 0000:..:...: Set irq #... for MSI vector 0
```

und vom Original-LLI die gewohnte Meldung, dass `/dev/mvblli0` angelegt
und der Controller initialisiert wurde.

Prüfpunkte:

```sh
cat /sys/class/pixy-mvb/mvb0/board_type        # MVB
cat /sys/class/pixy-mvb/mvb0/connector_class   # ESD oder EMD
cat /sys/class/pixy-mvb/mvb0/fw_version        # dieselbe Zahl wie vorher
cat /sys/class/pixy-mvb/mvb0/controller_class
```

Dann den Registersatz gegen die Referenzmessung halten — mit dem
Dump-Skript aus `tools/`:

```sh
python3 /root/mvbsnap.py /root/snap-stufe1
diff <(grep -A40 'MVBC-Register' /root/snap-stufe1/summary.txt) \
     <(grep -A40 'MVBC-Register' /root/mvbsnap-referenz/summary.txt)
```

Solange noch kein `open()` gelaufen ist, prüft der Vergleich den
**Lesepfad**: `MCR`, `DR`, `STSR`, `DAOR`, `DAOK`, `TCR` und die 74
`la_pit`-Einträge müssen unverändert dastehen. Nur `SCR` und `IMR0`/`IMR1`
weichen ab, weil das Schließen durch `extApp` den Controller gestoppt hat
(`SCR` auf IL = CONFIG, Masken auf 0).

Danach mit dem **originalen** LLI ein `open()` ausführen und erneut
aufnehmen — das ist der Sollwert für Stufe 2 und die einzige Prüfung des
**Schreibpfads**. Erwartet: `SCR` mit gesetztem TMO-Feld für 43 µs,
`MCR` mit `mcm = 3`, `la_pit` komplett null, `DAOR = 0x0000`,
`DAOK = 0x0094`, die drei QDT-Einträge neu eingehängt, `STSR` **nicht**
`0x10DB` (ohne Anwendung läuft kein `PD_CONF`).

### Stufe 2 — beide Module aus dem Nachbau

```sh
rmmod pixy_mvblli
rmmod pixy_mvb

insmod /root/mvb-new/pixy-mvb.ko
insmod /root/mvb-new/pixy-mvblli.ko
dmesg | tail -20
```

Erwartet zusätzlich:

```
pixy-mvblli v3.0.0 - MVB Link Layer Interface
pixy-mvblli: /dev/mvblli0 attached to board 0
```

Jetzt muss `open()` den Controller hochfahren:

```sh
python3 - <<'PY'
import os
fd = os.open("/dev/mvblli0", os.O_RDWR)
print("offen")
input("Enter zum Schliessen")
os.close(fd)
PY
```

Während der Datei-Deskriptor offen ist, in einer zweiten Sitzung:

```sh
dmesg | tail -5      # "traffic memory 256 KiB (mcm=3) at ISA+0x40000"
                     # "controller 0 initialized (MVBC02D ...)"
python3 /root/mvbsnap.py /root/snap-stufe2
```

Ein zweites `open()` muss `EBUSY` liefern — das ist die Exklusivität des
Originals:

```sh
python3 -c "import os; os.open('/dev/mvblli0', os.O_RDWR)"
# OSError: [Errno 16] Device or resource busy
```

### Stufe 3 — mit der Originalanwendung

Erst nachdem Stufe 2 durch ist. Ein Reboot stellt den Herstellerstand
wieder her; danach die eigenen Module laden **bevor** `session-1.scope`
startet, oder die Anwendung von Hand starten.

```sh
mvb_pd_tool ...            # Prozessdaten lesen
mvb_messagedata_tool ...   # Message-Daten gegen ein zweites Gerät
```

### Zurück zum Original

```sh
reboot
```

Es wurde nichts im Dateisystem verändert, das Original wird ganz normal
wieder geladen.

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
- **`mmap` setzt die Seiten nicht auf uncached.**
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
