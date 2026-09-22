# Testplan für den Nachbau auf dem Gerät

Für `pixy-mvb.ko` und `pixy-mvblli.ko`. Gedacht zum Mitlesen am Gerät —
jeder Schritt in der Reihenfolge, in der er ausgeführt werden muss.

## Bevor irgendetwas geladen wird

> **Das Image auf diesem Gerät wurde nicht selbst gebaut.**
> Die Originalmodule unter `/usr/lib/modules/5.10.16.rt30.pixy-2/extra/`
> werden **nie** überschrieben oder verschoben. Der Nachbau liegt unter
> `/root/mvb-new` und wird ausschließlich mit `insmod` geladen. Rückweg
> ist in jedem Fall ein Neustart.
>
> **`systemctl stop session-1.scope` ist nicht umkehrbar.** `target` und
> `extApp` lassen sich danach ohne Neustart nicht wieder starten. Der
> ganze Plan ist deshalb so gebaut, dass er mit **einem** Neustart
> auskommt: Schritt 0 läuft noch mit laufendem Stack, alles danach in
> einem Zug.
>
> **Es geht nichts auf den Fahrzeugbus.** Ohne Anwendung läuft kein
> `PD_CONF`, und `MVB_GO` wird nirgends aufgerufen. Der Controller
> bleibt gestoppt. Stufe 3 ist etwas anderes — dazu unten.
>
> **`IVR0`/`IVR1` niemals von Hand lesen**, solange ein Stack läuft: das
> quittiert Interrupts und stiehlt sie dem LLI. `mvbsnap.py` spart beide
> an jeder Kandidatenadresse aus.

---

## Schritt 0 — Vorbereitung, Stack läuft noch

`/dev/mvb0` ist nicht exklusiv. Alles hier geht im laufenden Betrieb.

```sh
# 1. Nachbau nach /root/mvb-new kopieren, dann prüfen
modinfo /root/mvb-new/pixy-mvb.ko    | grep vermagic
modinfo /root/mvb-new/pixy-mvblli.ko | grep vermagic
modinfo /usr/lib/modules/5.10.16.rt30.pixy-2/extra/pixy-mvb.ko.xz | grep vermagic
```

Alle drei Zeilen müssen **zeichengleich** sein:

```
vermagic:       5.10.16.rt30.pixy-2 SMP preempt_rt mod_unload
```

Weicht eine ab, hier abbrechen — `insmod` würde scheitern und der
Aufwand wäre umsonst.

```sh
# 2. Prüfsummen gegen die Liste in der Übergabe halten
md5sum /root/mvb-new/*.ko

# 3. Originale sichern und entpacken (insmod kann kein .ko.xz)
mkdir -p /root/mvb-orig
cp /usr/lib/modules/5.10.16.rt30.pixy-2/extra/pixy-mvb.ko.xz     /root/mvb-orig/
cp /usr/lib/modules/5.10.16.rt30.pixy-2/extra/pixy-mvblli.ko.xz  /root/mvb-orig/
cd /root/mvb-orig && unxz -k pixy-mvb.ko.xz && unxz -k pixy-mvblli.ko.xz

# 4. Referenzaufnahme des Herstellerstandes — die einzige Gelegenheit
python3 /root/mvbsnap.py /root/snap0-orig-laufend
```

`snap0` ist die **einzige** Aufnahme, die den Herstellerstand zeigt
(74 Ports, `SCR 0x87C7`, `STSR 0x10DB`, `DAOR 0x00F0`). Nach dem ersten
eigenen `open()` ist sie unwiederbringlich weg, weil `mvb_config` die
Port-Index-Tabelle löscht.

Erwartete Kopfzeile in `snap0/summary.txt`:

```
Aktive Service Area: TM+0x0FC00   MCR 2803 (Version 5, mcm 3)
```

---

## Schritt 1 — Stack stoppen

```sh
systemctl stop session-1.scope
fuser -v /dev/mvblli0            # muss leer bleiben
rmmod pixy_mvblli
rmmod pixy_mvb
```

Scheitert ein `rmmod` mit `EBUSY`, hält noch jemand das Gerät — dann
`fuser` nachsehen, nicht mit Gewalt.

---

## Schritt 2 — Stufe 1: eigener Board-Treiber unter dem **originalen** LLI

Das beste Prüfgestell, das es gibt: das Original-LLI spricht nur über
ioctl mit dem Board-Treiber und merkt nicht, wer darunter liegt.

```sh
insmod /root/mvb-new/pixy-mvb.ko
insmod /root/mvb-orig/pixy-mvblli.ko
dmesg | tail -30
ls -l /dev/mvb0 /dev/mvblli0
```

Im `dmesg` erwartet:

```
pixy-mvb v3.0.0 - Pixy-1000 MVB/PC104 Extension Board Driver
pixy-mvb 0000:..:...: CoreID magic 'PIXY' (0x50495859), BAR0 67108864 bytes
pixy-mvb 0000:..:...: MVB Extension Board detected in bus slot .., Fw version code ...
pixy-mvb 0000:..:...: Created device entry /dev/mvb0
pixy-mvb 0000:..:...: Configuring single MSI interrupt...
pixy-mvb 0000:..:...: Set irq #.. for MSI vector 0
pixy-mvb: upper driver 1 subscribed
```

Dazu die gewohnte Meldung des Original-LLI, dass `/dev/mvblli0` angelegt
wurde. **`upper driver 1 subscribed` darf nur einmal erscheinen**, und es
darf **keine** Meldung des Original-LLI kommen, dass Karte 0 bereits
registriert sei — genau das hätte die frühere Fassung ausgelöst.

Sysfs gegenprüfen:

```sh
cat /sys/class/pixy-mvb/mvb0/board_type        # MVB
cat /sys/class/pixy-mvb/mvb0/connector_class   # ESD oder EMD
cat /sys/class/pixy-mvb/mvb0/fw_version        # dieselbe Zahl wie vorher
cat /sys/class/pixy-mvb/mvb0/controller_class
cat /sys/class/pixy-mvb/mvb0/pci_id
```

### 2a — Lesepfad prüfen

```sh
python3 /root/mvbsnap.py /root/snap1-passiv
diff /root/snap0-orig-laufend/summary.txt /root/snap1-passiv/summary.txt
```

Es ist noch kein `open()` gelaufen, also steht das Traffic Memory
unverändert da. Der Vergleich prüft damit genau eine Sache, und die ist
wertvoll: **ob BAR-Mapping, Blockoffsets und `mmap` des Nachbaus
stimmen.**

| Größe | erwartet |
|---|---|
| `MCR`, `DR`, `STSR`, `DAOR`, `DAOK`, `TCR`, QDT | **unverändert** gegenüber `snap0` |
| `la_pit` | unverändert 74 Einträge |
| `SCR` | `0x87C5` statt `0x87C7` — `extApp` hat beim Schließen `mvb_stop` ausgelöst, IL steht auf CONFIG |
| `IMR0`/`IMR1` | `0x0000` statt `0x0003`/`0x0880` — ebenfalls vom Abbau |

Das sind die einzigen zwei erwarteten Abweichungen. Alles andere, was
der `diff` zeigt, ist ein Befund.

### 2b — Schreibpfad prüfen, Sollwert für Stufe 2 aufnehmen

Jetzt mit dem **originalen** LLI ein `open()` ausführen. Sitzung A:

```sh
python3 -c "import os; fd=os.open('/dev/mvblli0', os.O_RDWR); print('offen'); input()"
```

Solange das offen steht, in Sitzung B:

```sh
dmesg | tail -10
python3 /root/mvbsnap.py /root/snap2-orig-open
python3 -c "import os; os.open('/dev/mvblli0', os.O_RDWR)"   # muss EBUSY liefern
```

Dabei schreibt der Originaltreiber seine **gesamte** Initialisierung
durch meinen Board-Treiber ins Traffic Memory. Erwartet in
`snap2/summary.txt`:

| Größe | erwartet | woran es liegt |
|---|---|---|
| Kopfzeile | `Aktive Service Area: TM+0x0FC00 … mcm 3` | `MCR` wurde programmiert |
| `SCR` | TMO-Feld `0x0400` gesetzt (43 µs) | `mvb_hardw_config(2, 1)` |
| `DR` | Bit 0 (`SLM`) gelöscht | Zweileitungsbetrieb |
| `la_pit` | **komplett 0**, keine belegten Einträge | `mvb_config` löscht die Tabelle |
| `STSR` | **nicht** `0x10DB` | ohne Anwendung läuft kein `PD_CONF` |
| `DAOR` / `DAOK` | `0x0000` / `0x0094` | `mvb_set_device_address(ts, 0)` |
| QDT | drei Einträge ≠ 0, neu eingehängt | `mvb_md_q_init` |

Das zweite `open()` muss `OSError: [Errno 16] Device or resource busy`
liefern.

Danach Sitzung A mit Enter schließen.

---

## Schritt 3 — Stufe 2: beide Module aus dem Nachbau

```sh
rmmod pixy_mvblli
rmmod pixy_mvb

insmod /root/mvb-new/pixy-mvb.ko
insmod /root/mvb-new/pixy-mvblli.ko
dmesg | tail -20
```

Beim Laden erwartet — und **nur** das, die Hardware wird hier noch nicht
angefasst:

```
pixy-mvblli v3.0.0 - MVB Link Layer Interface
pixy-mvblli: /dev/mvblli0 attached to board 0
pixy-mvb: upper driver 1 subscribed
```

Dann dasselbe `open()` wie in Schritt 2b. Sitzung A:

```sh
python3 -c "import os; fd=os.open('/dev/mvblli0', os.O_RDWR); print('offen'); input()"
```

Sitzung B:

```sh
dmesg | tail -10
python3 /root/mvbsnap.py /root/snap3-neu-open
python3 -c "import os; os.open('/dev/mvblli0', os.O_RDWR)"   # muss EBUSY liefern
```

Im `dmesg` erwartet:

```
pixy-mvblli: traffic memory 256 KiB (mcm=3) at ISA+0x40000
pixy-mvblli: controller 0 initialized (MVBC02D ESD)
```

Und dann die eigentliche Prüfung des ganzen Unterfangens:

```sh
diff /root/snap2-orig-open/summary.txt /root/snap3-neu-open/summary.txt
```

**Kein Unterschied heißt: der Nachbau hinterlässt denselben
Controllerzustand wie das Original.** Das ist das Abnahmekriterium.

---

## Wenn Stufe 2 scheitert

Die drei Abbruchmeldungen des LLI und was sie bedeuten:

| `dmesg` | Bedeutung |
|---|---|
| `cannot determine traffic memory size` | die Fenstersondierung über `BASR0`/`BASR1`/`BCR` greift nicht — Problem im Board-Treiber oder im ISA-Block |
| `unexpected MVBC version` | `MCR >> 11` ist nicht 5 — der Registersatz wird an der falschen Stelle gelesen |
| `MVBC loopback self test failed (0x….)` | Controller und Traffic Memory arbeiten nicht zusammen |
| `SCR does not read back` / `DPR does not read back` | Register antworten nicht |

Für die drei letzten liefert `mvbsnap.py` die Diagnose. `summary.txt`
enthält am Ende den Registerblock an **allen drei** Kandidatenadressen:

```
Registerblock an TM+0x03C00
Registerblock an TM+0x07C00
Registerblock an TM+0x0FC00   <-- aktiv
```

Steht nach dem `open()` nur bei `TM+0x03C00` etwas Sinnvolles und die
Kopfzeile meldet `mcm 0` oder gar keine gültige Service Area, dann ist
die Initialisierung stehengeblieben, bevor `MCR` programmiert wurde.
Fehlt umgekehrt überall ein plausibles `MCR` (Version 5), antwortet der
Controller gar nicht.

In beiden Fällen: `snap3` und den `dmesg`-Auszug sichern, dann Schritt 5.

---

## Schritt 4 — Zurück zum Original

```sh
reboot
```

Im Dateisystem wurde nichts verändert; der Herstellerstand lädt ganz
normal wieder. Bei einer FPGA-Karte ist ein echter Aus-/Einschaltvorgang
die sauberere Wahl und schadet nichts.

---

## Stufe 3 — mit der Originalanwendung

Erst wenn Stufe 2 durch ist, und mit einem eigenen Neustart. **Ab hier
geht echter Verkehr auf den Bus** — nur auf einem Prüfgerät, nie im
Fahrzeug.

Nach dem Neustart die eigenen Module laden, **bevor** `session-1.scope`
startet, oder die Anwendung von Hand starten. Dann:

```sh
mvb_pd_tool ...            # Prozessdaten lesen
mvb_messagedata_tool ...   # Message-Daten gegen ein zweites Gerät
```

Worauf dabei besonders zu achten ist:

* `STSR` muss wieder `0x10DB` werden, sobald die Anwendung ihre 74 Ports
  konfiguriert hat — das ist die Probe auf die Index-Vergabe.
* `DAOR` muss wieder `0x00F0` zeigen (Geräteadresse 240).
* `READ_STATS` liefert `frames` und `errors` aus den Zählern des MVBC.
  Ob die Anwendung dort fortlaufende oder aufsummierte Werte erwartet,
  ist aus dem Dekompilat nicht zu entscheiden; der Nachbau weist zu.
  Springende oder unplausible Zählerstände wären genau dieser Fall.
