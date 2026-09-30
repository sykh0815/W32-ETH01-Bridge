# Changelog

🇬🇧 **English** | 🇩🇪 [Deutsch](CHANGELOG.de.md)

All notable changes to this project are documented in this file.

Version numbers follow the scheme **MAJOR.MINOR.PATCH**:

- **MAJOR**: incompatible changes (e.g. settings have to be entered again)
- **MINOR**: new features
- **PATCH**: bug fixes only (written as e.g. `2.2.1`; a missing patch number means `.0`)

The current version is defined in `src/main.cpp` (`FIRMWARE_VERSION`) and shown in the web
interface and the serial output.

## [2.3] – 2026-09-30

### Changed
- The setup WiFi now starts **without a password** (open) until a password is set in the web
  interface. `SETUP_AP_PASSWORD` in `src/main.cpp` is empty by default; builders can still enter a
  fixed start password there.
- The button in the "Einrichtungs-WLAN" section reads "Passwort festlegen" (set password) while no
  password is set.

### Added
- Prominent, pulsing red warning at the top of the web interface while the setup WiFi is open,
  with a link to the password form. The form is highlighted in red as well.
- Warning in the serial output when the setup WiFi is open.

## [2.2] – 2026-09-30

### Added
- Setup WiFi password can be changed in the web interface (section "Einrichtungs-WLAN"). It is
  stored in flash and survives firmware updates.
- Warning in the web interface and in the serial output while the default setup WiFi password is
  still active.
- Password rules are checked: 8 to 63 printable ASCII characters, entered twice.

### Changed
- `SETUP_AP_PASSWORD` in `src/main.cpp` is now only the default, used until a custom password is
  set or after erasing the flash.

## [2.1] – 2026-09-30

### Added
- **Bridge** operating mode: the LAN device gets its IP address directly from the router (MAC
  translation, one device, IPv4 only). Selectable in the web interface, in addition to NAT.
- Operating modes shown as cards with pictograms and data rate.
- Info about the device on the LAN port: IP, MAC, link speed/duplex, connection time, packet
  counters in bridge mode.
- Version number in the web interface, the serial output and `/status`.
- Project description and tutorial in English and German, MIT license.

### Fixed
- WiFi scan now works while there is no router connection yet (connection attempts are paused
  during the scan; the scan runs asynchronously).
- DHCP server on the Ethernet port did not start (missing `ESP_NETIF_FLAG_AUTOUP`).
- DNS server was not passed to DHCP clients correctly.
- LAN8720 oscillator (GPIO16) is enabled before the EMAC starts.

## [2.0] and earlier

NAT mode only, not documented in detail. The last NAT-only version is kept in
`backup/main_nat_only.cpp`.
