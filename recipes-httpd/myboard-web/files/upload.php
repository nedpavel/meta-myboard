<?php
/*
 * upload.php — nimmt den Datenstrom des sendenden Geraets per HTTP entgegen.
 *
 * Aufruf vom anderen Geraet, z.B.:
 *     curl -X POST --data-binary @- http://<board>/upload.php
 *     (oder dauerhaft offen gehalten, Zeile fuer Zeile)
 *
 * nginx reicht den Request dank "fastcgi_request_buffering off" laufend
 * durch, dieses Skript liest ihn also Stueck fuer Stueck — es wartet
 * nicht, bis der Sender fertig ist. Das ist der Unterschied zwischen
 * Datei-Upload und Datenstrom.
 *
 * Die Daten landen in zwei Dateien:
 *     data/latest.txt  — zuletzt empfangene Zeile (fuer die Anzeige)
 *     data/stream.log  — Verlauf, auf MAX_LINES begrenzt
 */

const DATA_DIR  = '/var/www/localhost/data';
const MAX_LINES = 500;          /* Verlauf begrenzen, der Speicher ist klein */

if ($_SERVER['REQUEST_METHOD'] !== 'POST') {
    http_response_code(405);
    header('Allow: POST');
    echo "Nur POST wird angenommen.\n";
    exit;
}

if (!is_dir(DATA_DIR) && !@mkdir(DATA_DIR, 0775, true)) {
    http_response_code(500);
    echo "Datenverzeichnis nicht beschreibbar: " . DATA_DIR . "\n";
    exit;
}

$logPfad    = DATA_DIR . '/stream.log';
$letztePfad = DATA_DIR . '/latest.txt';

$ein = fopen('php://input', 'rb');
if ($ein === false) {
    http_response_code(500);
    echo "Eingabestrom nicht lesbar.\n";
    exit;
}

/* Zeilenweise lesen, damit auch eine dauerhaft offene Verbindung
   fortlaufend verarbeitet wird statt erst am Ende. */
$zeilen = 0;
$rest   = '';
while (!feof($ein)) {
    $brocken = fread($ein, 8192);
    if ($brocken === false || $brocken === '') {
        usleep(50000);          /* kurz warten statt heiss zu drehen */
        continue;
    }
    $rest .= $brocken;

    while (($pos = strpos($rest, "\n")) !== false) {
        $zeile = rtrim(substr($rest, 0, $pos), "\r");
        $rest  = substr($rest, $pos + 1);
        if ($zeile === '') {
            continue;
        }
        $stempel = date('Y-m-d H:i:s');
        file_put_contents($letztePfad, $stempel . "\t" . $zeile . "\n", LOCK_EX);
        file_put_contents($logPfad, $stempel . "\t" . $zeile . "\n",
                          FILE_APPEND | LOCK_EX);
        $zeilen++;
    }
}
fclose($ein);

/* Verlauf kuerzen, damit die Datei nicht unbegrenzt waechst. */
if (is_file($logPfad)) {
    $alle = file($logPfad, FILE_IGNORE_NEW_LINES | FILE_SKIP_EMPTY_LINES);
    if ($alle !== false && count($alle) > MAX_LINES) {
        file_put_contents($logPfad,
            implode("\n", array_slice($alle, -MAX_LINES)) . "\n", LOCK_EX);
    }
}

header('Content-Type: text/plain; charset=utf-8');
echo "OK, " . $zeilen . " Zeile(n) uebernommen.\n";
