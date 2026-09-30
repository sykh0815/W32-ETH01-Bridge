# Änderungsprotokoll

🇬🇧 [English](CHANGELOG.md) | 🇩🇪 **Deutsch**

Alle wichtigen Änderungen an diesem Projekt stehen in dieser Datei.

Die Versionsnummern folgen dem Schema **MAJOR.MINOR.PATCH**:

- **MAJOR**: inkompatible Änderungen (z. B. Einstellungen müssen neu eingegeben werden)
- **MINOR**: neue Funktionen
- **PATCH**: nur Fehlerbehebungen (geschrieben z. B. als `2.2.1`; fehlt die dritte Stelle, ist `.0`
  gemeint)

Die aktuelle Version steht in `src/main.cpp` (`FIRMWARE_VERSION`) und wird im Webinterface und in
der seriellen Ausgabe angezeigt.

## [2.4] – 30.09.2026

### Neu
- Komplettes Webinterface zusätzlich auf **Englisch**, einschließlich aller Ergebnis- und
  Fehlerseiten und der per JavaScript erzeugten Texte.
- Sprachumschalter mit kleinen Flaggen-Buttons (DE / EN) oben rechts. Die Wahl wird ein Jahr lang
  im Browser gespeichert (Cookie); ohne Wahl gilt die Sprache des Browsers.
- `tools/release.sh`: baut die Firmware und legt einen GitHub-Release mit den Firmware-Dateien an
  (`bootloader.bin`, `partitions.bin`, `boot_app0.bin`, `firmware.bin`, eine zusammengefügte
  `-full.bin`, falls esptool vorhanden ist, und `SHA256SUMS.txt`) sowie zweisprachigen
  Release-Notizen aus den Changelogs.

### Geändert
- `/status` meldet zusätzlich, ob das Einrichtungs-WLAN offen ist (`apOpen`).

## [2.3] – 30.09.2026

### Geändert
- Das Einrichtungs-WLAN startet jetzt **ohne Passwort** (offen), bis im Webinterface ein Passwort
  festgelegt wird. `SETUP_AP_PASSWORD` in `src/main.cpp` ist standardmäßig leer; wer selbst baut,
  kann dort weiterhin ein festes Start-Passwort eintragen.
- Der Button im Abschnitt „Einrichtungs-WLAN“ heißt „Passwort festlegen“, solange kein Passwort
  gesetzt ist.

### Neu
- Auffällige, pulsierende rote Warnung oben im Webinterface, solange das Einrichtungs-WLAN offen
  ist, mit Link zum Passwortformular. Das Formular ist ebenfalls rot hervorgehoben.
- Warnung in der seriellen Ausgabe, wenn das Einrichtungs-WLAN offen ist.

## [2.2] – 30.09.2026

### Neu
- Das Passwort des Einrichtungs-WLANs lässt sich im Webinterface ändern (Abschnitt
  „Einrichtungs-WLAN“). Es wird im Flash gespeichert und bleibt bei Firmware-Updates erhalten.
- Warnung im Webinterface und in der seriellen Ausgabe, solange noch das Standard-Passwort des
  Einrichtungs-WLANs aktiv ist.
- Prüfung der Passwortregeln: 8 bis 63 druckbare ASCII-Zeichen, doppelte Eingabe.

### Geändert
- `SETUP_AP_PASSWORD` in `src/main.cpp` ist jetzt nur noch der Standardwert. Er gilt, bis ein
  eigenes Passwort gesetzt ist, und nach dem Löschen des Flash.

## [2.1] – 30.09.2026

### Neu
- Betriebsart **Bridge**: Das LAN-Gerät bekommt seine IP-Adresse direkt vom Router
  (MAC-Umschreibung, ein Gerät, nur IPv4). Zusätzlich zu NAT im Webinterface wählbar.
- Betriebsarten als Karten mit Piktogrammen und Angabe der Datenrate.
- Infos zum Gerät am LAN-Port: IP, MAC, Link-Geschwindigkeit/Duplex, Verbindungsdauer,
  Paketzähler im Bridge-Modus.
- Versionsnummer im Webinterface, in der seriellen Ausgabe und in `/status`.
- Projektbeschreibung und Tutorial auf Deutsch und Englisch, MIT-Lizenz.

### Behoben
- Die WLAN-Suche funktioniert jetzt auch, solange noch keine Router-Verbindung besteht
  (Verbindungsversuche werden während des Scans angehalten, der Scan läuft asynchron).
- Der DHCP-Server am Ethernet-Port startete nicht (fehlendes `ESP_NETIF_FLAG_AUTOUP`).
- Der DNS-Server wurde nicht korrekt an die DHCP-Clients weitergegeben.
- Der Oszillator des LAN8720 (GPIO16) wird eingeschaltet, bevor der EMAC startet.

## [2.0] und älter

Nur NAT-Modus, nicht im Detail dokumentiert. Die letzte reine NAT-Version liegt in
`backup/main_nat_only.cpp`.
