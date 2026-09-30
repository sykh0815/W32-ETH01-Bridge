# WT32-ETH01 Ethernet-WLAN-Bridge

Firmware für das **WT32-ETH01 v1.4** (ESP32 + LAN8720), die ein Gerät mit LAN-Anschluss per WLAN
ins Netzwerk bringt. Das Gerät wird per Kabel an den WT32-ETH01 angeschlossen, der WT32-ETH01
verbindet sich per WLAN mit dem Router.

```
[ Gerät mit LAN ] ──Kabel── [ WT32-ETH01 ] ))) WLAN ))) [ Router ] ── Internet
```

## Funktionen

- **Zwei Betriebsarten**, umschaltbar im Webinterface:
  - **NAT** (Standard): eigenes Netz `192.168.50.0/24` am LAN-Port mit DHCP-Server,
    Internetfreigabe über NAPT. Mehrere Geräte möglich (z. B. über einen Switch).
  - **Bridge** (experimentell): Das LAN-Gerät bekommt seine IP **direkt vom Router**.
    Nur ein Gerät, nur IPv4 (Details siehe unten).
- **Webinterface** über ein eigenes Einrichtungs-WLAN:
  - WLAN-Suche und Eingabe der Router-Zugangsdaten
  - Anzeige der WLAN-Signalstärke
  - Infos zum Gerät am LAN-Port: IP-Adresse, MAC-Adresse, Link-Geschwindigkeit/Duplex,
    Verbindungsdauer, im Bridge-Modus zusätzlich Paketzähler
- Zugangsdaten und Betriebsart werden dauerhaft im Flash (NVS) gespeichert.
- Keine externen Bibliotheken nötig.

## Hardware

- WT32-ETH01 v1.4
- USB-TTL-Adapter mit **3,3 V**-Logik zum Flashen (z. B. CP2102 oder CH340)

| Adapter | WT32-ETH01 |
|---------|------------|
| TX      | RX0 (GPIO3) |
| RX      | TX0 (GPIO1) |
| GND     | GND |
| 5V      | 5V (oder 3,3 V an 3V3, nur eines von beiden) |

Zum Flashen **IO0 mit GND verbinden** und dann das Board einschalten. Nach dem Flashen die Brücke
entfernen und neu starten.

Verwendete Pins intern: GPIO16 (Oszillator-Enable LAN8720), GPIO23 (MDC), GPIO18 (MDIO),
GPIO0 (50-MHz-RMII-Takt-Eingang), PHY-Adresse 1.

## Bauen und Flashen

Voraussetzung: [PlatformIO](https://platformio.org/) (`brew install platformio`).

```bash
pio run -t upload        # bauen und flashen
pio device monitor       # serielle Ausgabe (115200 Baud)
```

Die Firmware braucht **Arduino-Core 3.x (ESP-IDF 5)**. Die `platformio.ini` nutzt dafür die
[pioarduino](https://github.com/pioarduino/platform-espressif32)-Plattform. Mit dem Standard-Paket
`platform = espressif32` (Arduino-Core 2.x) lässt sich der Ethernet-Teil nicht kompilieren.

## Einrichtung

1. Mit dem WLAN **`WT32-Bridge-Setup`** verbinden (Standard-Passwort: `BridgeSetup26`).
2. `http://192.168.4.1` öffnen.
3. Router-WLAN auswählen, Passwort eingeben, speichern.
4. Gerät per Kabel am LAN-Port anschließen.

> **Hinweis:** Das Passwort des Einrichtungs-WLANs ist im Quellcode festgelegt
> (`SETUP_AP_PASSWORD` in `src/main.cpp`). Bitte vor dem Einsatz ändern.

## Betriebsarten im Detail

### NAT

- Bridge-Adresse im LAN: `192.168.50.1` (Gateway)
- DHCP-Bereich: `192.168.50.x`, DNS: `1.1.1.1`
- Internetverkehr wird per NAPT über das WLAN geleitet.

### Bridge (experimentell)

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
ANLEITUNG.md              Schritt-für-Schritt-Anleitung zum Flashen und Fehlersuche
backup/main_nat_only.cpp  ältere Version nur mit NAT-Modus
```

## Fehlersuche

Die serielle Ausgabe (115200 Baud) zeigt den Zustand an, z. B.:

```
Betriebsart: NAT
DHCP server started on interface ETH_LAN with IP: 192.168.50.1
Ethernet-LAN: 192.168.50.1, DHCP-Server laeuft
Ethernet-Kabel verbunden (100 Mbit/s, Vollduplex)
LAN-Geraet AA:BB:CC:DD:EE:FF hat 192.168.50.2 bekommen
```

Weitere Hinweise stehen in der [ANLEITUNG.md](ANLEITUNG.md).
