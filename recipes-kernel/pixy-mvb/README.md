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

`SCR`, `MCR`, `DR`, `STSR`, `DAOR`, `DAOK` und `TCR` müssen wieder die
Werte von vorher zeigen, und `la_pit` wieder 74 Einträge.

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
