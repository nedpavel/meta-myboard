<?php
/*
 * daten.php — liefert nur die Messwerte als JSON, ohne HTML-Geruest.
 *
 * index.php holt sich damit im Sekundentakt den aktuellen Stand, statt
 * die ganze Seite neu zu laden. Das vermeidet das Flackern und haelt die
 * Last klein: statt Seitenaufbau samt Verlaufsabfrage werden nur ein paar
 * hundert Byte uebertragen.
 */
const DATA_DIR = '/var/www/localhost/data';
const ZEILEN   = 40;

header('Content-Type: application/json; charset=utf-8');
/* Nicht zwischenspeichern — sonst zeigt der Browser alte Werte. */
header('Cache-Control: no-store');

$streamPfad = '/run/myboard-stream/latest.txt';
$postPfad   = DATA_DIR . '/latest.txt';
$dbPfad     = DATA_DIR . '/werte.db';
$logPfad    = DATA_DIR . '/stream.log';

/* Aktueller Wert — die jeweils neuere der beiden Quellen gilt. */
$letztePfad = $postPfad;
if (is_file($streamPfad)) {
    if (!is_file($postPfad) || filemtime($streamPfad) >= filemtime($postPfad)) {
        $letztePfad = $streamPfad;
    }
}
$letzte   = is_file($letztePfad) ? trim(file_get_contents($letztePfad)) : '';
$alterSek = is_file($letztePfad) ? max(0, time() - filemtime($letztePfad)) : -1;

/* Verlauf: bevorzugt aus der Datenbank (streamd), sonst aus stream.log */
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

echo json_encode([
    'wert'    => $letzte,
    'alter'   => $alterSek,
    'verlauf' => $verlauf,
], JSON_UNESCAPED_UNICODE);
