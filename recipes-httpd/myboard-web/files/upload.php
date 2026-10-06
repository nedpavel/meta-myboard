<?php
/*
 * upload.php — nimmt den Datenstrom des sendenden Geraets per HTTP entgegen.
 *
 * EMPFOHLENE VERWENDUNG — ein POST je Wert, periodisch wiederholt:
 *
 *     while true; do
 *         curl -s -X POST --data-binary "Wert $(date +%S)" \
 *              http://<board>/upload.php
 *         sleep 1
 *     done
 *
 * Eine DAUERHAFT OFFENE Verbindung funktioniert hier NICHT, auch wenn
 * nginx mit "fastcgi_request_buffering off" nicht puffert (per nginx -T
 * geprueft). Der Puffer sitzt in PHP-FPM: FPM uebergibt einen Request
 * erst dann an das Skript, wenn er vollstaendig vorliegt. Fuer stehende
 * Stroeme ist PHP-FPM bauartbedingt nicht gemacht; braeuchte man das,
 * waere ein eigener Dienst auf einem eigenen Port der richtige Weg.
 *
 * Die periodische Variante ist ohnehin robuster: Sie belegt keinen
 * FPM-Arbeitsprozess dauerhaft, und ein Verbindungsabbruch kostet
 * hoechstens einen Wert statt den ganzen Strom.
 *
 * Das Skript liest den Request dennoch stueckweise — so kostet ein
 * groesserer Block keinen zusaetzlichen Speicher.
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

/* Eine empfangene Zeile ablegen. */
function zeileSchreiben($zeile, $letztePfad, $logPfad)
{
    $zeile = trim($zeile);
    if ($zeile === '') {
        return 0;
    }
    $satz = date('Y-m-d H:i:s') . "\t" . $zeile . "\n";
    file_put_contents($letztePfad, $satz, LOCK_EX);
    file_put_contents($logPfad, $satz, FILE_APPEND | LOCK_EX);
    return 1;
}

/* Zeilenweise lesen, damit auch eine dauerhaft offene Verbindung
   fortlaufend verarbeitet wird statt erst am Ende. */
$zeilen = 0;
$rest   = '';
while (!feof($ein)) {
    $brocken = fread($ein, 8192);

    /* fread liefert '' sowohl bei "noch nichts da" als auch am Ende des
       Stroms. Ohne die feof-Pruefung hier wuerde die Schleife endlos
       weiterdrehen, wenn der Sender die Verbindung schliesst. */
    if ($brocken === false || $brocken === '') {
        if (feof($ein)) {
            break;
        }
        usleep(50000);          /* kurz warten statt heiss zu drehen */
        continue;
    }
    $rest .= $brocken;

    while (($pos = strpos($rest, "\n")) !== false) {
        $zeilen += zeileSchreiben(substr($rest, 0, $pos), $letztePfad, $logPfad);
        $rest = substr($rest, $pos + 1);
    }
}
fclose($ein);

/* Rest ohne abschliessenden Zeilenumbruch nicht verwerfen. Genau das
   passierte bei einem einzelnen Wert ohne \n — die Antwort lautete dann
   "OK, 0 Zeile(n) uebernommen", obwohl Daten angekommen waren. */
$zeilen += zeileSchreiben($rest, $letztePfad, $logPfad);

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
