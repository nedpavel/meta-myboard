// Voreinstellungen fuer den Kiosk (browser-kiosk). Firefox liest alle
// Dateien in defaults/pref als Vorgaben. Updates, Telemetrie und
// Erststart-Seiten regelt distribution/policies.json.

// Nach pkill oder Stromausfall weder "Sitzung wiederherstellen" noch die
// Frage nach dem abgesicherten Modus - der Kiosk soll einfach die Seite
// zeigen.
pref("browser.sessionstore.resume_from_crash", false);
pref("toolkit.startup.max_resumed_crashes", -1);

// Keine Hinweis- und Neuigkeitsseiten beim Start
pref("browser.startup.homepage_override.mstone", "ignore");
pref("browser.aboutwelcome.enabled", false);
pref("datareporting.policy.dataSubmissionPolicyBypassNotification", true);

// Keine Uebersetzungs-Leiste ueber den deutschen Seiten
pref("browser.translations.automaticallyPopup", false);

// Weniger Schreibzugriffe auf den Flash-Speicher: Cache nur im RAM,
// Sitzungsstand alle 10 Minuten statt alle 15 Sekunden
pref("browser.cache.disk.enable", false);
pref("browser.sessionstore.interval", 600000);
