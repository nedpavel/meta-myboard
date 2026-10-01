# Ersetzt die Standard-Site des nginx-Rezepts durch unsere Variante mit
# PHP-Anbindung (FastCGI an php-fpm) und den Einstellungen fuer den
# Datenstrom-Upload. Ueber den bbappend statt ueber ein eigenes Paket,
# damit es keinen Dateikonflikt um sites-available/default_server gibt.
FILESEXTRAPATHS:prepend := "${THISDIR}/files:"

# Ohne php-fpm liefert die PHP-Location nur 502 Bad Gateway.
RDEPENDS:${PN} += "php-fpm"
