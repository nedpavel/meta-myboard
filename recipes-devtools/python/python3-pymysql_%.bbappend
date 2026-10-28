# PyMySQL braucht cryptography nur fuer die RSA-Anmeldung
# (sha256_password, caching_sha2_password ohne TLS) und laedt es nur, wenn
# es da ist. idu2-mvb meldet sich ueber den Unix-Socket an (unix_socket);
# so bleibt das Image ohne das grosse Rust-Paket python3-cryptography.
RDEPENDS:${PN}:remove = "python3-cryptography"
