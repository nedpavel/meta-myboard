<?php
/*
 * phpMyAdmin fuer das Pixy 1000 (phpmyadmin_%.bbappend).
 *
 * Anmeldung mit einem MariaDB-Benutzer, z.B. admin (Passwort siehe
 * myboard-lamp/files/db-setup.sql - nach der Inbetriebnahme aendern).
 */

/* Schluessel fuer das Anmelde-Cookie: 32 Zeichen, je Geraet verschieden
   (aus /etc/machine-id), damit kein Geheimnis im Repository steht */
$cfg['blowfish_secret'] = substr(hash('sha256', 'phpmyadmin-' .
    trim((string) @file_get_contents('/etc/machine-id'))), 0, 32);

$i = 0;
$i++;
$cfg['Servers'][$i]['auth_type'] = 'cookie';
$cfg['Servers'][$i]['host'] = 'localhost';
/* MariaDB-Socket wie in /etc/my.cnf */
$cfg['Servers'][$i]['socket'] = '/var/lib/mysql/mysql.sock';
$cfg['Servers'][$i]['AllowNoPassword'] = false;

/* Temp-Verzeichnis (angelegt von myboard-lamp, tmpfiles) */
$cfg['TempDir'] = '/var/lib/phpmyadmin/tmp';
