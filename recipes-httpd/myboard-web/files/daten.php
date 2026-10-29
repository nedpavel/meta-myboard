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

/* Verlauf in MariaDB (schreibt streamd). php-fpm laeuft als nobody und
   meldet sich ueber den Unix-Socket ohne Passwort an (unix_socket). */
const DB_SOCKET = '/var/lib/mysql/mysql.sock';

$streamPfad = '/run/myboard-stream/latest.txt';
$postPfad   = DATA_DIR . '/latest.txt';
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
try {
    $db = new mysqli('localhost', 'nobody', '', 'icn', 0, DB_SOCKET);
    /* Zeit mit Millisekunden wie in latest.txt (%f liefert 6 Stellen) */
    $res = $db->query("SELECT LEFT(DATE_FORMAT(zeit, '%Y-%m-%d %H:%i:%s.%f'), 23)
                              AS zeit, wert
                       FROM werte ORDER BY id DESC LIMIT " . ZEILEN);
    while ($r = $res->fetch_assoc()) {
        $verlauf[] = $r['zeit'] . "\t" . $r['wert'];
    }
    $db->close();
} catch (Throwable $e) {
    /* Datenbank gerade nicht erreichbar — dann eben ohne Verlauf */
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
