<?php
/*
 * index.php — Anzeigeseite fuer die empfangenen Daten.
 *
 * Die Seite wird EINMAL geladen; danach holt sie sich im Sekundentakt nur
 * noch die Werte von daten.php (JSON) und tauscht die Texte aus. Frueher
 * lud sie sich per <meta refresh> komplett neu — das flackerte sichtbar
 * und baute jedes Mal den ganzen Verlauf neu auf.
 *
 * Das anfaengliche Fuellen passiert weiterhin serverseitig, damit sofort
 * etwas dasteht, auch bevor das erste Nachladen durch ist.
 */
const DATA_DIR = '/var/www/localhost/data';
const ZEILEN   = 40;          /* so viele Verlaufszeilen anzeigen */

$streamPfad = '/run/myboard-stream/latest.txt';
$postPfad   = DATA_DIR . '/latest.txt';
$logPfad    = DATA_DIR . '/stream.log';
$dbPfad     = DATA_DIR . '/werte.db';

/* Zwei moegliche Quellen fuer den aktuellen Wert:
   - streamd (TCP, Port 9100) schreibt ins tmpfs, weil es bis zu
     100 Werte/s sind und die CFast das nicht jedes Mal sehen soll
   - upload.php (HTTP-POST) schreibt direkt ins Datenverzeichnis
   Es gilt die jeweils neuere Datei. */
$letztePfad = $postPfad;
if (is_file($streamPfad)) {
    if (!is_file($postPfad) || filemtime($streamPfad) >= filemtime($postPfad)) {
        $letztePfad = $streamPfad;
    }
}

$letzte = is_file($letztePfad) ? trim(file_get_contents($letztePfad)) : '';

/* Verlauf aus zwei moeglichen Quellen:
   - werte.db  schreibt streamd (TCP) — gebuendelt, um die CFast zu schonen
   - stream.log schreibt upload.php (HTTP-POST)
   Die Datenbank hat Vorrang, sofern vorhanden. */
$verlauf = [];
if (is_file($dbPfad)) {
    try {
        $db = new SQLite3($dbPfad, SQLITE3_OPEN_READONLY);
        $db->busyTimeout(2000);     /* streamd schreibt parallel */
        $res = $db->query('SELECT zeit, wert FROM werte
                           ORDER BY id DESC LIMIT ' . ZEILEN);
        while ($res && ($r = $res->fetchArray(SQLITE3_ASSOC))) {
            $verlauf[] = $r['zeit'] . "\t" . $r['wert'];
        }
        $db->close();
    } catch (Exception $e) {
        /* Datenbank gerade nicht lesbar — dann eben ohne Verlauf */
    }
}
if (!$verlauf && is_file($logPfad)) {
    $alle = file($logPfad, FILE_IGNORE_NEW_LINES | FILE_SKIP_EMPTY_LINES);
    if ($alle !== false) {
        $verlauf = array_reverse(array_slice($alle, -ZEILEN));
    }
}
$alterSek = is_file($letztePfad) ? max(0, time() - filemtime($letztePfad)) : -1;
?>
<!DOCTYPE html>
<html lang="de">
<head>
<meta charset="utf-8">
<title>MyBoard — Datenstrom</title>
<style>
  body   { background:#EDEDE6; color:#2B2B2B;
           font-family: sans-serif; margin:0; padding:12px; }
  h1     { color:#23457E; font-size:22px; margin:0 0 10px 0; }
  .kopf  { border-bottom:1px solid #9AA0A6; padding-bottom:8px;
           margin-bottom:10px; }
  .wert  { font-size:28px; font-weight:bold; margin:6px 0; }
  .alt   { color:#C0392B; }
  .frisch{ color:#2E7D32; }
  .leer  { color:#888; font-style:italic; }
  table  { border-collapse:collapse; width:100%; font-size:14px; }
  th     { background:#E0E0E0; text-align:left; padding:4px; }
  td     { padding:3px 4px; border-bottom:1px solid #DDD;
           font-family: monospace; }
</style>
</head>
<body>
<div class="kopf">
  <h1>Datenstrom</h1>
  <div id="wert" class="wert"><?= htmlspecialchars($letzte) ?></div>
  <p id="status" class="<?= ($alterSek >= 0 && $alterSek <= 5) ? 'frisch' : 'alt' ?>">
     <?php if ($letzte === ''): ?>
       Noch keine Daten empfangen.
     <?php else: ?>
       zuletzt aktualisiert vor <?= $alterSek ?> s
     <?php endif; ?>
  </p>
</div>

<table>
  <tr><th>Verlauf (neueste zuerst)</th></tr>
  <tbody id="verlauf">
  <?php foreach ($verlauf as $z): ?>
    <tr><td><?= htmlspecialchars($z) ?></td></tr>
  <?php endforeach; ?>
  <?php if (!$verlauf): ?>
    <tr><td class="leer">—</td></tr>
  <?php endif; ?>
  </tbody>
</table>

<script>
/* Jede Sekunde nur die Werte nachladen und die Texte austauschen —
   die Seite selbst bleibt stehen, daher kein Flackern. */
var wertFeld    = document.getElementById('wert');
var statusFeld  = document.getElementById('status');
var verlaufFeld = document.getElementById('verlauf');

function aktualisieren() {
    fetch('daten.php', { cache: 'no-store' })
        .then(function (a) { return a.json(); })
        .then(function (d) {
            wertFeld.textContent = d.wert;

            if (d.wert === '') {
                statusFeld.textContent = 'Noch keine Daten empfangen.';
                statusFeld.className = 'leer';
            } else {
                statusFeld.textContent = 'zuletzt aktualisiert vor '
                                         + d.alter + ' s'
                                         + (d.alter > 5 ? ' — Datenstrom steht?' : '');
                statusFeld.className = (d.alter <= 5) ? 'frisch' : 'alt';
            }

            /* Verlauf nur neu aufbauen, wenn er sich geaendert hat —
               sonst waere die Tabelle jede Sekunde neu gezeichnet. */
            var neu = d.verlauf.join('\n');
            if (neu !== verlaufFeld.dataset.stand) {
                verlaufFeld.dataset.stand = neu;
                verlaufFeld.textContent = '';
                if (d.verlauf.length === 0) {
                    var leer = verlaufFeld.insertRow();
                    var lz = leer.insertCell();
                    lz.className = 'leer';
                    lz.textContent = '—';
                } else {
                    d.verlauf.forEach(function (z) {
                        verlaufFeld.insertRow().insertCell().textContent = z;
                    });
                }
            }
        })
        .catch(function () {
            statusFeld.textContent = 'Verbindung zum Board unterbrochen';
            statusFeld.className = 'alt';
        });
}

setInterval(aktualisieren, 1000);
aktualisieren();
</script>
</body>
</html>
