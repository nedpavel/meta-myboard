-- Datenbank, Tabelle und Benutzer fuer das Pixy 1000 (MariaDB).
-- Laeuft bei jedem Start ueber myboard-db-setup.service; alles mit
-- IF NOT EXISTS, damit bestehende Daten und geaenderte Passwoerter
-- erhalten bleiben.

-- mariadb-install-db --rpm (install_db.service) legt anonyme Benutzer
-- ''@'localhost' und ''@'<hostname>' an. ''@'localhost' ist spezifischer
-- als 'admin'@'%' und wuerde die Anmeldung von admin am Geraet (phpMyAdmin)
-- abweisen. Weg damit und mit der offenen Datenbank test - wie
-- mariadb-secure-installation.
DELETE FROM mysql.global_priv WHERE User = '';
DELETE FROM mysql.db WHERE Db = 'test' OR Db = 'test\\_%';
FLUSH PRIVILEGES;
DROP DATABASE IF EXISTS test;

CREATE DATABASE IF NOT EXISTS myboard
    CHARACTER SET utf8mb4 COLLATE utf8mb4_unicode_ci;

-- Messwerte von streamd (TCP-Port 9100), Verlauf auf 100000 Zeilen begrenzt
CREATE TABLE IF NOT EXISTS myboard.werte (
    id    BIGINT UNSIGNED NOT NULL AUTO_INCREMENT PRIMARY KEY,
    zeit  DATETIME(3)     NOT NULL,
    wert  VARCHAR(1024)   NOT NULL,
    KEY werte_zeit (zeit)
) ENGINE=InnoDB;

-- streamd und PHP melden sich ohne Passwort ueber den Unix-Socket an: der
-- Linux-Benutzer muss so heissen wie der Datenbank-Benutzer (unix_socket).
-- streamd laeuft als Benutzer streamd, php-fpm als nobody.
CREATE USER IF NOT EXISTS 'streamd'@'localhost' IDENTIFIED VIA unix_socket;
GRANT SELECT, INSERT, DELETE ON myboard.werte TO 'streamd'@'localhost';

CREATE USER IF NOT EXISTS 'nobody'@'localhost' IDENTIFIED VIA unix_socket;
GRANT SELECT ON myboard.* TO 'nobody'@'localhost';

-- Verwaltung ueber das Netz (MySQL Workbench) und phpMyAdmin.
-- Passwort nach der Inbetriebnahme aendern:
--   mariadb -e "ALTER USER 'admin'@'%' IDENTIFIED BY 'neues-passwort';"
CREATE USER IF NOT EXISTS 'admin'@'%' IDENTIFIED BY 'myboard';
GRANT ALL PRIVILEGES ON *.* TO 'admin'@'%' WITH GRANT OPTION;
