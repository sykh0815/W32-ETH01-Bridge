# WT32-ETH01 Ethernet-WLAN-Bridge (ESP32 Ethernet to WiFi Bridge)

🇬🇧 [English](README.md) | 🇩🇪 **Deutsch**

[![Version](https://img.shields.io/badge/Version-2.4-1263a6)](CHANGELOG.de.md)
[![Plattform](https://img.shields.io/badge/ESP32-WT32--ETH01-green)](#hardware)
[![PlatformIO](https://img.shields.io/badge/PlatformIO-Arduino%20Core%203.x-orange?logo=platformio)](#bauen-und-flashen)
[![Lizenz: MIT](https://img.shields.io/badge/Lizenz-MIT-yellow)](LICENSE)
[![Buy me a coffee](https://img.shields.io/badge/Buy%20me%20a%20coffee-FFDD00?logo=buymeacoffee&logoColor=black)](https://buymeacoffee.com/sykh)

**Version 2.4** – siehe [Änderungsprotokoll](CHANGELOG.de.md)

Firmware für das **WT32-ETH01 v1.4** (ESP32 + LAN8720), die ein Gerät mit LAN-Anschluss per WLAN
ins Netzwerk bringt: ein **WLAN-Adapter für Geräte ohne WLAN** oder eine **WLAN-Bridge für den
Ethernet-Anschluss**, z. B. für Drucker, Smart-TV, Spielkonsole, NAS, SPS oder Messgeräte. Das Gerät
wird per Kabel an den WT32-ETH01 angeschlossen, der WT32-ETH01 verbindet sich per WLAN mit dem Router.

<p align="center">
  <img src="docs/wt32-eth01.svg" alt="WT32-ETH01 Board mit RJ45-Buchse, LAN8720 PHY und ESP32-Modul (Illustration)" width="480">
</p>

```
[ Gerät mit LAN ] ──Kabel── [ WT32-ETH01 ] ))) WLAN ))) [ Router ] ── Internet
```

## Webinterface

<p align="center">
  <img src="docs/webinterface.png" alt="Webinterface der Bridge im Bridge-Modus: WLAN-Empfang, Infos zum Gerät am LAN-Port, WLAN-Suche und Auswahl der Betriebsart" width="420">
</p>

Das Webinterface (hier auf Englisch, Version 2.4) im Bridge-Modus: Sprachumschalter, Warnung
solange das Einrichtungs-WLAN kein Passwort hat, WLAN-Empfang, Daten des Geräts am LAN-Port
(IP-Adresse vom Router, Link-Geschwindigkeit, Paketzähler), Router-WLAN, Betriebsart und Passwort
des Einrichtungs-WLANs.

## Funktionen

- **Zwei Betriebsarten**, umschaltbar im Webinterface:
  - **NAT – eigenes Netzwerk** (Standard): eigenes Netz `192.168.50.0/24` am LAN-Port mit
    DHCP-Server, Internetfreigabe über NAPT. Mehrere Geräte möglich (z. B. über einen Switch).
    Datenrate bis ca. 10 Mbit/s.
  - **Bridge – direkt ins Heimnetz** (experimentell): Das LAN-Gerät bekommt seine IP **direkt vom
    Router**. Nur ein Gerät, nur IPv4. Datenrate über 30 Mbit/s (Details siehe unten).
- **Webinterface** auf **Deutsch und Englisch** (umschaltbar per Flaggen-Button) über ein eigenes
  Einrichtungs-WLAN (Passwort im Webinterface änderbar):
  - WLAN-Suche und Eingabe der Router-Zugangsdaten
  - Anzeige der WLAN-Signalstärke
  - Infos zum Gerät am LAN-Port: IP-Adresse, MAC-Adresse, Link-Geschwindigkeit/Duplex,
    Verbindungsdauer, im Bridge-Modus zusätzlich Paketzähler
- Zugangsdaten und Betriebsart werden dauerhaft im Flash (NVS) gespeichert.
- Keine externen Bibliotheken nötig.

## Hardware

- WT32-ETH01 v1.4 von Wireless-Tag: ESP32-Modul, LAN8720-Ethernet-PHY, RJ45-Buchse (10/100 Mbit/s),
  Versorgung mit 5 V **oder** 3,3 V
- USB-TTL-Adapter mit **3,3 V**-Logik zum Flashen (z. B. CP2102 oder CH340)

| Adapter | WT32-ETH01 |
|---------|------------|
| TX      | RX0 (GPIO3) |
| RX      | TX0 (GPIO1) |
| GND     | GND |
| 5V      | 5V (oder 3,3 V an 3V3, nur eines von beiden) |

Zum Flashen **IO0 mit GND verbinden** und dann das Board einschalten. Nach dem Flashen die Brücke
entfernen und neu starten.

### Pinbelegung der Stiftleisten

Laut [Wireless-Tag-Wiki](https://wiki.wireless-tag.com/docs/en/WT32-ETH01/board_features.html):

| Leiste 1 | Funktion | Leiste 2 | Funktion |
|----------|----------|----------|----------|
| EN | Enable (aktiv high) | GND | Masse |
| CFG | IO32 (Werksreset) | IO39 | nur Eingang |
| 485_EN | IO33 (RS485-Enable) | IO36 | nur Eingang |
| RXD | IO5 (UART2 RX) | IO15 | GPIO |
| TXD | IO17 (UART2 TX) | IO14 | GPIO |
| GND | Masse | IO12 | GPIO |
| 3V3 | 3,3 V Ein-/Ausgang | IO35 | nur Eingang |
| GND | Masse | IO4 | GPIO |
| 5V | 5 V Ein-/Ausgang | IO2 | GPIO |
| LINK | Link-LED | GND | Masse |

Zum Flashen dienen die Programmier-Pins **TXD0 (IO1), RXD0 (IO3), GND, 3V3, EN und IO0**.

Verwendete Pins intern: GPIO16 (Oszillator-Enable LAN8720), GPIO23 (MDC), GPIO18 (MDIO),
GPIO0 (50-MHz-RMII-Takt-Eingang), PHY-Adresse 1.

## Bauen und Flashen

Eine ausführliche Schritt-für-Schritt-Anleitung (auch ohne Kommandozeile) steht im
**[Tutorial](docs/TUTORIAL.de.md)**.

Kurzfassung mit [PlatformIO](https://platformio.org/) (`brew install platformio`):

```bash
pio run -t upload        # bauen und flashen
pio device monitor       # serielle Ausgabe (115200 Baud)
```

Die Firmware braucht **Arduino-Core 3.x (ESP-IDF 5)**. Die `platformio.ini` nutzt dafür die
[pioarduino](https://github.com/pioarduino/platform-espressif32)-Plattform. Mit dem Standard-Paket
`platform = espressif32` (Arduino-Core 2.x) lässt sich der Ethernet-Teil nicht kompilieren.

## Einrichtung

1. Mit dem WLAN **`WT32-Bridge-Setup`** verbinden. Beim ersten Start ist es **offen (ohne Passwort)**.
2. `http://192.168.4.1` öffnen.
3. Router-WLAN auswählen, Passwort eingeben, speichern.
4. Ein Passwort für das Einrichtungs-WLAN festlegen (bis dahin zeigt das Webinterface eine rote Warnung).
5. Gerät per Kabel am LAN-Port anschließen.

## WLAN-Passwörter

Die Bridge kennt zwei Passwörter:

| Passwort | Wo steht es? | Wie ändern? |
|----------|--------------|-------------|
| **Einrichtungs-WLAN** `WT32-Bridge-Setup` (anfangs **offen**, ohne Passwort) | im Flash des ESP32 (NVS); ein optionaler Standardwert lässt sich in `SETUP_AP_PASSWORD` in `src/main.cpp` setzen | im Webinterface unter „Einrichtungs-WLAN“ (8–63 Zeichen), die Bridge startet danach neu |
| **Router-WLAN** | im Flash des ESP32 (NVS), nicht im Code | im Webinterface unter „Router-WLAN“ neu eingeben und „Speichern und verbinden“ |

> **Wichtig:** Beim ersten Start ist das Einrichtungs-WLAN offen, jeder in Reichweite könnte die
> Einstellungen ändern. Das Webinterface zeigt eine auffällige rote Warnung, bis ein Passwort gesetzt
> ist. Lege es direkt nach der Einrichtung fest. Details: [Tutorial, Abschnitt WLAN-Passwörter](docs/TUTORIAL.de.md#7-wlan-passwörter-finden-und-ändern).

## Betriebsarten im Detail

### NAT – eigenes Netzwerk

Die Bridge baut am LAN-Port ein eigenes Netz auf und vergibt die Adressen selbst. Ideal, wenn
mehrere Geräte über einen Switch angeschlossen werden sollen. Datenraten bis ca. 10 Mbit/s.

- Bridge-Adresse im LAN: `192.168.50.1` (Gateway)
- DHCP-Bereich: `192.168.50.x`, DNS: `1.1.1.1`
- Internetverkehr wird per NAPT über das WLAN geleitet.

### Bridge – direkt ins Heimnetz (experimentell)

Das angeschlossene Gerät erhält seine IP-Adresse direkt vom Router und ist im Heimnetz wie jedes
andere Gerät erreichbar. Datenraten über 30 Mbit/s.

Ein WLAN-Client darf nur Pakete mit seiner eigenen MAC-Adresse senden. Die Bridge schreibt deshalb
die MAC-Adressen in den Paketen um, auch in DHCP- und ARP-Paketen. Das gleiche Verfahren nutzt
Espressifs Beispiel
[`sta2eth`](https://github.com/espressif/esp-idf/tree/master/examples/network/sta2eth).

Einschränkungen:

- nur **ein** Gerät am LAN-Port, nur **IPv4**
- im Router erscheint das Gerät mit der **WLAN-MAC der Bridge** (wird im Webinterface angezeigt).
  Feste IP-Zuweisungen im Router müssen auf diese MAC eingetragen werden.
- die Bridge hat im Router-Netz keine eigene IP; das Webinterface ist nur über das
  Einrichtungs-WLAN erreichbar
- nutzt interne ESP-IDF-WLAN-Funktionen (`esp_wifi_internal_*`), die sich mit Core-Updates
  ändern können
- nach dem Umschalten der Betriebsart am LAN-Gerät kurz das Kabel ziehen, damit es eine neue
  Adresse holt

## Projektstruktur

```
platformio.ini            Build-Konfiguration
src/main.cpp              Firmware
docs/TUTORIAL.de.md       Tutorial: Kompilieren und Flashen (Deutsch)
docs/TUTORIAL.en.md       Tutorial: build and flash (English)
docs/webinterface.png     Screenshot des Webinterface
docs/wt32-eth01.svg       Illustration des Boards
docs/social-preview.png   Vorschaubild für GitHub
CHANGELOG.de.md           Änderungsprotokoll (Deutsch)
CHANGELOG.md              changelog (English)
LICENSE                   MIT-Lizenz
tools/release.sh          baut die Firmware und legt einen GitHub-Release an
backup/main_nat_only.cpp  ältere Version nur mit NAT-Modus
```

## Versionen

Aktuelle Version: **2.4** – Webinterface auf Deutsch und Englisch mit Sprachumschalter.
Fertige Firmware-Dateien hängen an jedem [Release](../../releases).
Alle Änderungen stehen im **[Änderungsprotokoll](CHANGELOG.de.md)**.

## Unterstützen

Wenn dir das Projekt hilft, freue ich mich über einen Kaffee:

<a href="https://buymeacoffee.com/sykh"><img src="https://img.shields.io/badge/Buy%20me%20a%20coffee-FFDD00?style=for-the-badge&logo=buymeacoffee&logoColor=black" alt="Buy me a coffee"></a>

## Fehlersuche

Die serielle Ausgabe (115200 Baud) zeigt den Zustand an, z. B.:

```
Betriebsart: NAT
DHCP server started on interface ETH_LAN with IP: 192.168.50.1
Ethernet-LAN: 192.168.50.1, DHCP-Server laeuft
Ethernet-Kabel verbunden (100 Mbit/s, Vollduplex)
LAN-Geraet AA:BB:CC:DD:EE:FF hat 192.168.50.2 bekommen
```

Weitere Hinweise stehen im [Tutorial](docs/TUTORIAL.de.md#8-probleme-lösen).

## Lizenz

Veröffentlicht unter der [MIT-Lizenz](LICENSE): Der Code darf frei verwendet, verändert und
weitergegeben werden, auch kommerziell. Der Copyright-Hinweis muss dabei erhalten bleiben.
