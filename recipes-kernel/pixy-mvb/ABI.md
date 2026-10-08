# Die ABI der beiden Kernelmodule

Referenz für den Bau einer eigenen API auf `pixy-mvb` und `pixy-mvblli`,
als Ersatz für `libpixymvb.so` und `libmvbrtp.so`. Alles hier Beschriebene
ist aus den Treiberquellen entnommen und am Gerät bestätigt.

Wo Verhalten vom Original abweicht oder nach einem Fehler aussieht, steht
es ausdrücklich dabei. Die Treiber sind ABI-gleich zum Hersteller, also
gelten diese Eigenheiten für dessen Bibliotheken genauso.

## Was wo liegt

```
eigene API                        ← das baust du
  │  open  ioctl  read  write  poll  mmap
  ▼
/dev/mvblli0        /dev/mvb0
pixy-mvblli.ko      pixy-mvb.ko
  │                   │
  │ MVB-Controller    │ PCI, BAR0, MSI
  └──────┬────────────┘
         ▼
   Pixy-1000 (FPGA, PCI 1204:EC30)
```

| | `pixy-mvb` | `pixy-mvblli` |
|---|---|---|
| Gerät | `/dev/mvb0`, Symlink `/dev/mvb` | `/dev/mvblli0`, Symlink `/dev/mvblli` |
| Zugriff | beliebig viele Benutzer | **ausschließlich**, zweites `open()` gibt `EBUSY` |
| Aufgabe | Karte finden, BAR abbilden, Interrupts verteilen | MVB-Controller bedienen, Prozess- und Message-Daten |
| Für die API | fast nie nötig | der eigentliche Zugang |

Die Symlinks legt udev an (`40-mvb.rules`, `41-mvblli.rules`). Fehlen sie,
scheitert ein Programm, das `/dev/mvblli` öffnet, mit `ENOENT`, bevor der
Treiber etwas davon sieht.

## Normbezug

`pixy-mvblli` deckt die **Link-Layer-Schicht** nach IEC 61375-1 ab, also
die `lm_*`-Primitiven aus Figure 122. Was die Norm darüber ansiedelt,
fehlt und muss in die eigene API:

| Schicht | Wer |
|---|---|
| Anwendung | deine Anwendung |
| Transport / Network (`MTC`, `conn_ref`, Fragmentierung, `final`/`origin`) | **deine API** (heute `libmvbrtp.so`) |
| Link Layer (`lm_*`), Traffic Store, Ports, Queues | `pixy-mvblli.ko` |
| Physical Layer, MVBC02D, Interrupts | `pixy-mvb.ko` + FPGA |

Der Treiber überträgt **nur Einzelframes von 32 Byte**. Fragmentierung und
Zusammensetzen längerer Nachrichten gehören in die API — mehr dazu unter
„Message-Daten".

WTB kommt auf dieser Karte nicht vor. Der MVBC02D spricht MVB; WTB-Signale
erreichen das Gerät als gewöhnliche MVB-Prozessdaten über ein Gateway.

## `/dev/mvb0` — Board-Treiber

### mmap

```c
int fd = open("/dev/mvb0", O_RDWR);
void *bar = mmap(NULL, 0x4000000, PROT_READ | PROT_WRITE,
                 MAP_SHARED, fd, 0);     /* vm_pgoff wählt den Startpunkt */
```

Abgebildet wird das ganze BAR0, 64 MiB:

| Block | Offset im BAR |
|---|---|
| COREID (Kennung, Firmwarestand) | `0x0000000` |
| GPIO (Bestückung, Betriebsart, JTAG) | `0x1000000` |
| ISA (Fensterregister, Traffic Memory) | `0x2000000` |
| SPARE (unbestückt, liest `0xFF`) | `0x3000000` |

Im ISA-Block: `BASR0` bei `+0x320`, `BASR1` bei `+0x322`, `BCR` bei
`+0x324`; das Traffic Memory beginnt bei `ISA + 0x40000`.

### Cachezeilen — unbedingt lesen

`mmap` setzt die Seiten **nicht** auf uncached, wie das Original. Die CPU
holt deshalb je Zugriff eine ganze 64-Byte-Zeile vom Bus. Die
MVBC-Registerbasis liegt zeilenbündig bei BAR `0x204FF80`:

| Cachezeile | Register darin |
|---|---|
| `0x204FF80` = `SA+0x380…0x3BF` | SCR MCR DR STSR **FC EC MFR MFRE** MR MR2 DPR DPR2 IPR IMR |
| `0x204FFC0` = `SA+0x3C0…0x3FF` | ISR0 ISR1 **IVR0 IVR1** DAOR DAOK TCR TR1 TR2 TC1 TC2 |

**Lies aus der zweiten Zeile nichts**, solange der Treiber Interrupts
benutzt — auch nicht `ISR` oder `TCR`. Jeder Zugriff dort liest `IVR0` und
`IVR1` mit, der Controller gibt seine anstehenden Interruptquellen heraus,
der Treiber sieht sie nie, und weil der MSI auf eine Flanke reagiert,
kommt nie wieder ein Interrupt: Message-Empfang tot bis zum Neustart. Am
Gerät mehrfach so erlebt.

Das gilt auch ohne Herstellerstack: das eigene LLI braucht die Interrupts
ebenso (`FEV`, `DTI1`, `DTI2`).

Lesen in der ersten Zeile ist für den Interrupt harmlos. Es löscht `FC`
und `EC` **nicht** — am Gerät gemessen, `FC` zählt zwischen zwei
Lesungen gleichmäßig weiter. `MFR`/`MFRE` sind nicht geprüft.

### ioctls

Magic `'L'`, Argument ist ein Zeiger auf den angegebenen Typ.
`ENOTTY` bei fremdem Magic oder Nummer > 21, `ENODEV` wenn die Karte nicht
bereit ist.

| Nr | Name | Typ | Liefert |
|---|---|---|---|
| 0 | `GET_MODULE_INFO` | `PixyMvbModuleVerStr` | Versionszeile des Treibers |
| 1 | `GET_COREID` | `PixyMvbBoardCoreID` | Magic, Modell, HW-Index, Epoch, CodeID |
| 2 | `GET_GPIO` | `PixyMvbBoardGPIO` | alle acht GPIO-Register |
| 3 | `SET_GPIO_DAT` | `uint32_t` | setzt Bits in DAT |
| 4 | `CLR_GPIO_DAT` | `uint32_t` | löscht Bits in DAT |
| 5 | `GET_GPIO_DAT` | `uint32_t` | DAT |
| 6 | `SET_GPIO_DIR` | `uint32_t` | schreibt DIR |
| 7 | `GET_BAR0_SIZE` | `size_t` | `0x4000000` |
| 8 | `GET_MAGIC_NUMBER` | `uint32_t` | `0x50495859` = `'PIXY'` |
| 9 | `GET_FW_VERSION` | `uint32_t` | CodeID der FPGA-Ladung |
| 10…13 | `GET_*_OFFSET` | `uint32_t` | Offsets der vier Blöcke |
| 14 | `SET_SW_RESET` | — | FPGA-Reset über GPIO |
| 15 | `SET_FW_RELOAD` | — | FPGA neu laden |

Die Nummern 16…21 (`KGET_PISA`, `KSET_IRQ_SERVER`, `KSET_DRV_SUBSCRIBE` …)
sind **kernelintern**. Ihr Argument ist ein Kernelzeiger; `pixy-mvblli`
benutzt sie, aus dem Userspace sind sie unbrauchbar.

Nützliche Bits in `GPIO.DATreg`: `0x01` gesetzt heißt MVB-Karte (sonst
PC104), `0x02` gesetzt heißt EMD (sonst ESD).

### sysfs

Unter `/sys/class/pixy-mvb/mvb0/` stehen ohne jeden `ioctl` lesbar:
`board_type`, `controller_class`, `connector_class`, `sensibility`,
`fw_epoch`, `fw_version`, `hw_revision`, `vendor`, `device`,
`subsystem_vendor`, `subsystem_device`, `class`, `pci_id`.

## `/dev/mvblli0` — Link Layer Interface

### Lebenszyklus

`open()` **initialisiert den Controller** und `close()` **baut ihn ab**.
Das ist kein gewöhnliches Zeichengerät:

| | Was passiert |
|---|---|
| `open()` | andocken an `/dev/mvb0`, TM-Größe ermitteln, `mvb_config()`, Antwortfenster, Message-Ringe einhängen, Interruptquellen freigeben. **`la_pit` und `da_pit` werden dabei vollständig genullt** — jede bestehende Portkonfiguration ist weg. |
| zweites `open()` | `EBUSY` |
| `close()` | Interruptmaske löschen, `mvb_stop()`, `SCR = 0`. Der Controller geht in RESET, der Registerblock springt an den Grundplatz `TM+0x3C00` zurück. |

Für einen Dienst heißt das: Das Gerät einmal öffnen und offen halten. Wer
es zwischendurch schließt, nimmt dem Fahrzeugbus die Station.

### Modulparameter

| Parameter | Vorgabe | Wirkung |
|---|---|---|
| `irq` | 7 | ISA-Interruptnummer im `BCR`. **Über 0 heißt: empfangene Messages kommen nur über den Interrupt.** `read()` ruft den Verteiler dann nicht selbst auf, und nur dann meldet `poll()` überhaupt etwas. `irq=0` schaltet auf Abfragebetrieb. |
| `irq_rearm` | 1 | `IVR1`/`IVR0` wiederholt leeren, bis ein Durchgang leer bleibt. `0` = Verhalten des Originals. |
| `dbg_*` | — | nur lesbare Zähler: `dbg_irq`, `dbg_dti1`, `dbg_dti2`, `dbg_fev`, `dbg_rqe`, `dbg_other`, `dbg_rearm` |

### ioctls

Magic `'L'`. `ENODEV` wenn nicht initialisiert, `ENOTTY` bei fremdem Magic
oder Nummer > 22, **`EINVAL` für jede nicht belegte Nummer** — nicht
`ENOTTY`, so wie im Original. `EALREADY` heißt durchweg „geht nur, solange
der Controller nicht läuft".

| Nr | Name | Argument | Bemerkung |
|---|---|---|---|
| 1 | `READ_DEV_ADDR` | Zeiger, 2 Byte heraus | Geräteadresse aus `DAOR` |
| 2 | `WRITE_DEV_ADDR` | **Wert**, 1…0xFFF | siehe Fallstricke. `EFAULT` wenn der Controller läuft |
| 3 | `READ_DSW` | Zeiger, 2 Byte heraus | Gerätestatuswort (physischer Port FC15) |
| 4 | `WRITE_DSW` | **Wert**: oberes Wort Maske, unteres Wert | nur maskierte Bits ändern sich |
| 5 | `START` | — | `MVB_GO`. Verlangt eine Adresse 1…0x1000, **keine** fertige Portliste. `EALREADY` wenn schon aktiv |
| 6 | `STOP` | — | `ENETDOWN` wenn nicht aktiv |
| 7 | `RETRIGGER` | — | auf dem MVBC02D immer `EINVAL` (Watchdog des MVBC1S) |
| 8 | `READ_STATS` | Zeiger auf `mvb_stat` | Zähler summieren Überlaufperioden und laufenden Hardwarestand |
| 9 | `REC_CONF` | Zeiger auf `mvb_rec_event` | Ereignisaufzeichnung, zwei Plätze |
| 10 | `REC_DEL` | Zeiger auf `mvb_rec_event` | |
| 11 | `PD_NSDB` | Zeiger auf `mvb_config_nsdb` | prüft die Eingabe und liefert dann **`ENOSYS`** |
| 12 | `PD_CONF` | Zeiger auf `PixyMvblliConfigPorts` | Portliste, siehe unten |
| 13 | `MD_NSDB` | — | nicht im Verteiler, `EINVAL` |
| 14 | `MD_CONF` | Zeiger auf `PixyMvblliConfigMex` | setzt die Sendequeue-Priorität, schaltet Message-Daten frei |
| 15 | `BA_NSDB` | — | nicht im Verteiler, `EINVAL` |
| 16 | `DISABLE_PORT` | **Wert**, Portadresse ≤ 0xFFF | nur Quellports, setzt sie passiv |
| 17 | `READ_TM` | Zeiger auf `mvb_tm` | `ts_id`, `size_id` = `mcm`. **`address` ist ein Kernelzeiger und im Userspace nutzlos** — das TM erreicht man über `mmap` auf `/dev/mvb0` |
| 18 | `MD_FLUSH_QUEUE` | kein Argument | ein Argument ungleich 0 gibt `EINVAL` |
| 19 | `MD_GET_STATUS` | besonders, siehe Fallstricke | |
| 20 | `WRITE_CONTROL` | Zeiger auf `mvb_ctrl` | Adresse, Antwortfenster, Leitungsbetrieb, Zähler löschen |
| 21 | `HWINIT` | Zeiger auf `PixyMvblliConfigLpTs` | `ts_type` muss 1 sein, sonst `EIO` |
| 22 | `USERS` | Zeiger, 4 Byte heraus | Zahl der offenen Benutzer |

#### HWINIT

```c
PixyMvblliConfigLpTs ts = {
    .pb_mwd         = NULL,
    .ownership      = 1,    /* 1 = Porttabellen leeren */
    .ts_type        = 1,    /* alles andere: EIO */
    .prt_addr_max   = 0,    /* wird auf 0xFFF gesetzt, Eingabe ignoriert */
    .prt_indx_max   = 0,    /* aus mcm: mcm 3 -> 0x3FF */
    .auto_reset_rld = 0,
};
ioctl(fd, IOCTL_PIXY_MVBLLI_HWINIT, &ts);
```

Setzt außerdem alle Fehlerzähler auf null. Das ist nicht nebensächlich:
Ein gesättigter Framezähler lässt `FEV` nie wieder melden, und ohne
`HWINIT` fasst niemand die Zähler an.

#### PD_CONF

```c
struct {
    uint16_t prt_count;
    PixyMvblliConfigLpPrt prt[N];     /* prt_addr, size, type */
} __attribute__((packed)) cfg;
```

`size` ist 2, 4, 8, 16 oder 32 Byte; `type` ist 0 passiv, 1 Senke,
2 Quelle. Der Treiber vergibt daraus die Dock-Indizes: Start bei 4, die
Portklassen **absteigend nach Größe**, Schrittweite nach belegten Docks
(32 B → 4, 16 B → 2, darunter 1). Innerhalb einer Klasse gilt die
Reihenfolge der Liste. Aus dem höchsten vergebenen Index berechnet er das
Sink-Time-Raster `STSR`.

Eine leere Liste ist gültig. Ein erneutes `PD_CONF` räumt die Tabellen
zuerst leer. Doppelte Portadressen sind ein Fehler, dann wird alles
verworfen und `EIO` gemeldet.

**Quellports bleiben zunächst passiv.** Erst das erste `write()` typisiert
sie. Das verhindert, dass eine Station nach `START` Nullen sendet.

## Prozessdaten

Container für `read()` und `write()`, 38 Byte:

```c
typedef struct mvb_port {
    uint16_t type;      /* 1 = PD, 2 = MD_HIGH, 3 = MD_LOW */
    uint16_t port;      /* Portadresse */
    uint16_t data[16];  /* 32 Byte */
    uint16_t freshness;
} mvb_port;
```

Der `size`-Parameter von `read`/`write` ist die **Portlänge**, nicht
`sizeof(mvb_port)`:

```c
mvb_port p = { .type = 1, .port = 181 };
ssize_t n = read(fd, &p, 4);        /* Port 181 ist 4 Byte lang */
```

Der Treiber liest zuerst die ganze Struktur aus dem Puffer, holt dann die
Daten und schreibt die Struktur zurück. `read` gibt `size` zurück.

| Bedingung | Ergebnis |
|---|---|
| `size` passt nicht zum F-Code des Ports | `EIO` |
| Port nicht konfiguriert | `EIO` |
| Daten älter als die Sink-Time-Schwelle | `ETIMEDOUT` |
| Controller läuft nicht | `ENETDOWN` — **`read` verlangt `START`, `write` nicht** |
| auf eine Senke schreiben | `EIO` |

`freshness` ist `(0xFFFF - tack) << tmo_shift`, also das Alter der Daten.
Der Treiber führt die Doppelpufferung selbst: Gelesen wird die sichtbare
Seite, geschrieben die andere, danach schaltet er `VP` um. Darum darf man
die Docks nicht parallel über `mmap` anfassen.

## Message-Daten

### Senden

```c
mvb_port m = { .type = 3, .port = 6 };   /* MD_LOW an Gerät 6 */
memcpy(&m.data[2], nutzdaten, 28);       /* ab Byte 4! */
write(fd, &m, sizeof(m));                /* liefert 32 */
```

Der Treiber überschreibt `data[0]` und `data[1]` mit dem Link_Header nach
IEC 61375-1, Figure 120 — Zieladresse, Quelladresse und `PT = '1000'B`
(TCN Real-Time-Protocols). **Nutzdaten beginnen bei Byte 4, es sind 28 je
Frame.** Empfangsseitig liefert der Treiber den Link_Header mit, dort
beginnen die Nutzdaten also ebenfalls bei Byte 4 des Frames.

`type` wählt die Sendequeue: `MD_HIGH` die kurze mit hoher Priorität,
`MD_LOW` die lange. Ist die Queue voll, kommt `ENOBUFS`.

Die Felder dahinter — `SZ`, `final`, `origin`, `MTC`, `conn_ref` und die
Transport Data — reicht der Treiber unverändert durch. **Das ist die
Arbeit deiner API**, nach IEC 61375-1 Abschnitt 6.3 und Figure 125/128.

### Empfangen

```c
mvb_port m = { .type = 3, .port = 0 };
ssize_t n = read(fd, &m, 32);    /* liefert 32; m.data enthält den Frame */
```

Für Message-Daten wertet der Treiber `size` **nicht** aus — nur `type`
entscheidet. Er beschreibt genau `data[]`, also 32 Byte ab Offset 4;
`type`, `port` und `freshness` in deinem Puffer bleiben unberührt.

Ein `read` liefert **genau einen** Frame aus dem Softwarering (222 Plätze)
und `ENOBUFS`, wenn nichts da ist. Der Treiber verwirft bei Überlauf
stillschweigend; nur Bit 4 von `MD_GET_STATUS` verrät es.

Mit `irq > 0` füllt der Interrupt den Ring, und `poll()` meldet je
Eingang einmal `EPOLLIN`:

```c
struct pollfd pfd = { .fd = fd, .events = POLLIN };
if (poll(&pfd, 1, 100) > 0)
    while (read(fd, &m, 32) == 32) verarbeiten(&m);
```

`poll()` meldet **nicht**, solange der Ring Frames enthält, sondern einmal
je Interruptmeldung. Lies deshalb nach jedem `poll()` leer, bis `ENOBUFS`
kommt. `POLLOUT` meldet der Treiber nie.

Mit `irq = 0` meldet `poll()` nichts, dafür ruft `read()` den Verteiler
selbst auf — dann ist reines Abfragen der richtige Weg.

### Zustand abfragen

`MD_GET_STATUS` liefert drei Bits:

| Bit | Bedeutung |
|---|---|
| 1 | Nachricht empfangen (`tack` des physischen Ports MSNK ist `0xFFFF`) |
| 2 | Nachricht gesendet (dito MSRC) |
| 4 | Empfangsring war übergelaufen |

`MD_FLUSH_QUEUE` verwirft die Sendequeue.

## Fallstricke

Alles Folgende ist Verhalten des Originals. Wer es „repariert", bricht die
Kompatibilität zu den Hersteller-Bibliotheken.

**Drei ioctls nehmen den Wert, nicht den Zeiger.** `WRITE_DEV_ADDR`,
`WRITE_DSW` und `DISABLE_PORT` lesen `arg` direkt, obwohl der Kopf
`_IOW(..., uint16_t)` sagt:

```c
ioctl(fd, IOCTL_PIXY_MVBLLI_WRITE_DEV_ADDR, 240);          /* richtig */
ioctl(fd, IOCTL_PIXY_MVBLLI_WRITE_DSW, 0xFFFF0000u | dsw); /* Maske|Wert */
```

Folge: Die Geräteadresse 0 ist nicht setzbar, `arg == 0` fällt vorher in
`EINVAL`.

**`MD_GET_STATUS` benutzt die Zeigeradresse als Argument.** Das Original
liest vier Byte aus dem Zeiger und prüft, dass in beiden Hälften nur die
Bits 0…2 stehen — reicht dann aber nicht dieses Wort weiter, sondern die
**Adresse**: Bits 16…31 des Zeigers sind `selector`, Bits 0…15 `reset`.
Zurück kommen zwei Byte, nicht vier. Das ist ein Fehler, aber die
Anwendung läuft seit Jahren damit, und welche Bits sie sieht, hängt davon
ab. Der Nachbau macht es genauso.

**Message-Ziele nur unter 256.** `read` und `write` lehnen `port >= 0x100`
mit `EINVAL` ab, obwohl `DD` zwölf Bit breit ist. Broadcast `0xFFF` ist
über die Zeichengeräte-ABI damit nicht erreichbar.

**Beim Senden werden immer 32 Byte kopiert**, unabhängig von `SZ`. Kurze
Pakete tragen Reste der vorigen Belegung deines Puffers auf den Bus. Wer
den Rest nullt, erzeugt anderen Busverkehr als das Original — für einen
bytegenauen Vergleich mit einem Bustrace ist das wichtig.

**Längere Nachrichten gibt es nicht.** Der Treiber sendet nur Einzelframes.
Alles über 28 Byte Nutzlast muss die API fragmentieren und
wieder zusammensetzen.

**`close()` nimmt die Station vom Bus.** Kein Prozessende ohne Absicht.

## Startreihenfolge

So kommt eine Station auf den Bus. Die Reihenfolge ist die der
Hersteller-Bibliothek, am Gerät nachgemessen:

```c
fd = open("/dev/mvblli0", O_RDWR);          /* initialisiert den Controller */

ioctl(fd, IOCTL_PIXY_MVBLLI_HWINIT, &ts);          /* ts_type 1, ownership 1 */
ioctl(fd, IOCTL_PIXY_MVBLLI_PD_CONF, &portliste);  /* Docks und STSR         */
ioctl(fd, IOCTL_PIXY_MVBLLI_MD_CONF, &mex);        /* Message-Daten frei     */
ioctl(fd, IOCTL_PIXY_MVBLLI_WRITE_DEV_ADDR, 240);  /* Wert, nicht Zeiger     */
ioctl(fd, IOCTL_PIXY_MVBLLI_WRITE_CONTROL, &ctrl); /* Leitung, Antwortfenster*/
ioctl(fd, IOCTL_PIXY_MVBLLI_START);                /* MVB_GO                 */

/* Betrieb: read/write für PD, write/poll/read für MD, MD_GET_STATUS */

ioctl(fd, IOCTL_PIXY_MVBLLI_STOP);
close(fd);                                  /* baut den Controller ab */
```

Der Sollzustand nach `START`, am Gerät im Produktivbetrieb gemessen:

| Register | Wert |
|---|---|
| `SCR` | `87C7` — IM, TMO_43US, WS_3, ARB_3, RCEV, IL_RUNNING, **MAS = 0** |
| `MCR` | `2803` — `mcm = 3`, 256 KiB. Bits `0x2800` sind FPGA-Vorgabe, nur Read-Modify-Write |
| `DR` | `150D` — ESD, 1,5 Mbit |
| `STSR` | `10DB` — Raster 1 ms, aus `PD_CONF` berechnet |
| `IMR0` / `IMR1` | `0003` / `0880` |
| `DAOR` / `DAOK` | `00F0` / Override aktiv — Geräteadresse 240 |
| `BCR` (ISA) | `2677` — Interruptnummer 7 im unteren Byte |

Weicht etwas davon ab, stimmt die Reihenfolge nicht. Dieses Gerät ist
**kein Busadministrator** (`MAS = 0`); ohne einen anderen Teilnehmer, der
die Abfrageliste abarbeitet, bewegt sich auf dem Bus nichts.

## Zum Weiterlesen

| Datei | Inhalt |
|---|---|
| `README.md` | Bauen, Gegenlesen gegen das Dekompilat, bewusste Eigenheiten |
| `PLAN.md` | Hardwarestand, Traffic-Memory-Karte, Registerwerte, Messprotokolle |
| `tools/mvbdiff.py` | Original gegen Nachbau am Gerät vergleichen |
| `tools/irqstate.py` | Interruptkette aufnehmen, ohne sie zu zerstören |
| `../../tests/tm_replay.c` | Treibercode gegen einen echten Speicherabzug abspielen |
