# Tutorial: build the firmware and flash it to the WT32-ETH01

🇬🇧 **English** | 🇩🇪 [Deutsch](TUTORIAL.de.md)

This guide takes you step by step from an empty computer to a running bridge. The easiest way is
**Visual Studio Code with the PlatformIO extension**: everything works with mouse clicks, no
command line needed. For advanced users the terminal command is listed as well.

Time needed: about 20–30 minutes, much of it waiting for the tools to download the first time.

---

## 1. What you need

| Part | Note |
|------|------|
| WT32-ETH01 (v1.4) | has no USB port of its own |
| USB-to-serial adapter with **3.3 V** logic | e.g. with a CP2102 or CH340 chip. Set adapters with a voltage switch to 3.3 V. |
| 5–6 jumper wires (Dupont, female–female) | |
| USB cable **with data lines** | charge-only cables do not work |
| Computer running Windows, macOS or Linux | |

---

## 2. Install the software

### Option A: Visual Studio Code + PlatformIO (recommended)

1. Download and install **Visual Studio Code**: <https://code.visualstudio.com/>
2. Start VS Code, click the **Extensions** icon on the left (four squares) or press
   `Ctrl+Shift+X` (Mac: `Cmd+Shift+X`).
3. Search for **PlatformIO IDE** and click **Install**.
4. Wait until the installation has finished (a message appears at the bottom right), then restart
   VS Code. A new icon with an ant head appears on the left: that's PlatformIO.

### Option B: command line only

- macOS: `brew install platformio`
- Windows, Linux or macOS without Homebrew: `python3 -m pip install --user platformio`

### USB drivers (if needed)

- **CP2102/CP210x:** driver from Silicon Labs ("CP210x USB to UART Bridge VCP Drivers")
- **CH340/CH341:** driver from the manufacturer WCH
- macOS and Linux usually detect both chips without a driver, and Windows often does too.

---

## 3. Download the project

**Without Git:** on the GitHub project page click the green **Code** button, then
**Download ZIP**. Unzip the file, e.g. to `Documents/WT32-ETH01-Bridge`.

**With Git:**

```bash
git clone https://github.com/<username>/WT32-ETH01-Bridge.git
```

Replace `<username>` with the GitHub name of the project, or copy the address via the **Code**
button.

In VS Code: **File → Open Folder…** and choose the project folder (the folder that contains
`platformio.ini`). PlatformIO detects the project automatically.

> The first time, PlatformIO downloads the ESP32 tools (several hundred MB). This takes a few
> minutes; progress is shown in the status bar at the bottom.

---

## 4. Connect the board

### Wiring

| USB-to-serial adapter | WT32-ETH01 |
|-----------------------|------------|
| TX                    | RX0 (GPIO3) |
| RX                    | TX0 (GPIO1) |
| GND                   | GND |
| 5V                    | 5V |
| –                     | **IO0 to GND** (for flashing only) |

- **TX and RX are crossed:** adapter TX goes to board RX and vice versa.
- **Power either via 5V or via 3V3, never both.** 5 V is usually more stable, because the 3.3 V
  output of many adapters is too weak for the ESP32.

### Put the board into flash mode

1. Connect the wires as above and **connect IO0 to GND**.
2. Only now plug the adapter into your computer. The board starts in flash mode.
3. If the board is already powered: briefly connect EN to GND (reset) while IO0 is on GND.

PlatformIO shows which port the adapter uses under **PlatformIO → Devices**. Typical names:
`COM3` (Windows), `/dev/cu.usbserial-…` or `/dev/cu.wchusbserial…` (macOS),
`/dev/ttyUSB0` (Linux).

---

## 5. Build and upload

### In VS Code (with mouse clicks)

The blue status bar at the bottom has three important icons:

| Icon | Function |
|------|----------|
| ✓ (check mark) | **Build**: compile only |
| → (arrow) | **Upload**: compile and transfer to the board |
| 🔌 (plug) | **Serial Monitor**: show the board's output |

1. Click **→ Upload**. PlatformIO compiles the firmware and transfers it. At the end the terminal
   shows **SUCCESS**.
2. **Disconnect IO0 from GND.**
3. Restart the board: unplug USB briefly and plug it back in, or briefly connect EN to GND.
4. Click **🔌 Serial Monitor**. Among other things you'll see:

   ```
   WT32-ETH01 Ethernet-WLAN-Bridge, Firmware 2.4
   Einrichtungsseite: http://192.168.4.1
   ```

The same commands are also available on the left under **PlatformIO → Project Tasks →
wt32-eth01 → General**.

### In the terminal

```bash
cd path/to/WT32-ETH01-Bridge
pio run -t upload          # build and flash
# disconnect IO0 from GND, restart the board
pio device monitor         # show output, quit with Ctrl+C
```

If PlatformIO doesn't find the port by itself, add it to `platformio.ini`:

```ini
upload_port = /dev/cu.usbserial-0001   ; Windows e.g. COM3
monitor_port = /dev/cu.usbserial-0001
```

---

## 6. Set up the bridge

1. Connect your phone or laptop to the WiFi **`WT32-Bridge-Setup`**.
   On first start it is **open**, no password needed (see section 7).
2. Open **http://192.168.4.1** in the browser. The page follows your browser language; use the
   flag buttons **DE / EN** at the top right to switch between German and English.
3. Click **Search for WiFi networks**, tap your router's WiFi, enter the password and click
   **Save and connect**.
4. Choose the operating mode: **NAT** (own network, several devices) or **Bridge** (IP directly
   from the router, one device). After **Apply and restart** the bridge restarts.
5. Connect the device to the LAN port with a network cable. Its IP and MAC address appear under
   "Device on the LAN port".

---

## 7. Find and change the WiFi passwords

The bridge uses **two different** passwords.

### Setup WiFi password (`WT32-Bridge-Setup`)

This is the WiFi network the bridge creates itself so you can reach the web interface.

- **On first start** the setup WiFi is **open** (no password) so you can get in right away. A
  pulsing red warning **"No WiFi password set!"** appears at the top
  of the web interface until you set a password. Do this right after setup.
- **Set or change it in the web interface:**
  1. Connect to the setup WiFi and open http://192.168.4.1.
  2. Tap **Set a password now** in the red warning, or scroll down to the section
     **Setup WiFi**, and enter the password twice.
  3. Click **Set password** or **Change password**. The
     bridge restarts.
  4. On your phone or laptop, "forget" the WiFi `WT32-Bridge-Setup` and reconnect with the new
     password.
- **Rules:** 8 to 63 characters; letters, digits, spaces and common special characters only, no
  umlauts or other non-ASCII characters.
- **Where is it stored?** In the ESP32's flash (NVS). It survives power loss and firmware updates.
- **Forgot the password?** Erase the flash (see "Erase everything" below). Afterwards the setup
  WiFi is open again.
- **Fixed start password for your own builds:** if you prefer a start password instead of an open
  WiFi, enter it in `src/main.cpp` at `SETUP_AP_PASSWORD` (8–63 characters) and flash again. It
  only applies as long as no custom password has been set in the web interface. You can also
  change the WiFi name (`SETUP_AP_SSID`) there.

> **Important:** as long as no password is set, anyone in range can use the setup WiFi and change
> all settings of the bridge.

### Your router's WiFi password

- **Where is it?** Not in the code. You enter it in the web interface, and the bridge stores it in
  the ESP32's flash memory (NVS). It survives power loss and firmware updates.
- **Change it:** enter the new password in the web interface under **Router WiFi** and click
  **Save and connect**. If the password field is left empty, the bridge keeps the old one.
- **Show it:** for security reasons the web interface never displays the stored password.
- **Erase everything** (router credentials, operating mode and setup WiFi password): erase the whole flash and upload
  the firmware again.
  - VS Code: **PlatformIO → Project Tasks → wt32-eth01 → Platform → Erase Flash**, then
    **Upload**
  - Terminal: `pio run -t erase`, then `pio run -t upload`

  Both steps need IO0 on GND, just like flashing.

---

## 8. Troubleshooting

| Message or behavior | Cause and fix |
|---------------------|---------------|
| No port visible | USB cable without data lines, missing driver (section 2), or adapter not plugged in |
| `Failed to connect to ESP32: Wrong boot mode detected` | IO0 was not on GND at startup. Connect IO0 to GND, restart the board, upload again. |
| `Timed out waiting for packet header` | TX and RX swapped, or GND missing |
| Upload stops halfway | add `upload_speed = 115200` to `platformio.ini` |
| Board keeps restarting (`Brownout detector`) | power supply too weak: power it through the 5V pin |
| No WiFi `WT32-Bridge-Setup` | IO0 still on GND (board waits in flash mode), or password shorter than 8 characters |
| `LAN8720-Treiber konnte nicht gestartet werden` | check the board version (v1.4) and the power supply |
| Compile errors about Ethernet functions | wrong platform: `platformio.ini` must use the pioarduino platform |
| Old build leftovers | delete the `.pio` folder in the project and build again |

The **Serial Monitor** (115200 baud) shows what the bridge is doing. Its output also helps when
you ask for help. The log messages are in German.

---

## Appendix: flash a ready-made firmware in the browser (no compiler)

If the project's **Releases** section offers ready-made files (`bootloader.bin`,
`partitions.bin`, `boot_app0.bin`, `firmware.bin`), you can flash without PlatformIO:

1. Open <https://espressif.github.io/esptool-js/> in **Chrome** or **Edge** (Firefox and Safari
   don't support the required serial access).
2. Put the board into flash mode as described in section 4, click **Connect** and choose the port.
3. Add the files with these addresses:

   | Address | File |
   |---------|------|
   | `0x1000` | `bootloader.bin` |
   | `0x8000` | `partitions.bin` |
   | `0xe000` | `boot_app0.bin` |
   | `0x10000` | `firmware.bin` |

   Easier: add only the file `wt32-eth01-bridge-v…-full.bin` at address `0x0`; it contains all
   four parts.

4. Click **Program**, then disconnect IO0 from GND and restart.

Updating from an older version keeps all settings (as long as you do not choose "Erase Flash" in the web flasher).

If you build it yourself, the files are in `.pio/build/wt32-eth01/` after `pio run`
(`boot_app0.bin` is in the PlatformIO package `framework-arduinoespressif32` under
`tools/partitions/`).
