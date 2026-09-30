# WT32-ETH01 Bridge - bauen und flashen

## 1. PlatformIO installieren (einmalig)

    brew install platformio

(Ohne Homebrew: `python3 -m pip install --user platformio`)

## 2. Verkabelung USB-TTL-Adapter (3,3 V Logik!)

| Adapter | WT32-ETH01 |
|---------|------------|
| TX      | RX0 (GPIO3) |
| RX      | TX0 (GPIO1) |
| GND     | GND |
| 5V      | 5V  (oder 3V3 -> 3V3, nur eines) |

Zum Flashen: **IO0 mit GND verbinden**, dann Strom anlegen (oder EN kurz auf GND).

## 3. Bauen und flashen

Im Terminal in diesem Ordner:

    cd ~/Documents/W32-ETH01-Bridge
    pio run -t upload

Der erste Lauf laedt die Toolchain herunter (einige Minuten).

## 4. Starten und Log ansehen

1. Bruecke IO0-GND entfernen
2. Neu starten (Strom kurz trennen oder EN auf GND)
3. Seriellen Monitor oeffnen:

        pio device monitor

Beenden mit `Strg+C`.

## Erwartete Ausgabe

    DHCP server started on interface ETH_LAN with IP: 192.168.50.1
    Ethernet-LAN: 192.168.50.1, DHCP-Server laeuft
    Einrichtungsseite: http://192.168.4.1
    Ethernet-Kabel verbunden

Setup-WLAN: `WT32-Bridge-Setup`, Passwort `BridgeSetup26`, dann http://192.168.4.1

## Probleme

- **Port nicht gefunden:** `ls /dev/cu.*` ausfuehren und in `platformio.ini`
  `upload_port` / `monitor_port` eintragen. CH340-Adapter brauchen evtl. einen Treiber.
- **"Failed to connect / Wrong boot mode":** IO0 war beim Einschalten nicht auf GND.
- **Alter Build-Muell:** Ordner `.pio` loeschen und neu bauen.

## Betriebsarten (im Webinterface unter "Betriebsart", danach Neustart)

**NAT** (Standard): eigenes Netz 192.168.50.x mit DHCP-Server der Bridge. Mehrere Geraete moeglich
(z. B. ueber einen Switch).

**Bridge**: Das LAN-Geraet holt sich seine IP direkt vom Router.
- nur **ein** Geraet am LAN-Port, nur IPv4
- im Router erscheint das Geraet mit der **WLAN-MAC der Bridge** (steht im Webinterface).
  Feste IP-Zuweisungen im Router muessen auf diese MAC eingetragen werden.
- die Bridge hat im Router-Netz keine eigene IP; Webinterface nur ueber das Setup-WLAN (192.168.4.1)
- nach dem Umschalten am LAN-Geraet kurz das Kabel ziehen, damit es eine neue IP holt

Erwartete Log-Ausgabe im Bridge-Modus:

    Betriebsart: Bridge
    Bridge-Modus: Ethernet <-> WLAN, Geraet erscheint im Router als 24:0A:...
    WLAN verbunden (Bridge-Modus)
    Ethernet-Kabel verbunden (100 Mbit/s, Vollduplex)
    Router hat dem LAN-Geraet 192.168.1.x zugewiesen

Die vorherige Version (nur NAT) liegt unter `backup/main_nat_only.cpp`.
