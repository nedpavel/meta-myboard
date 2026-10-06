<?php
/*
 * index.php — Anzeigeseite fuer die ueber upload.php empfangenen Daten.
 * Bewusst schlicht gehalten: kein JavaScript-Rahmenwerk, die Seite laedt
 * sich selbst neu. Laeuft damit auch im minimalen Browser (surf).
 */
const DATA_DIR = '/var/www/localhost/data';
const ZEILEN   = 40;          /* so viele Verlaufszeilen anzeigen */

/* Zwei moegliche Quellen fuer den aktuellen Wert:
   - streamd (TCP, Port 9000) schreibt ins tmpfs, weil es bis zu
     100 Werte/s sind und die CFast das nicht jedes Mal sehen soll
   - upload.php (HTTP-POST) schreibt direkt ins Datenverzeichnis
   Es gilt die jeweils neuere Datei. */
$streamPfad = '/run/myboard-stream/latest.txt';
$postPfad   = DATA_DIR . '/latest.txt';
$logPfad    = DATA_DIR . '/stream.log';
$dbPfad     = DATA_DIR . '/werte.db';

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
$alterSek = is_file($letztePfad) ? (time() - filemtime($letztePfad)) : -1;
?>
<!DOCTYPE html>
<html lang="de">
<head>
<meta charset="utf-8">
<meta http-equiv="refresh" content="2">
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
  <?php if ($letzte === ''): ?>
    <p class="leer">Noch keine Daten empfangen.
       Das sendende Geraet schickt per HTTP POST an
       <code>/upload.php</code>.</p>
  <?php else: ?>
    <div class="wert"><?= htmlspecialchars($letzte) ?></div>
    <p class="<?= ($alterSek >= 0 && $alterSek <= 5) ? 'frisch' : 'alt' ?>">
       zuletzt aktualisiert vor <?= max($alterSek, 0) ?> s
       <?= ($alterSek > 5) ? '— Datenstrom steht?' : '' ?></p>
  <?php endif; ?>
</div>

<table>
  <tr><th>Verlauf (neueste zuerst)</th></tr>
  <?php foreach ($verlauf as $z): ?>
    <tr><td><?= htmlspecialchars($z) ?></td></tr>
  <?php endforeach; ?>
  <?php if (!$verlauf): ?>
    <tr><td class="leer">—</td></tr>
  <?php endif; ?>
</table>
</body>
</html>
