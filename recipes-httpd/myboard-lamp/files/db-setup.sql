-- Eigene Tabellen, Benutzer und root-Passwort in MariaDB (Pixy 1000).
-- Laeuft bei jedem Start ueber myboard-db-setup.service, direkt nach
-- icn_setup.sql des Applikationsingenieurs (Datenbank icn, Benutzer icn,
-- Tabellen icn_*). Unsere Tabellen liegen in derselben Datenbank icn; alles
-- mit IF NOT EXISTS, damit bestehende Daten erhalten bleiben.

-- mariadb-install-db --rpm (install_db.service) legt anonyme Benutzer
-- ''@'localhost' und ''@'<hostname>' an. ''@'localhost' ist spezifischer
-- als 'admin'@'%' und wuerde die Anmeldung von admin am Geraet (phpMyAdmin)
-- abweisen. Weg damit und mit der offenen Datenbank test - wie
-- mariadb-secure-installation.
DELETE FROM mysql.global_priv WHERE User = '';
DELETE FROM mysql.db WHERE Db = 'test' OR Db = 'test\\_%';
FLUSH PRIVILEGES;
DROP DATABASE IF EXISTS test;

-- legt icn_setup.sql an; hier nur fuer den Fall, dass es im Image fehlt
CREATE DATABASE IF NOT EXISTS icn
    CHARACTER SET utf8mb4 COLLATE utf8mb4_unicode_ci;

-- Bis 2026-10 lagen unsere Tabellen in der Datenbank myboard: mit Inhalt
-- nach icn umziehen (nur beim ersten Start mit diesem Stand), dann myboard
-- samt ihrer Rechte loeschen. mvb_prozessdaten (MEMORY) legt idu2-mvb
-- ohnehin neu an.
RENAME TABLE IF EXISTS myboard.werte TO icn.werte;
RENAME TABLE IF EXISTS myboard.dds_meldungen TO icn.dds_meldungen;
RENAME TABLE IF EXISTS myboard.dds_stand TO icn.dds_stand;
RENAME TABLE IF EXISTS myboard.tasten TO icn.tasten;
DROP DATABASE IF EXISTS myboard;
DELETE FROM mysql.db WHERE Db = 'myboard';
DELETE FROM mysql.tables_priv WHERE Db = 'myboard';
FLUSH PRIVILEGES;

-- Messwerte von streamd (TCP-Port 9100), Verlauf auf 100000 Zeilen begrenzt
CREATE TABLE IF NOT EXISTS icn.werte (
    id    BIGINT UNSIGNED NOT NULL AUTO_INCREMENT PRIMARY KEY,
    zeit  DATETIME(3)     NOT NULL,
    wert  VARCHAR(1024)   NOT NULL,
    KEY werte_zeit (zeit)
) ENGINE=InnoDB;

-- DDS-Meldungen von DIA1, geschrieben von idu2-mvb (als root ueber den
-- Unix-Socket). Gilt fuer eine Version des DDS-Caches bei DIA1: aendert
-- sich der Cache_index, ersetzt idu2-mvb den ganzen Inhalt; beim Blaettern
-- (gleicher Cache_index) kommen die Seiten dazu. Spalten wie auf der Seite
-- DDS-Speicher; Zeiten so, wie die Oberflaeche sie zeigt, NULL = keine.
CREATE TABLE IF NOT EXISTS icn.dds_meldungen (
    ereignisnr   SMALLINT UNSIGNED NOT NULL PRIMARY KEY,
    pos          SMALLINT UNSIGNED NULL,     -- Platz in der Liste, ab 1
    seite        SMALLINT UNSIGNED NULL,     -- NULL: DIA1 schickte von sich aus
    uic          CHAR(4)           NOT NULL, -- UIC/Sub
    prio         VARCHAR(2)        NOT NULL,
    zaehler      SMALLINT UNSIGNED NOT NULL,
    kommt        DATETIME          NULL,
    geht         DATETIME          NULL,
    status       VARCHAR(10)       NOT NULL,
    knoten       CHAR(2)           NOT NULL,
    knoten2      CHAR(2)           NOT NULL,
    meldung      VARCHAR(1000)     NOT NULL,
    meldnr       SMALLINT UNSIGNED NOT NULL,
    stoer        SMALLINT UNSIGNED NOT NULL,
    ref7         SMALLINT UNSIGNED NOT NULL,
    ref8         SMALLINT UNSIGNED NOT NULL,
    nr           TINYINT UNSIGNED  NOT NULL, -- Index im Satz + 1
    roh          VARCHAR(100)      NOT NULL, -- 32 Byte ab 00 80 00
    cache_index  SMALLINT UNSIGNED NOT NULL,
    abgerufen    DATETIME          NOT NULL,
    KEY dds_pos (pos)
) ENGINE=InnoDB;

-- Stand der Tabelle oben, eine Zeile (id = 1)
CREATE TABLE IF NOT EXISTS icn.dds_stand (
    id            TINYINT UNSIGNED  NOT NULL PRIMARY KEY,
    cache_index   SMALLINT UNSIGNED NOT NULL,
    geaendert     DATETIME          NOT NULL, -- seit wann dieser Cache_index
    aktualisiert  DATETIME          NOT NULL, -- letzte Seite geschrieben
    vollstaendig  BOOLEAN           NOT NULL  -- bis zur letzten Seite geholt
) ENGINE=InnoDB;

-- Prozessdaten am MVB, geschrieben von idu2-mvb: eine Zeile je Signal der
-- Rolle des Pixy (IDU1 oder IDU2) laut Busbeschreibung. richtung 'lesen' =
-- Sink-Port, das Display liest; 'schreiben' = Source-Port, das Display
-- sendet. idu2-mvb legt den Inhalt beim Start neu an und schreibt jede
-- Lesung eines Ports (gelesen, frisch, Wert; geaendert nur bei neuem Wert).
-- Wo der Wert steht, haengt vom datentyp ab:
--   bool uint8 uint16 uint32 int16 float   wert
--   datetime                               wert (Unix-Zeit, exakt) und
--                                          wert_zeit (UTC)
--   string                                 wert_text
-- Texte (AR_CHAR8_n, ARn) stehen zusaetzlich je Byte als eigene Zeile
-- <Name>_ArrByte<i> (uint8) - so wie in icn_datafields; ueber den Namen
-- lassen sich damit alle Signale mit icn_datafields verknuepfen.
-- MEMORY statt InnoDB: rund 300 Lesungen pro Sekunde gingen sonst als
-- Redo-Log auf die CFast (Dutzende GB am Tag). Der Inhalt ist nach einem
-- Neustart von MariaDB weg, bis idu2-mvb ihn neu anlegt - das tut es beim
-- Start ohnehin. MEMORY reserviert jede Zeile in voller Laenge, deshalb
-- die Textspalten knapp (laengste Beschreibung 237 Zeichen).
CREATE TABLE IF NOT EXISTS icn.mvb_prozessdaten (
    port          SMALLINT UNSIGNED NOT NULL,
    name          VARCHAR(40)       NOT NULL,
    richtung      ENUM('lesen', 'schreiben') NOT NULL DEFAULT 'lesen',
    telegramm     VARCHAR(30)       NOT NULL DEFAULT '',
    quelle        VARCHAR(20)       NOT NULL DEFAULT '',  -- sendendes Geraet
    zyklus_ms     SMALLINT UNSIGNED NOT NULL DEFAULT 0,
    port_bytes    TINYINT UNSIGNED  NOT NULL DEFAULT 0,
    typ           VARCHAR(16)       NOT NULL DEFAULT '',  -- wie in der Busbeschreibung
    datentyp      VARCHAR(10)       NOT NULL DEFAULT '',
    byte_ofs      TINYINT UNSIGNED  NOT NULL DEFAULT 0,
    bit_ofs       TINYINT UNSIGNED  NOT NULL DEFAULT 0,
    faktor        DOUBLE            NOT NULL DEFAULT 1,
    einheit       VARCHAR(8)        NOT NULL DEFAULT '',
    beschreibung  VARCHAR(500)      NOT NULL DEFAULT '',
    senken        VARCHAR(100)      NOT NULL DEFAULT '',  -- alle Empfaenger
    wert          DOUBLE            NULL,
    wert_zeit     DATETIME(6)       NULL,
    wert_text     VARCHAR(100)      NULL,
    roh           VARCHAR(100)      NULL,     -- Bytes des Signals, hex
    frisch        BOOLEAN           NULL,     -- Port aktuell; NULL = nie gelesen
    gelesen       DATETIME(3)       NULL,     -- letzte Lesung des Ports
    geaendert     DATETIME(3)       NULL,     -- letzte Aenderung des Werts
    PRIMARY KEY (port, name),
    KEY mvb_name (name)
) ENGINE=MEMORY;

-- Hardkeys der Front, geschrieben von myboard-tasten: eine Zeile je Druck,
-- sofort beim Druecken; losgelassen und dauer_ms kommen beim Loslassen
-- dazu (NULL = noch gedrueckt). taste und position nach der
-- Standardbelegung (INC-100 FW User Manual 5.11, Figure 8); zeichen, hid
-- und keycode zeigen, was die Taste wirklich gesendet hat. Hoechstens
-- 100000 Zeilen, die aeltesten fallen weg.
CREATE TABLE IF NOT EXISTS icn.tasten (
    id           BIGINT UNSIGNED   NOT NULL AUTO_INCREMENT PRIMARY KEY,
    gedrueckt    DATETIME(3)       NOT NULL,  -- Zeit des Kernels
    losgelassen  DATETIME(3)       NULL,
    dauer_ms     INT UNSIGNED      NULL,
    taste        VARCHAR(20)       NOT NULL,  -- Beschriftung, z. B. 'F1', 'Pfeil hoch'
    position     VARCHAR(8)        NOT NULL,  -- P41,n; leer: andere Tastatur
    zeichen      VARCHAR(8)        NOT NULL,  -- gesendet, z. B. 'F1', 'Up', 'm'
    hid          TINYINT UNSIGNED  NULL,      -- HID Usage ID
    keycode      SMALLINT UNSIGNED NOT NULL,  -- Linux-Keycode
    kernel_name  VARCHAR(20)       NOT NULL,  -- z. B. KEY_F1
    geraet       VARCHAR(80)       NOT NULL,  -- Name des Eingabegeraets
    KEY tasten_gedrueckt (gedrueckt)
) ENGINE=InnoDB;

-- streamd und PHP melden sich ohne Passwort ueber den Unix-Socket an: der
-- Linux-Benutzer muss so heissen wie der Datenbank-Benutzer (unix_socket).
-- streamd laeuft als Benutzer streamd, php-fpm als nobody.
CREATE USER IF NOT EXISTS 'streamd'@'localhost' IDENTIFIED VIA unix_socket;
GRANT SELECT, INSERT, DELETE ON icn.werte TO 'streamd'@'localhost';

CREATE USER IF NOT EXISTS 'nobody'@'localhost' IDENTIFIED VIA unix_socket;
GRANT SELECT ON icn.* TO 'nobody'@'localhost';

-- myboard-tasten laeuft als Benutzer tasten
CREATE USER IF NOT EXISTS 'tasten'@'localhost' IDENTIFIED VIA unix_socket;
GRANT SELECT, INSERT, UPDATE, DELETE ON icn.tasten TO 'tasten'@'localhost';

-- root: Passwort SBB-ICN (phpMyAdmin, mariadb -u root -p). Am Geraet selbst
-- meldet sich root weiter ohne Passwort ueber den Unix-Socket an - so
-- arbeiten idu2-mvb und diese Einrichtung. Gesetzt bei jedem Start, wie das
-- Passwort von icn in icn_setup.sql.
ALTER USER 'root'@'localhost'
    IDENTIFIED VIA unix_socket OR mysql_native_password USING PASSWORD('SBB-ICN');

-- Verwaltung ueber das Netz (MySQL Workbench) und phpMyAdmin.
-- Passwort nach der Inbetriebnahme aendern:
--   mariadb -e "ALTER USER 'admin'@'%' IDENTIFIED BY 'neues-passwort';"
CREATE USER IF NOT EXISTS 'admin'@'%' IDENTIFIED BY 'myboard';
GRANT ALL PRIVILEGES ON *.* TO 'admin'@'%' WITH GRANT OPTION;
