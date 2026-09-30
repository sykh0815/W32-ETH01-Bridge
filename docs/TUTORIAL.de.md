# Tutorial: Firmware kompilieren und auf den WT32-ETH01 übertragen

🇬🇧 [English](TUTORIAL.en.md) | 🇩🇪 **Deutsch**

Diese Anleitung führt Schritt für Schritt vom leeren Rechner bis zur laufenden Bridge. Der
einfachste Weg ist **Visual Studio Code mit der Erweiterung PlatformIO**: Alles geht per Mausklick,
eine Kommandozeile ist nicht nötig. Für Fortgeschrittene steht jeweils auch der Befehl für das
Terminal dabei.

Zeitbedarf: etwa 20–30 Minuten, davon viel Wartezeit beim ersten Download der Werkzeuge.

---

## 1. Was du brauchst

| Teil | Hinweis |
|------|---------|
| WT32-ETH01 (v1.4) | hat keinen eigenen USB-Anschluss |
| USB-TTL-Adapter mit **3,3 V**-Logik | z. B. mit CP2102- oder CH340-Chip. Adapter mit Umschalter auf 3,3 V stellen. |
| 5–6 Jumper-Kabel (Dupont, Buchse–Buchse) | |
| USB-Kabel **mit Datenleitungen** | reine Ladekabel funktionieren nicht |
| Rechner mit Windows, macOS oder Linux | |

---

## 2. Software installieren

### Variante A: Visual Studio Code + PlatformIO (empfohlen)

1. **Visual Studio Code** herunterladen und installieren: <https://code.visualstudio.com/>
2. VS Code starten, links auf das Symbol **Erweiterungen** klicken (vier Quadrate) oder
   `Strg+Umschalt+X` (Mac: `Cmd+Umschalt+X`) drücken.
3. Nach **PlatformIO IDE** suchen und **Installieren** klicken.
4. Warten, bis die Installation fertig ist (unten rechts erscheint eine Meldung), dann VS Code
   neu starten. Links erscheint ein neues Symbol mit einem Ameisenkopf: das ist PlatformIO.

### Variante B: nur Kommandozeile

- macOS: `brew install platformio`
- Windows, Linux oder macOS ohne Homebrew: `python3 -m pip install --user platformio`

### USB-Treiber (falls nötig)

- **CP2102/CP210x:** Treiber von Silicon Labs („CP210x USB to UART Bridge VCP Drivers“)
- **CH340/CH341:** Treiber vom Hersteller WCH
- Unter macOS und Linux werden beide Chips meist ohne Treiber erkannt, unter Windows oft auch.

---

## 3. Projekt herunterladen

**Ohne Git:** Auf der GitHub-Projektseite auf den grünen Button **Code** klicken, dann auf
**Download ZIP**. Die ZIP-Datei entpacken, z. B. nach `Dokumente/WT32-ETH01-Bridge`.

**Mit Git:**

```bash
git clone https://github.com/<benutzername>/WT32-ETH01-Bridge.git
```

`<benutzername>` durch den GitHub-Namen des Projekts ersetzen oder die Adresse über den Button
**Code** kopieren.

In VS Code: **Datei → Ordner öffnen …** und den Projektordner wählen (den Ordner, in dem die Datei
`platformio.ini` liegt). PlatformIO erkennt das Projekt automatisch.

> Beim ersten Öffnen lädt PlatformIO die ESP32-Werkzeuge herunter (mehrere hundert MB). Das dauert
> einige Minuten, der Fortschritt steht unten in der Statusleiste.

---

## 4. Board anschließen

### Verkabelung

| USB-TTL-Adapter | WT32-ETH01 |
|-----------------|------------|
| TX              | RX0 (GPIO3) |
| RX              | TX0 (GPIO1) |
| GND             | GND |
| 5V              | 5V |
| –               | **IO0 an GND** (nur zum Flashen) |

- **TX und RX werden gekreuzt:** TX des Adapters an RX des Boards und umgekehrt.
- **Stromversorgung entweder über 5V oder über 3V3, niemals beides.** 5 V ist meist stabiler,
  weil der 3,3-V-Ausgang vieler Adapter für den ESP32 zu schwach ist.

### Board in den Flash-Modus bringen

1. Kabel wie oben stecken, **IO0 mit GND verbinden**.
2. Erst jetzt den Adapter per USB an den Rechner anschließen. Das Board startet damit im
   Flash-Modus.
3. Hat das Board schon Strom: EN kurz mit GND verbinden (Reset), während IO0 auf GND liegt.

Welcher Port der Adapter ist, zeigt PlatformIO unter **PlatformIO → Devices** an. Typische Namen:
`COM3` (Windows), `/dev/cu.usbserial-…` oder `/dev/cu.wchusbserial…` (macOS),
`/dev/ttyUSB0` (Linux).

---

## 5. Kompilieren und hochladen

### In VS Code (per Mausklick)

In der blauen Statusleiste unten gibt es drei wichtige Symbole:

| Symbol | Funktion |
|--------|----------|
| ✓ (Häkchen) | **Build**: nur kompilieren |
| → (Pfeil) | **Upload**: kompilieren und auf das Board übertragen |
| 🔌 (Stecker) | **Serial Monitor**: Ausgaben des Boards anzeigen |

1. Auf **→ Upload** klicken. PlatformIO kompiliert die Firmware und überträgt sie. Am Ende steht
   im Terminal **SUCCESS**.
2. **IO0 von GND trennen.**
3. Board neu starten: USB kurz abziehen und wieder anstecken, oder EN kurz auf GND.
4. Auf **🔌 Serial Monitor** klicken. Es erscheint unter anderem:

   ```
   WT32-ETH01 Ethernet-WLAN-Bridge, Firmware 2.4
   Einrichtungsseite: http://192.168.4.1
   ```

Die gleichen Befehle findest du auch links unter **PlatformIO → Project Tasks → wt32-eth01 →
General**.

### Im Terminal

```bash
cd Pfad/zum/WT32-ETH01-Bridge
pio run -t upload          # kompilieren und flashen
# IO0 von GND trennen, Board neu starten
pio device monitor         # Ausgabe ansehen, beenden mit Strg+C
```

Falls PlatformIO den Port nicht selbst findet, in `platformio.ini` eintragen:

```ini
upload_port = /dev/cu.usbserial-0001   ; Windows z. B. COM3
monitor_port = /dev/cu.usbserial-0001
```

---

## 6. Bridge einrichten

1. Mit dem Handy oder Laptop mit dem WLAN **`WT32-Bridge-Setup`** verbinden.
   Beim ersten Start ist es **offen**, ein Passwort ist nicht nötig (siehe Abschnitt 7).
2. Im Browser **http://192.168.4.1** öffnen. Die Seite richtet sich nach der Sprache des Browsers;
   mit den Flaggen-Buttons **DE / EN** oben rechts lässt sie sich zwischen Deutsch und Englisch
   umschalten.
3. **Verfügbare WLANs suchen**, dein Router-WLAN antippen, Passwort eingeben,
   **Speichern und verbinden**.
4. Betriebsart wählen: **NAT** (eigenes Netz, mehrere Geräte) oder **Bridge** (IP direkt vom
   Router, ein Gerät). Nach **Übernehmen und neu starten** startet die Bridge neu.
5. Das Gerät per Netzwerkkabel am LAN-Port anschließen. Unter „Gerät am LAN-Port“ erscheinen
   IP- und MAC-Adresse.

---

## 7. WLAN-Passwörter finden und ändern

Die Bridge arbeitet mit **zwei verschiedenen** Passwörtern.

### Passwort des Einrichtungs-WLANs (`WT32-Bridge-Setup`)

Das ist das WLAN, das die Bridge selbst aufspannt, damit du das Webinterface erreichst.

- **Beim ersten Start** ist das Einrichtungs-WLAN **offen** (ohne Passwort), damit du sofort
  hineinkommst. Oben im Webinterface erscheint eine pulsierende rote Warnung **„Kein
  WLAN-Passwort gesetzt!“**, bis du ein Passwort festlegst. Mach das direkt nach der Einrichtung.
- **Festlegen oder ändern im Webinterface:**
  1. Mit dem Einrichtungs-WLAN verbinden und http://192.168.4.1 öffnen.
  2. In der roten Warnung auf **Jetzt Passwort festlegen** tippen oder ganz unten zum Abschnitt
     **Einrichtungs-WLAN** scrollen und das Passwort zweimal eingeben.
  3. **Passwort festlegen** (bzw. **Passwort ändern**) klicken. Die Bridge startet neu.
  4. Auf dem Handy oder Laptop das WLAN `WT32-Bridge-Setup` „vergessen“ und mit dem neuen
     Passwort neu verbinden.
- **Regeln:** 8 bis 63 Zeichen, nur Buchstaben, Ziffern, Leerzeichen und übliche Sonderzeichen,
  keine Umlaute.
- **Wo wird es gespeichert?** Im Flash des ESP32 (NVS). Es bleibt bei Stromausfall und
  Firmware-Updates erhalten.
- **Passwort vergessen?** Den Flash löschen (siehe unten, „Alles löschen“). Danach ist das
  Einrichtungs-WLAN wieder offen.
- **Festes Start-Passwort für eigene Builds:** Wer statt eines offenen WLANs ein Start-Passwort
  möchte, trägt es in `src/main.cpp` bei `SETUP_AP_PASSWORD` ein (8–63 Zeichen) und flasht neu.
  Es gilt nur, solange im Webinterface kein eigenes Passwort gesetzt wurde. Dort lässt sich auch
  der WLAN-Name (`SETUP_AP_SSID`) ändern.

> **Wichtig:** Solange kein Passwort gesetzt ist, kann jeder in Reichweite das Einrichtungs-WLAN
> nutzen und alle Einstellungen der Bridge ändern.

### Passwort deines Router-WLANs

- **Wo steht es?** Nicht im Code. Du gibst es im Webinterface ein, und die Bridge speichert es im
  Flash-Speicher des ESP32 (NVS). Es bleibt auch bei Stromausfall und nach einem Firmware-Update
  erhalten.
- **Ändern:** Im Webinterface unter **Router-WLAN** das neue Passwort eintragen und
  **Speichern und verbinden** klicken. Bleibt das Passwortfeld leer, behält die Bridge das bisherige.
- **Anzeigen:** Aus Sicherheitsgründen zeigt das Webinterface das gespeicherte Passwort nie an.
- **Alles löschen** (Router-Zugangsdaten, Betriebsart und Passwort des Einrichtungs-WLANs): den Flash komplett löschen und die
  Firmware neu aufspielen.
  - VS Code: **PlatformIO → Project Tasks → wt32-eth01 → Platform → Erase Flash**, danach
    **Upload**
  - Terminal: `pio run -t erase` und danach `pio run -t upload`

  Beide Schritte brauchen wie beim Flashen IO0 an GND.

---

## 8. Probleme lösen

| Meldung oder Verhalten | Ursache und Lösung |
|------------------------|--------------------|
| Kein Port sichtbar | USB-Kabel ohne Datenleitungen, fehlender Treiber (Abschnitt 2) oder Adapter nicht eingesteckt |
| `Failed to connect to ESP32: Wrong boot mode detected` | IO0 lag beim Start nicht auf GND. IO0 an GND, Board neu starten, nochmal hochladen. |
| `Timed out waiting for packet header` | TX und RX vertauscht, oder GND fehlt |
| Upload bricht mittendrin ab | in `platformio.ini` `upload_speed = 115200` eintragen |
| Board startet ständig neu (`Brownout detector`) | Stromversorgung zu schwach: über den 5V-Pin versorgen |
| Kein WLAN `WT32-Bridge-Setup` | IO0 noch auf GND (Board wartet im Flash-Modus) oder Passwort kürzer als 8 Zeichen |
| `LAN8720-Treiber konnte nicht gestartet werden` | Board-Version prüfen (v1.4), Stromversorgung prüfen |
| Kompilierfehler zu Ethernet-Funktionen | falsche Plattform: in `platformio.ini` muss die pioarduino-Plattform stehen |
| Alte Build-Reste | Ordner `.pio` im Projekt löschen und neu kompilieren |

Im **Serial Monitor** (115200 Baud) siehst du, was die Bridge gerade macht. Diese Ausgabe hilft
auch, wenn du Hilfe suchst.

---

## Anhang: Fertige Firmware im Browser flashen (ohne Compiler)

Wenn im Bereich **Releases** des Projekts fertige Dateien (`bootloader.bin`, `partitions.bin`,
`boot_app0.bin`, `firmware.bin`) angeboten werden, geht es auch ohne PlatformIO:

1. In **Chrome** oder **Edge** <https://espressif.github.io/esptool-js/> öffnen (Firefox und
   Safari unterstützen den nötigen seriellen Zugriff nicht).
2. Board wie in Abschnitt 4 in den Flash-Modus bringen, **Connect** klicken und den Port wählen.
3. Die Dateien mit diesen Adressen eintragen:

   | Adresse | Datei |
   |---------|-------|
   | `0x1000` | `bootloader.bin` |
   | `0x8000` | `partitions.bin` |
   | `0xe000` | `boot_app0.bin` |
   | `0x10000` | `firmware.bin` |

   Einfacher: nur die Datei `wt32-eth01-bridge-v…-full.bin` mit der Adresse `0x0` eintragen, sie
   enthält alle vier Teile.

4. **Program** klicken, danach IO0 von GND trennen und neu starten.

Bei einem Update von einer älteren Version bleiben alle Einstellungen erhalten (solange im Web-Flasher nicht „Erase Flash“ gewählt wird).

Wer selbst kompiliert, findet die Dateien nach `pio run` unter `.pio/build/wt32-eth01/`
(`boot_app0.bin` liegt im PlatformIO-Paket `framework-arduinoespressif32` unter
`tools/partitions/`).
