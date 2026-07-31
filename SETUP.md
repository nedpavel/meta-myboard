# Build-Setup & Layer-Manifest

Dieses Repo versioniert **nur** den `meta-myboard`-Layer. Die Upstream-Layer sind
große Git-Klone und werden **nicht** mitversioniert, sondern hier auf feste
Revisionen gepinnt. Damit lässt sich die komplette Build-Umgebung reproduzieren,
ohne mehrere GB Fremdcode zu speichern.

Yocto-Release-Basis: **scarthgap / wrynose** (OE-Core), Qt6 aus **meta-qt6 6.11**.

## 1. Verzeichnislayout

Alle Layer liegen nebeneinander unter einer gemeinsamen Wurzel (hier `~/Embedded/`):

```
~/Embedded/
├── bitbake/              # Upstream (Klon)
├── openembedded-core/    # Upstream (Klon)  -> liefert oe-init-build-env
├── meta-yocto/           # Upstream (Klon)
├── meta-openembedded/    # Upstream (Klon)
├── meta-intel/           # Upstream (Klon)
├── meta-qt6/             # Upstream (Klon)
├── meta-myboard/         # <-- DIESES Repo
└── build/                # generiert, NICHT versioniert (tmp/, sstate-cache/, downloads/)
```

## 2. Upstream-Layer klonen und auf die gepinnte Revision setzen

| Layer               | Branch        | Commit (gepinnt)                           |
|---------------------|---------------|--------------------------------------------|
| openembedded-core   | wrynose       | `cd635307ab8460d05d13dbfb3f28efdbde6609cd` |
| bitbake             | 2.18          | `acfe02fa38b5da9e6a36c6cedcf91d4fcbefbfbd` |
| meta-yocto          | wrynose       | `8251bdad5fda780a000fb41e6eda82eadf0fa39e` |
| meta-openembedded   | wrynose       | `a43f0d532c399458cae44ce66f3799a220fbb497` |
| meta-intel          | master        | `efa8a9b22ffaeecfdd113673074496d377ca290c` |
| meta-qt6            | 6.11          | `ef0611a598fd7ba909804be0c6314ed7c85725b9` |
| poky (optional*)    | my-scarthgap  | `da5493bf86b3e75bbae4c5789fdfaca67b6f6a65` |
| psplash (optional*) | master        | `53ae74a36bf17675228552abb927d2f981940a6a` |

\* `poky` und `psplash` sind auf diesem Host geklont, stehen aber **nicht** in der
`bblayers.conf` (die aktiven Basislayer sind `openembedded-core/meta` +
`meta-yocto/*`). Nur der Vollständigkeit halber aufgeführt.

```bash
cd ~/Embedded

git clone https://git.openembedded.org/openembedded-core
git -C openembedded-core checkout cd635307ab8460d05d13dbfb3f28efdbde6609cd

git clone https://git.openembedded.org/bitbake
git -C bitbake checkout acfe02fa38b5da9e6a36c6cedcf91d4fcbefbfbd

git clone https://git.yoctoproject.org/meta-yocto
git -C meta-yocto checkout 8251bdad5fda780a000fb41e6eda82eadf0fa39e

git clone https://git.openembedded.org/meta-openembedded
git -C meta-openembedded checkout a43f0d532c399458cae44ce66f3799a220fbb497

git clone https://git.yoctoproject.org/git/meta-intel
git -C meta-intel checkout efa8a9b22ffaeecfdd113673074496d377ca290c

git clone https://code.qt.io/yocto/meta-qt6.git
git -C meta-qt6 checkout ef0611a598fd7ba909804be0c6314ed7c85725b9

# meta-myboard = dieses Repo
git clone <URL-dieses-Repos> meta-myboard
```

## 3. Build-Konfiguration

Referenz-Snapshots liegen unter [`build-conf/`](build-conf/):

- `build-conf/local.conf`
- `build-conf/bblayers.conf`

Nach dem ersten `oe-init-build-env` (das eine Default-`build/conf/` anlegt) die
Snapshots übernehmen:

```bash
cd ~/Embedded
source openembedded-core/oe-init-build-env build
cp ../meta-myboard/build-conf/local.conf   conf/local.conf
cp ../meta-myboard/build-conf/bblayers.conf conf/bblayers.conf
```

> **Hinweis:** `bblayers.conf` enthält **absolute Pfade** (`/home/manolo/Embedded/...`).
> Liegt die Wurzel woanders, diese Pfade anpassen.

## 4. Bauen

```bash
cd ~/Embedded
source openembedded-core/oe-init-build-env build
bitbake myboard-image      # volles Image (-> .wic / .img.bz2 unter tmp/deploy/images)
bitbake myboard-gui        # nur die GUI-Binary (für schnellen NFS-Deploy)
```

## 5. Entwickler-Workflows (Kurzreferenz)

**GUI per NFS testen** (ohne Reflash) — Host exportiert `/srv/nfs/myboard-dev`,
Board mountet `/mnt/dev`:

```bash
# Host:
cp build/tmp/work/corei7-64-poky-linux/myboard-gui/1.0/build/myboard-gui /srv/nfs/myboard-dev/
# Board:
systemctl stop myboard-gui && cp /mnt/dev/myboard-gui /usr/bin/ && systemctl start myboard-gui
```

**App-Update per USB-Stick** (Feature in der GUI): ein `*.tar.gz` mit der Datei
`myboard-gui` in die **Wurzel** eines USB-Sticks legen → am Board Taste **5**
(Update App) → **Ja (1)**. Der Updater ersetzt `/usr/bin/myboard-gui` atomar
(`mv`, kein in-place `cp` wegen ETXTBSY der laufenden Binary) und startet den
Dienst neu.
