#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <Preferences.h>
#include <esp_event.h>
#include <esp_netif.h>
#include <esp_wifi.h>
#include <esp_private/wifi.h>
#include <esp_eth.h>
#include <esp_eth_netif_glue.h>
#include <dhcpserver/dhcpserver.h>

// Zwei Betriebsarten (Umschaltung im Webinterface, danach Neustart):
//  NAT:    Ethernet-LAN ist ein eigenes Netz 192.168.50.0/24 mit DHCP-Server; WLAN ist der Upstream.
//  Bridge: Ethernet-Frames werden 1:1 ins WLAN weitergereicht. Das LAN-Geraet holt sich seine
//          IP direkt vom Router. Da ein WLAN-Client nur mit seiner eigenen MAC senden darf,
//          werden die MAC-Adressen umgeschrieben (Verfahren wie Espressifs Beispiel "sta2eth").
//          Nur EIN Geraet am LAN-Port, nur IPv4.

enum BridgeMode : uint8_t { MODE_NAT = 0, MODE_BRIDGE = 1 };

String htmlEscape(const String &text);
String jsonEscape(const String &text);
int wifiPercent();
String signalClass(int percent);
String encryptionName(wifi_auth_mode_t mode);
String pageHeader(const String &title);
String pageFooter();
void detectLanguage();
void setLanguage();
String languageSwitchHtml();
inline const char *T(const char *de, const char *en);
String signalMeterHtml();
String macToString(const uint8_t *mac);
String lanStatusJson();
void showStatus();
void showNetworks();
void showHome();
void connectToRouter();
void resumeRouterConnection();
void saveSettings();
void saveMode();
void saveApPassword();
bool isValidWifiPassword(const String &password);
void onEthernetEvent(void *argument, esp_event_base_t eventBase, int32_t eventId, void *eventData);
void onIpEvent(void *argument, esp_event_base_t eventBase, int32_t eventId, void *eventData);
void onWifiDriverEvent(void *argument, esp_event_base_t eventBase, int32_t eventId, void *eventData);
void onNetworkEvent(WiFiEvent_t event);
bool installEthernetDriver();
void startNatLan();
void startBridgeLan();
void startLanServices();

constexpr int ETH_PHY_POWER_PIN = 16;
constexpr int ETH_PHY_ADDRESS = 1;
constexpr char FIRMWARE_VERSION[] = "2.4";
constexpr char SETUP_AP_SSID[] = "WT32-Bridge-Setup";
// Standard-Passwort des Einrichtungs-WLANs. Leer = offenes WLAN beim ersten Start; das Webinterface
// fordert dann auffaellig dazu auf, ein Passwort festzulegen.
constexpr char SETUP_AP_PASSWORD[] = "";

WebServer webServer(80);
Preferences preferences;
BridgeMode bridgeMode = MODE_NAT;
esp_netif_t *ethernetNetif = nullptr;
esp_eth_handle_t ethernetHandle = nullptr;
String routerSsid;
String routerPassword;
String setupApPassword;  // aktuelles Passwort des Einrichtungs-WLANs (NVS, sonst Standard; leer = offen)
volatile bool ethernetLinkUp = false;
volatile bool ethernetLanReady = false;
volatile bool wifiConnected = false;
volatile bool ethernetServicesPending = false;
uint32_t restartAtMs = 0;
bool scanPausedConnect = false;  // Verbindungsversuche waehrend eines WLAN-Scans angehalten
uint32_t scanStartedMs = 0;

// Informationen ueber die Geraete am LAN-Port
struct LanClient {
  bool used;
  uint8_t mac[6];
  uint32_t ip;          // Netzwerk-Byte-Reihenfolge wie esp_ip4_addr_t (0 = unbekannt)
  uint32_t assignedMs;  // millis() bei Vergabe bzw. Erkennung
};
constexpr int MAX_LAN_CLIENTS = 4;
LanClient lanClients[MAX_LAN_CLIENTS] = {};
portMUX_TYPE lanClientsLock = portMUX_INITIALIZER_UNLOCKED;
volatile int ethernetSpeedMbit = 0;
volatile bool ethernetFullDuplex = false;
volatile uint32_t ethernetLinkSinceMs = 0;

// Zustand des Bridge-Modus (wird aus den Empfangs-Tasks von WLAN und Ethernet benutzt)
uint8_t staMac[6] = {};
uint8_t lanDeviceMac[6] = {};
volatile bool lanDeviceKnown = false;
volatile bool bridgeWifiLinked = false;
volatile uint32_t framesToWifi = 0;
volatile uint32_t framesToLan = 0;
volatile uint32_t framesDropped = 0;

// ---------------------------------------------------------------------------
// Hilfsfunktionen
// ---------------------------------------------------------------------------

String htmlEscape(const String &text) {
  String escaped;
  for (size_t i = 0; i < text.length(); ++i) {
    const char c = text[i];
    if (c == '&') escaped += "&amp;";
    else if (c == '<') escaped += "&lt;";
    else if (c == '>') escaped += "&gt;";
    else if (c == '\"') escaped += "&quot;";
    else if (c == '\'') escaped += "&#39;";
    else escaped += c;
  }
  return escaped;
}

String jsonEscape(const String &text) {
  String escaped;
  for (size_t i = 0; i < text.length(); ++i) {
    const char c = text[i];
    if (c == '\\' || c == '\"') escaped += '\\';
    if (c == '\n') escaped += "\\n";
    else if (c == '\r') escaped += "\\r";
    else escaped += c;
  }
  return escaped;
}

String macToString(const uint8_t *mac) {
  char text[18];
  snprintf(text, sizeof(text), "%02X:%02X:%02X:%02X:%02X:%02X", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
  return String(text);
}

int wifiPercent() {
  return wifiConnected ? constrain((WiFi.RSSI() + 90) * 100 / 60, 0, 100) : 0;
}

String signalClass(int percent) {
  if (!wifiConnected) return "off";
  if (percent < 35) return "weak";
  if (percent < 65) return "fair";
  return "good";
}

String encryptionName(wifi_auth_mode_t mode) {
  switch (mode) {
    case WIFI_AUTH_OPEN: return T("Offen", "Open");
    case WIFI_AUTH_WEP: return "WEP";
    case WIFI_AUTH_WPA_PSK: return "WPA";
    case WIFI_AUTH_WPA2_PSK: return "WPA2";
    case WIFI_AUTH_WPA_WPA2_PSK: return "WPA/WPA2";
    case WIFI_AUTH_WPA2_ENTERPRISE: return "WPA2-Enterprise";
    case WIFI_AUTH_WPA3_PSK: return "WPA3";
    case WIFI_AUTH_WPA2_WPA3_PSK: return "WPA2/WPA3";
    default: return T("Unbekannt", "Unknown");
  }
}

// Merkt sich MAC/IP eines LAN-Geraets fuer die Anzeige. Darf aus jedem Task aufgerufen werden.
void rememberLanClient(const uint8_t *mac, uint32_t ip) {
  portENTER_CRITICAL(&lanClientsLock);
  int slot = -1;
  for (int i = 0; i < MAX_LAN_CLIENTS; ++i) {
    if (lanClients[i].used && memcmp(lanClients[i].mac, mac, 6) == 0) { slot = i; break; }
  }
  if (slot < 0) {
    for (int i = 0; i < MAX_LAN_CLIENTS; ++i) {
      if (!lanClients[i].used) { slot = i; break; }
    }
  }
  if (slot < 0) {
    slot = 0;  // Liste voll: aeltesten Eintrag ersetzen
    for (int i = 1; i < MAX_LAN_CLIENTS; ++i) {
      if (lanClients[i].assignedMs < lanClients[slot].assignedMs) slot = i;
    }
  }
  const bool changed = !lanClients[slot].used || (ip != 0 && lanClients[slot].ip != ip);
  lanClients[slot].used = true;
  memcpy(lanClients[slot].mac, mac, 6);
  if (ip != 0) lanClients[slot].ip = ip;
  if (changed) lanClients[slot].assignedMs = millis();
  portEXIT_CRITICAL(&lanClientsLock);
}

void clearLanClients() {
  portENTER_CRITICAL(&lanClientsLock);
  for (LanClient &client : lanClients) client.used = false;
  lanDeviceKnown = false;
  portEXIT_CRITICAL(&lanClientsLock);
}

// ---------------------------------------------------------------------------
// Bridge-Modus: MAC-Adressen umschreiben
// ---------------------------------------------------------------------------

constexpr uint16_t BR_ETHERTYPE_IPV4 = 0x0800;
constexpr uint16_t BR_ETHERTYPE_ARP = 0x0806;
constexpr size_t BR_ETH_HEADER_LEN = 14;
constexpr size_t BR_DHCP_CHADDR_OFFSET = 28;
constexpr size_t BR_DHCP_YIADDR_OFFSET = 16;
constexpr size_t BR_DHCP_OPTIONS_OFFSET = 240;
constexpr uint8_t BR_DHCP_MSG_ACK = 5;

inline uint16_t readBe16(const uint8_t *p) { return static_cast<uint16_t>((p[0] << 8) | p[1]); }

// Sucht eine DHCP-Option. Liefert einen Zeiger auf den Optionskopf (Code, Laenge, Daten) oder nullptr.
uint8_t *findDhcpOption(uint8_t *options, const uint8_t *end, uint8_t code) {
  while (options < end && options[0] != 255) {
    if (options[0] == 0) { ++options; continue; }
    if (options + 2 > end) break;
    uint8_t *next = options + 2 + options[1];
    if (next > end) break;
    if (options[0] == code) return options;
    options = next;
  }
  return nullptr;
}

void updateUdpChecksum(const uint8_t *ipHeader, uint8_t *udp, uint16_t udpLen) {
  udp[6] = 0;
  udp[7] = 0;
  uint32_t sum = 0;
  for (int i = 12; i < 20; i += 2) sum += readBe16(ipHeader + i);  // Quell- und Ziel-IP
  sum += 17 + udpLen;                                             // Protokoll UDP + Laenge
  for (uint16_t i = 0; i + 1 < udpLen; i += 2) sum += readBe16(udp + i);
  if (udpLen & 1) sum += static_cast<uint32_t>(udp[udpLen - 1]) << 8;
  while (sum >> 16) sum = (sum & 0xFFFF) + (sum >> 16);
  uint16_t result = static_cast<uint16_t>(~sum);
  if (result == 0) result = 0xFFFF;
  udp[6] = result >> 8;
  udp[7] = result & 0xFF;
}

// Schreibt die Client-MAC (chaddr und Option 61) in DHCP-Paketen um.
// Viele Router schicken DHCP-Antworten per Unicast an chaddr - das muss die WLAN-MAC der Bridge sein.
void rewriteDhcp(bool fromLan, uint8_t *frame, uint16_t len) {
  uint8_t *ip = frame + BR_ETH_HEADER_LEN;
  if (len < BR_ETH_HEADER_LEN + 20 || (ip[0] >> 4) != 4 || ip[9] != 17) return;
  const size_t ipHeaderLen = (ip[0] & 0x0F) * 4;
  uint8_t *udp = ip + ipHeaderLen;
  if (udp + 8 > frame + len) return;
  const uint16_t srcPort = readBe16(udp);
  const uint16_t dstPort = readBe16(udp + 2);
  const bool isDhcp = fromLan ? (srcPort == 68 && dstPort == 67) : (srcPort == 67 && dstPort == 68);
  if (!isDhcp) return;
  const uint16_t udpLen = readBe16(udp + 4);
  if (udpLen < 8 + BR_DHCP_OPTIONS_OFFSET || udp + udpLen > frame + len) return;

  uint8_t *dhcp = udp + 8;
  const uint8_t *end = udp + udpLen;
  if (readBe16(dhcp + 236) != 0x6382 || readBe16(dhcp + 238) != 0x5363) return;  // Magic Cookie

  const uint8_t *oldMac = fromLan ? lanDeviceMac : staMac;
  const uint8_t *newMac = fromLan ? staMac : lanDeviceMac;
  bool changed = false;
  if (memcmp(dhcp + BR_DHCP_CHADDR_OFFSET, oldMac, 6) == 0) {
    memcpy(dhcp + BR_DHCP_CHADDR_OFFSET, newMac, 6);
    changed = true;
  }
  uint8_t *options = dhcp + BR_DHCP_OPTIONS_OFFSET;
  uint8_t *clientId = findDhcpOption(options, end, 61);
  if (clientId != nullptr && clientId[1] == 7 && clientId[2] == 1 && memcmp(clientId + 3, oldMac, 6) == 0) {
    memcpy(clientId + 3, newMac, 6);
    changed = true;
  }
  if (!fromLan) {
    uint8_t *type = findDhcpOption(options, end, 53);
    if (type != nullptr && type[1] == 1 && type[2] == BR_DHCP_MSG_ACK) {
      uint32_t assigned;
      memcpy(&assigned, dhcp + BR_DHCP_YIADDR_OFFSET, 4);
      if (assigned != 0) {
        rememberLanClient(lanDeviceMac, assigned);
        Serial.printf("Router hat dem LAN-Geraet " IPSTR " zugewiesen\n", IP2STR(reinterpret_cast<esp_ip4_addr_t *>(&assigned)));
      }
    }
  }
  if (changed && (udp[6] != 0 || udp[7] != 0)) updateUdpChecksum(ip, udp, udpLen);
}

// Liefert false, wenn der Frame verworfen werden soll.
bool rewriteFrame(bool fromLan, uint8_t *frame, uint16_t len) {
  if (len < BR_ETH_HEADER_LEN) return false;
  uint8_t *dst = frame;
  uint8_t *src = frame + 6;
  const uint16_t type = readBe16(frame + 12);
  if (type != BR_ETHERTYPE_IPV4 && type != BR_ETHERTYPE_ARP) return false;  // nur IPv4 und ARP

  if (fromLan) {
    if (src[0] & 0x01) return false;  // ungueltige Quelladresse
    if (!lanDeviceKnown) {
      portENTER_CRITICAL(&lanClientsLock);
      memcpy(lanDeviceMac, src, 6);
      lanDeviceKnown = true;
      portEXIT_CRITICAL(&lanClientsLock);
      rememberLanClient(src, 0);
    } else if (memcmp(src, lanDeviceMac, 6) != 0) {
      return false;  // zweites Geraet (z. B. hinter einem Switch) - im Bridge-Modus nicht moeglich
    }
  } else if (!lanDeviceKnown) {
    return true;  // Geraet noch unbekannt: Broadcasts trotzdem durchreichen
  }

  if (type == BR_ETHERTYPE_ARP && len >= BR_ETH_HEADER_LEN + 28) {
    uint8_t *arp = frame + BR_ETH_HEADER_LEN;
    uint8_t *senderMac = arp + 8;
    uint8_t *targetMac = arp + 18;
    if (fromLan) {
      uint32_t senderIp;
      memcpy(&senderIp, arp + 14, 4);
      if (senderIp != 0) rememberLanClient(lanDeviceMac, senderIp);  // erkennt auch feste IPs
      if (memcmp(senderMac, lanDeviceMac, 6) == 0) memcpy(senderMac, staMac, 6);
    } else if (memcmp(targetMac, staMac, 6) == 0) {
      memcpy(targetMac, lanDeviceMac, 6);
    }
  } else if (type == BR_ETHERTYPE_IPV4) {
    rewriteDhcp(fromLan, frame, len);
  }

  if (fromLan) {
    memcpy(src, staMac, 6);
  } else if (memcmp(dst, staMac, 6) == 0) {
    memcpy(dst, lanDeviceMac, 6);
  }
  return true;
}

// Ethernet -> WLAN (laeuft im Empfangs-Task des Ethernet-Treibers)
esp_err_t onLanFrame(esp_eth_handle_t handle, uint8_t *buffer, uint32_t len, void *priv) {
  if (bridgeWifiLinked && len <= 1600 && rewriteFrame(true, buffer, static_cast<uint16_t>(len))) {
    if (esp_wifi_internal_tx(WIFI_IF_STA, buffer, static_cast<uint16_t>(len)) == ESP_OK) framesToWifi = framesToWifi + 1;
    else framesDropped = framesDropped + 1;
  } else {
    framesDropped = framesDropped + 1;
  }
  free(buffer);
  return ESP_OK;
}

// WLAN -> Ethernet (laeuft im WLAN-Task)
esp_err_t onWifiFrame(void *buffer, uint16_t len, void *eb) {
  if (ethernetLinkUp && rewriteFrame(false, static_cast<uint8_t *>(buffer), len)) {
    if (esp_eth_transmit(ethernetHandle, buffer, len) == ESP_OK) framesToLan = framesToLan + 1;
    else framesDropped = framesDropped + 1;
  }
  esp_wifi_internal_free_rx_buffer(eb);
  return ESP_OK;
}

// ---------------------------------------------------------------------------
// Sprache des Webinterface (Deutsch/Englisch)
// ---------------------------------------------------------------------------

bool uiEnglish = false;  // wird zu Beginn jeder Anfrage per detectLanguage() gesetzt

// Waehlt den Text in der aktuellen Sprache
inline const char *T(const char *de, const char *en) { return uiEnglish ? en : de; }

// Sprache aus Cookie "lang" (vom Umschalter gesetzt), sonst aus dem Browser (Accept-Language)
void detectLanguage() {
  const String cookie = webServer.header("Cookie");
  if (cookie.indexOf("lang=en") >= 0) { uiEnglish = true; return; }
  if (cookie.indexOf("lang=de") >= 0) { uiEnglish = false; return; }
  String accept = webServer.header("Accept-Language");
  accept.toLowerCase();
  uiEnglish = !accept.startsWith("de");
}

// /lang?l=de oder /lang?l=en: Sprache fuer ein Jahr im Browser merken und zur Startseite
void setLanguage() {
  const bool english = webServer.arg("l") == "en";
  webServer.sendHeader("Set-Cookie", english ? "lang=en; Path=/; Max-Age=31536000" : "lang=de; Path=/; Max-Age=31536000");
  webServer.sendHeader("Location", "/");
  webServer.send(302, "text/plain", "");
}

String languageSwitchHtml() {
  const char *flagDe = "<svg viewBox='0 0 5 3' aria-hidden='true'><rect width='5' height='1' fill='#000'/><rect y='1' width='5' height='1' fill='#dd0000'/><rect y='2' width='5' height='1' fill='#ffce00'/></svg>";
  const char *flagEn = "<svg viewBox='0 0 60 30' aria-hidden='true'><clipPath id='uk1'><path d='M0 0v30h60V0z'/></clipPath><clipPath id='uk2'><path d='M30 15h30v15zv15H0zH0V0zV0h30z'/></clipPath>"
    "<g clip-path='url(#uk1)'><path d='M0 0v30h60V0z' fill='#012169'/><path d='M0 0l60 30m0-30L0 30' stroke='#fff' stroke-width='6'/>"
    "<path d='M0 0l60 30m0-30L0 30' clip-path='url(#uk2)' stroke='#c8102e' stroke-width='4'/><path d='M30 0v30M0 15h60' stroke='#fff' stroke-width='10'/>"
    "<path d='M30 0v30M0 15h60' stroke='#c8102e' stroke-width='6'/></g></svg>";
  return String("<nav class='lang' aria-label='Sprache / Language'>") +
    "<a href='/lang?l=de' hreflang='de' class='" + (uiEnglish ? "" : "on") + "' title='Deutsch'>" + flagDe + "DE</a>" +
    "<a href='/lang?l=en' hreflang='en' class='" + (uiEnglish ? "on" : "") + "' title='English'>" + flagEn + "EN</a></nav>";
}

// ---------------------------------------------------------------------------
// Webinterface
// ---------------------------------------------------------------------------

String pageHeader(const String &title) {
  return String("<!doctype html><html lang='") + (uiEnglish ? "en" : "de") + "'><head><meta name='viewport' content='width=device-width,initial-scale=1'><title>" + title + "</title><style>body{font-family:Arial,sans-serif;max-width:700px;margin:30px auto;padding:0 18px;background:#f2f6fa;color:#17212b}.card{background:#fff;border-radius:16px;padding:24px;box-shadow:0 4px 18px #0002}h1{margin-top:0;color:#1263a6}h2{font-size:18px;margin:26px 0 4px}.status{padding:12px 14px;margin:12px 0;border-radius:10px;background:#edf5fd}.ok{color:#08783d}.wait{color:#875b00}.bad{color:#a32020}.meter{display:flex;align-items:flex-end;gap:4px;height:38px;margin:10px 0 3px}.bar{width:13px;border-radius:3px 3px 0 0;background:#d3dae1}.bar.on.good{background:#1a9b59}.bar.on.fair{background:#dd9a17}.bar.on.weak{background:#ce3e3e}label{display:block;font-weight:bold;margin-top:16px}input{box-sizing:border-box;width:100%;padding:12px;margin-top:6px;border:1px solid #aac;border-radius:8px;font-size:16px}label.mode{display:flex;gap:14px;align-items:flex-start;font-weight:normal;margin-top:12px;padding:14px;border:2px solid #cbd8e3;border-radius:12px;cursor:pointer;background:#fff}label.mode:has(input:checked){border-color:#1263a6;background:#f3f8fd}label.mode input{width:auto;margin:4px 0 0}.mode svg{flex:none;width:46px;height:46px;color:#1263a6}.mode b{display:block;font-size:17px;margin-bottom:4px}.mode p{margin:6px 0 0;color:#4b5865;font-size:14px;line-height:1.4}.badge{display:inline-block;margin-top:8px;padding:3px 10px;border-radius:99px;font-size:13px;font-weight:bold}.badge.slow{background:#fdf1dc;color:#875b00}.badge.fast{background:#e3f4ea;color:#08783d}.alert{display:flex;gap:14px;align-items:flex-start;background:#c62828;color:#fff;padding:16px 18px;border-radius:12px;margin:0 0 18px;line-height:1.45;box-shadow:0 0 0 4px #f8d4d4;animation:pulse 2s ease-in-out infinite}.alert svg{flex:none;width:34px;height:34px}.alert a{display:inline-block;margin-top:8px;color:#fff;font-weight:bold;text-decoration:underline}@keyframes pulse{50%{box-shadow:0 0 0 8px #f8d4d4}}.apbox.open{border:2px solid #c62828;background:#fdecec;border-radius:12px;padding:4px 16px 16px}.lang{display:flex;justify-content:flex-end;gap:6px;margin:-8px -8px 8px 0}.lang a{display:flex;align-items:center;gap:6px;padding:5px 9px;border:1px solid #cbd8e3;border-radius:8px;text-decoration:none;color:#4b5865;font-size:13px;font-weight:bold}.lang a.on{border-color:#1263a6;background:#eaf3fc;color:#1263a6}.lang svg{width:24px;height:15px;border-radius:2px;box-shadow:0 0 0 1px #0003}button{margin-top:22px;background:#1263a6;color:#fff;border:0;border-radius:8px;padding:12px 18px;font-size:16px;cursor:pointer}.secondary{margin-top:12px;background:#587080}.network{display:block;width:100%;text-align:left;margin-top:8px;padding:11px;border:1px solid #cbd8e3;border-radius:8px;background:#f8fbfe;color:#17212b}.network b{display:block}.network small,small{color:#4b5865}table.info{width:100%;border-collapse:collapse;margin-top:8px}table.info td{padding:5px 4px;border-top:1px solid #d6e2ee;vertical-align:top}table.info td:first-child{color:#4b5865;width:45%}</style></head><body><div class='card'>";
}

String pageFooter() { return "</div></body></html>"; }

String signalMeterHtml() {
  return String("<div class='status'><b>") + T("WLAN-Empfang", "WiFi signal") + "</b><div class='meter' id='meter'><i class='bar' style='height:20%'></i><i class='bar' style='height:40%'></i><i class='bar' style='height:65%'></i><i class='bar' style='height:100%'></i></div><span id='signalText'>" + T("Wird geladen...", "Loading...") + "</span></div>";
}

String lanStatusJson() {
  const uint32_t now = millis();
  String json = "{\"speed\":" + String(ethernetLinkUp ? ethernetSpeedMbit : 0);
  json += ",\"fullDuplex\":" + String(ethernetFullDuplex ? "true" : "false");
  json += ",\"linkSeconds\":" + String(ethernetLinkUp ? (now - ethernetLinkSinceMs) / 1000 : 0);

  uint8_t bridgeMac[6] = {};
  if (ethernetHandle != nullptr && esp_eth_ioctl(ethernetHandle, ETH_CMD_G_MAC_ADDR, bridgeMac) == ESP_OK) {
    json += ",\"bridgeMac\":\"" + macToString(bridgeMac) + "\"";
  }
  if (bridgeMode == MODE_BRIDGE) {
    json += ",\"staMac\":\"" + macToString(staMac) + "\"";
    json += ",\"toWifi\":" + String(framesToWifi) + ",\"toLan\":" + String(framesToLan) + ",\"dropped\":" + String(framesDropped);
  }
  uint32_t leaseMinutes = 0;
  if (ethernetNetif != nullptr && esp_netif_dhcps_option(ethernetNetif, ESP_NETIF_OP_GET, ESP_NETIF_IP_ADDRESS_LEASE_TIME, &leaseMinutes, sizeof(leaseMinutes)) == ESP_OK) {
    json += ",\"leaseMinutes\":" + String(leaseMinutes);
  }

  LanClient copy[MAX_LAN_CLIENTS];
  portENTER_CRITICAL(&lanClientsLock);
  memcpy(copy, lanClients, sizeof(copy));
  portEXIT_CRITICAL(&lanClientsLock);

  json += ",\"clients\":[";
  bool first = true;
  for (const LanClient &client : copy) {
    if (!client.used) continue;
    if (!first) json += ',';
    first = false;
    json += "{\"ip\":\"" + (client.ip ? IPAddress(client.ip).toString() : String("")) + "\",\"mac\":\"" + macToString(client.mac) + "\",\"seconds\":" + String((now - client.assignedMs) / 1000) + "}";
  }
  json += "]}";
  return json;
}

void showStatus() {
  const bool hasRouterIp = bridgeMode == MODE_NAT && wifiConnected;
  const String json = "{\"version\":\"" + String(FIRMWARE_VERSION) + "\",\"mode\":\"" + String(bridgeMode == MODE_BRIDGE ? "bridge" : "nat") + "\",\"wifi\":" + String(wifiConnected ? "true" : "false") + ",\"ssid\":\"" + jsonEscape(WiFi.SSID()) + "\",\"ip\":\"" + (hasRouterIp ? WiFi.localIP().toString() : String("")) + "\",\"rssi\":" + String(wifiConnected ? WiFi.RSSI() : 0) + ",\"percent\":" + String(wifiPercent()) + ",\"quality\":\"" + signalClass(wifiPercent()) + "\",\"ethLink\":" + String(ethernetLinkUp ? "true" : "false") + ",\"dhcp\":" + String(ethernetLanReady ? "true" : "false") + ",\"apOpen\":" + String(setupApPassword.isEmpty() ? "true" : "false") + ",\"lan\":" + lanStatusJson() + "}";
  webServer.send(200, "application/json", json);
}

// Setzt unterbrochene Verbindungsversuche zum Router nach einem WLAN-Scan fort.
void resumeRouterConnection() {
  if (!scanPausedConnect) return;
  scanPausedConnect = false;
  WiFi.setAutoReconnect(true);
  connectToRouter();
}

// Asynchroner WLAN-Scan: /networks?start=1 startet, danach fragt die Seite /networks ab,
// bis das Ergebnis da ist. So blockiert der Scan den Webserver nicht.
void showNetworks() {
  detectLanguage();  // fuer die Namen der Verschluesselung
  int state = WiFi.scanComplete();
  if (webServer.hasArg("start") && state != WIFI_SCAN_RUNNING) {
    WiFi.scanDelete();
    if (!wifiConnected && !routerSsid.isEmpty()) {
      // Solange der ESP32 versucht, sich mit dem Router zu verbinden, lehnt der WLAN-Treiber
      // einen Scan ab ("STA is connecting"). Daher die Versuche fuer die Dauer des Scans anhalten.
      WiFi.setAutoReconnect(false);
      WiFi.disconnect(false, false);
      scanPausedConnect = true;
      delay(200);
    }
    scanStartedMs = millis();
    state = WiFi.scanNetworks(true, true);
    if (state == WIFI_SCAN_FAILED) {
      delay(500);
      state = WiFi.scanNetworks(true, true);
    }
    if (state == WIFI_SCAN_FAILED) {
      Serial.println("WLAN-Scan konnte nicht gestartet werden");
      resumeRouterConnection();
      webServer.send(200, "application/json", "{\"state\":\"failed\"}");
      return;
    }
    webServer.send(200, "application/json", "{\"state\":\"running\"}");
    return;
  }

  if (state == WIFI_SCAN_RUNNING) {
    if (millis() - scanStartedMs < 15000) {
      webServer.send(200, "application/json", "{\"state\":\"running\"}");
      return;
    }
    WiFi.scanDelete();
    state = WIFI_SCAN_FAILED;
  }
  if (state < 0) {
    resumeRouterConnection();
    webServer.send(200, "application/json", "{\"state\":\"failed\"}");
    return;
  }

  String json = "{\"state\":\"done\",\"networks\":[";
  bool first = true;
  for (int i = 0; i < state; ++i) {
    const String ssid = WiFi.SSID(i);
    bool duplicate = false;  // gleiche SSID mehrfach (Mesh/Repeater): nur den staerksten Eintrag zeigen
    for (int j = 0; j < i && !ssid.isEmpty(); ++j) {
      if (WiFi.SSID(j) == ssid && WiFi.RSSI(j) >= WiFi.RSSI(i)) { duplicate = true; break; }
    }
    if (duplicate) continue;
    if (!first) json += ',';
    first = false;
    const int rssi = WiFi.RSSI(i);
    const int percent = constrain((rssi + 90) * 100 / 60, 0, 100);
    const wifi_auth_mode_t encryption = WiFi.encryptionType(i);
    json += "{\"ssid\":\"" + jsonEscape(ssid) + "\",\"rssi\":" + String(rssi) + ",\"percent\":" + String(percent) + ",\"encryption\":\"" + jsonEscape(encryptionName(encryption)) + "\",\"secured\":" + String(encryption == WIFI_AUTH_OPEN ? "false" : "true") + "}";
  }
  json += "]}";
  WiFi.scanDelete();
  resumeRouterConnection();
  webServer.send(200, "application/json", json);
}

// Texte fuer das JavaScript der Startseite
const char *const JS_TEXT_DE = "var L={notConnected:'Nicht mit dem Router verbunden',noCable:'Kein Kabel erkannt. Stecke das Kabel am Endger\\u00e4t und an der Bridge fest ein.',"
  "link:'Verbindung',full:'Vollduplex',half:'Halbduplex',since:'Kabel steckt seit',device:'Ger\\u00e4t',notDetected:'noch nicht erkannt (sendet noch nichts)',"
  "noLease:'noch keine per DHCP vergeben',ip:'IP-Adresse',unknown:'noch unbekannt',mac:'MAC-Adresse',seen:'Erkannt vor',assigned:'Adresse vergeben vor',ago:'',"
  "addr:'Adressvergabe',byRouter:'direkt durch den Router',appears:'Ger\\u00e4t erscheint im Router als MAC',toWifi:'Frames LAN \\u2192 WLAN',toLan:'Frames WLAN \\u2192 LAN',"
  "dropped:'Verworfen',gw:'Gateway / DNS',lease:'Lease-Dauer',bridgeMac:'MAC der Bridge (LAN)',searching:'Suche nach WLANs ...',"
  "scanFail:'Die WLAN-Suche ist fehlgeschlagen. Bitte erneut versuchen.',none:'Keine WLANs gefunden.',hidden:'(verstecktes WLAN)',secured:' (gesichert)',"
  "selected:'Ausgew\\u00e4hlt: ',mismatch:'Die beiden Eingaben stimmen nicht \\u00fcberein.',confirmPw:'Passwort \\u00e4ndern? Die Bridge startet danach neu.'};";
const char *const JS_TEXT_EN = "var L={notConnected:'Not connected to the router',noCable:'No cable detected. Plug the cable firmly into the device and the bridge.',"
  "link:'Link',full:'full duplex',half:'half duplex',since:'Cable connected for',device:'Device',notDetected:'not detected yet (not sending anything)',"
  "noLease:'none assigned via DHCP yet',ip:'IP address',unknown:'not known yet',mac:'MAC address',seen:'Detected',assigned:'Address assigned',ago:' ago',"
  "addr:'Address assignment',byRouter:'directly by the router',appears:'Device appears in the router with MAC',toWifi:'Frames LAN \\u2192 WiFi',toLan:'Frames WiFi \\u2192 LAN',"
  "dropped:'Dropped',gw:'Gateway / DNS',lease:'Lease time',bridgeMac:'Bridge MAC (LAN)',searching:'Searching for WiFi networks ...',"
  "scanFail:'The WiFi scan failed. Please try again.',none:'No WiFi networks found.',hidden:'(hidden network)',secured:' (secured)',"
  "selected:'Selected: ',mismatch:'The two entries do not match.',confirmPw:'Change the password? The bridge will restart afterwards.'};";

void showHome() {
  detectLanguage();
  const bool bridge = bridgeMode == MODE_BRIDGE;
  String routerState;
  if (!wifiConnected) {
    routerState = String("<p class='wait'>") + T("Noch nicht mit dem Router verbunden. Speichere die Zugangsdaten unten; die Bridge versucht die Verbindung automatisch.", "Not connected to the router yet. Save the credentials below; the bridge will connect automatically.") + "</p>";
  } else if (bridge) {
    routerState = String("<p class='ok'>") + T("Mit Router verbunden: ", "Connected to router: ") + "<b>" + htmlEscape(WiFi.SSID()) + "</b></p>";
  } else {
    routerState = String("<p class='ok'>") + T("Mit Router verbunden: ", "Connected to router: ") + "<b>" + htmlEscape(WiFi.SSID()) + "</b><br>" + T("Router-IP der Bridge: ", "Bridge IP in the router network: ") + WiFi.localIP().toString() + "</p>";
  }

  String ethernetState;
  if (bridge) {
    ethernetState = ethernetLanReady
      ? String("<p class='ok'>") + T("Bridge-Modus aktiv. Das LAN-Ger&auml;t bekommt seine Adresse direkt vom Router.", "Bridge mode active. The LAN device gets its address directly from the router.") + "</p>"
      : String("<p class='bad'>") + T("Die Ethernet-Bridge ist noch nicht bereit.", "The Ethernet bridge is not ready yet.") + "</p>";
  } else {
    ethernetState = ethernetLanReady
      ? String("<p class='ok'>") + T("NAT-Modus: Ethernet-DHCP ist aktiv. Das angeschlossene Ger&auml;t bekommt automatisch eine Adresse im Netz <b>192.168.50.x</b>; Gateway ist <b>192.168.50.1</b>.", "NAT mode: Ethernet DHCP is active. The connected device automatically gets an address in the <b>192.168.50.x</b> network; the gateway is <b>192.168.50.1</b>.") + "</p>"
      : String("<p class='bad'>") + T("Der Ethernet-DHCP-Dienst ist noch nicht bereit.", "The Ethernet DHCP service is not ready yet.") + "</p>";
  }
  const String linkState = String("<div class='status'><b>") + T("Ger&auml;t am LAN-Port", "Device on the LAN port") + "</b><div id='lanInfo'>" + T("Wird geladen...", "Loading...") + "</div></div>";

  const String script = String("<script>") + (uiEnglish ? JS_TEXT_EN : JS_TEXT_DE) +
    "function status(){fetch('/status').then(r=>r.json()).then(s=>{let bars=document.querySelectorAll('#meter .bar'),n=s.wifi?Math.ceil(s.percent/25):0;bars.forEach((b,i)=>b.className='bar '+(i<n?'on '+s.quality:''));document.getElementById('signalText').textContent=s.wifi?s.rssi+' dBm - '+s.percent+' %':L.notConnected;showLan(s);});}"
    "function dur(t){let h=Math.floor(t/3600),m=Math.floor(t%3600/60),x=t%60;return (h?h+' h ':'')+(h||m?m+' min ':'')+x+' s';}"
    "function row(k,v){return '<tr><td>'+k+'</td><td><b>'+v+'</b></td></tr>';}"
    "function showLan(s){let l=s.lan,br=s.mode=='bridge',box=document.getElementById('lanInfo'),h='';"
    "if(!s.ethLink){box.innerHTML='<p class=\\'wait\\'>'+L.noCable+'</p>';return;}"
    "h+=row(L.link,l.speed+' Mbit/s, '+(l.fullDuplex?L.full:L.half));h+=row(L.since,dur(l.linkSeconds));"
    "if(!l.clients.length){h+=row(br?L.device:L.ip,br?L.notDetected:L.noLease);}"
    "l.clients.forEach((c,i)=>{let p=l.clients.length>1?' ('+(i+1)+')':'';h+=row(L.ip+p,c.ip||L.unknown);h+=row(L.mac+p,c.mac);h+=row((br?L.seen:L.assigned)+p,dur(c.seconds)+L.ago);});"
    "if(br){h+=row(L.addr,L.byRouter);if(l.staMac)h+=row(L.appears,l.staMac);h+=row(L.toWifi,l.toWifi);h+=row(L.toLan,l.toLan);h+=row(L.dropped,l.dropped);}"
    "else{h+=row(L.gw,'192.168.50.1 / 1.1.1.1');if(l.leaseMinutes)h+=row(L.lease,l.leaseMinutes+' min');if(l.bridgeMac)h+=row(L.bridgeMac,l.bridgeMac);}"
    "box.innerHTML='<table class=\\'info\\'>'+h+'</table>';}"
    "function scan(){document.getElementById('networks').textContent=L.searching;poll(true,0);}"
    "function poll(start,errors){fetch('/networks'+(start?'?start=1':'')).then(r=>r.json()).then(s=>{"
    "if(s.state=='running'){setTimeout(()=>poll(false,0),800);return;}"
    "if(s.state!='done'){document.getElementById('networks').textContent=L.scanFail;return;}"
    "showNetworks(s.networks);}).catch(()=>{if(errors<8)setTimeout(()=>poll(false,errors+1),1500);else document.getElementById('networks').textContent=L.scanFail;});}"
    "function showNetworks(list){let box=document.getElementById('networks');box.textContent='';if(!list.length){box.textContent=L.none;return;}"
    "list.forEach(n=>{let b=document.createElement('button');b.type='button';b.className='network';let name=document.createElement('b');name.textContent=n.ssid||L.hidden;"
    "let detail=document.createElement('small');detail.textContent=n.rssi+' dBm - '+n.percent+' % - '+n.encryption+(n.secured?L.secured:'');b.append(name,detail);"
    "b.onclick=()=>{document.querySelector('[name=ssid]').value=n.ssid;box.textContent=L.selected+n.ssid;};box.appendChild(b);});}"
    "status();setInterval(status,2000);</script>";

  // Piktogramme: NAT = ein Knoten verteilt auf mehrere Geraete, Bridge = Bruecke direkt ins Heimnetz
  const char *natIcon = "<svg viewBox='0 0 24 24' fill='none' stroke='currentColor' stroke-width='1.8' stroke-linecap='round' stroke-linejoin='round' aria-hidden='true'>"
    "<rect x='9' y='2' width='6' height='5' rx='1'/><path d='M12 7v4M5 11h14M5 11v4M12 11v4M19 11v4'/>"
    "<rect x='2.5' y='15' width='5' height='5' rx='1'/><rect x='9.5' y='15' width='5' height='5' rx='1'/><rect x='16.5' y='15' width='5' height='5' rx='1'/></svg>";
  const char *bridgeIcon = "<svg viewBox='0 0 24 24' fill='none' stroke='currentColor' stroke-width='1.8' stroke-linecap='round' stroke-linejoin='round' aria-hidden='true'>"
    "<path d='M3 15Q12 3 21 15'/><path d='M2 15h20M7.5 10.5V15M12 9v6M16.5 10.5V15M4 15v5M20 15v5'/></svg>";

  const String modeForm = String("<h2>") + T("Betriebsart", "Operating mode") + "</h2><form method='post' action='/mode'>"
    "<label class='mode'><input type='radio' name='mode' value='nat'" + (bridge ? "" : " checked") + ">" + natIcon +
    "<span><b>" + T("NAT &ndash; eigenes Netzwerk", "NAT &ndash; own network") + "</b><p>" +
    T("Die Bridge baut am LAN-Port ein eigenes Netz (192.168.50.x) auf und vergibt die Adressen selbst. Ideal, wenn mehrere Ger&auml;te &uuml;ber einen Switch angeschlossen werden sollen.",
      "The bridge creates its own network (192.168.50.x) on the LAN port and assigns the addresses itself. Ideal if you want to connect several devices through a switch.") +
    "</p><span class='badge slow'>" + T("Datenrate bis ca. 10 Mbit/s", "Data rate up to approx. 10 Mbit/s") + "</span></span></label>"
    "<label class='mode'><input type='radio' name='mode' value='bridge'" + (bridge ? " checked" : "") + ">" + bridgeIcon +
    "<span><b>" + T("Bridge &ndash; direkt ins Heimnetz", "Bridge &ndash; straight into your home network") + "</b><p>" +
    T("Das angeschlossene Ger&auml;t erh&auml;lt seine IP-Adresse direkt vom Router und ist im Heimnetz wie jedes andere Ger&auml;t erreichbar. F&uuml;r genau ein Ger&auml;t, nur IPv4.",
      "The connected device gets its IP address directly from your router and is reachable in your home network like any other device. For exactly one device, IPv4 only.") +
    "</p><span class='badge fast'>" + T("Datenrate &uuml;ber 30 Mbit/s", "Data rate above 30 Mbit/s") + "</span></span></label>"
    "<button type='submit'>" + T("&Uuml;bernehmen und neu starten", "Apply and restart") + "</button></form>";

  const bool apOpen = setupApPassword.isEmpty();
  const bool defaultApPassword = !apOpen && setupApPassword == SETUP_AP_PASSWORD;
  String apState;
  if (apOpen) apState = String("<p class='bad'>") + T("<b>Kein Passwort gesetzt.</b> Das Einrichtungs-WLAN ist offen. Lege jetzt ein Passwort fest.", "<b>No password set.</b> The setup WiFi is open. Set a password now.") + "</p>";
  else if (defaultApPassword) apState = String("<p class='bad'>") + T("Es ist noch das Standard-Passwort aktiv. Da es &ouml;ffentlich bekannt ist, solltest du es jetzt &auml;ndern.", "The default password is still active. As it is publicly known, you should change it now.") + "</p>";
  else apState = String("<p class='ok'>") + T("Ein eigenes Passwort ist gesetzt.", "A custom password is set.") + "</p>";
  const String apForm = String("<h2 id='ap'>") + T("Einrichtungs-WLAN", "Setup WiFi") + "</h2><div class='" + (apOpen ? "apbox open" : "apbox") + "'><p>Name: <b>" + SETUP_AP_SSID + "</b></p>" + apState +
    "<form method='post' action='/appass' onsubmit=\"if(this.ap_new.value!=this.ap_repeat.value){alert(L.mismatch);return false;}return confirm(L.confirmPw);\">"
    "<label>" + T("Neues Passwort", "New password") + "<input name='ap_new' type='password' minlength='8' maxlength='63' autocomplete='new-password' required></label>"
    "<label>" + T("Neues Passwort wiederholen", "Repeat new password") + "<input name='ap_repeat' type='password' minlength='8' maxlength='63' autocomplete='new-password' required></label>"
    "<p><small>" + T("8 bis 63 Zeichen, keine Umlaute. Nach dem Speichern startet die Bridge neu; danach mit dem neuen Passwort verbinden.", "8 to 63 characters, ASCII only. The bridge restarts after saving; then reconnect with the new password.") + "</small></p>"
    "<button type='submit'>" + (apOpen ? T("Passwort festlegen", "Set password") : T("Passwort &auml;ndern", "Change password")) + "</button></form></div>";

  // Auffaelliger Warnhinweis ganz oben, solange das Einrichtungs-WLAN offen ist
  String apAlert;
  if (apOpen) {
    apAlert = String("<div class='alert'><svg viewBox='0 0 24 24' fill='none' stroke='currentColor' stroke-width='2' stroke-linecap='round' stroke-linejoin='round' aria-hidden='true'><path d='M12 3L2 21h20L12 3z'/><path d='M12 10v5M12 18v.5'/></svg><div><b>") +
      T("Kein WLAN-Passwort gesetzt!", "No WiFi password set!") + "</b><br>" +
      T("Das Einrichtungs-WLAN", "The setup WiFi") + " <b>" + SETUP_AP_SSID + "</b> " +
      T("ist offen. Jeder in Reichweite kann diese Seite &ouml;ffnen und die Einstellungen &auml;ndern.", "is open. Anyone in range can open this page and change the settings.") + "<br><a href='#ap'>" +
      T("Jetzt Passwort festlegen &darr;", "Set a password now &darr;") + "</a></div></div>";
  }

  const String footer = String("<p><small>") + (bridge
    ? T("Im Bridge-Modus reicht die Bridge die Daten direkt zum Router durch. Sie selbst hat im Router-Netz keine eigene Adresse; diese Seite ist nur &uuml;ber das Einrichtungs-WLAN erreichbar.",
        "In bridge mode the bridge passes the data straight to the router. It has no address of its own in the router network; this page is only reachable through the setup WiFi.")
    : T("Das Ethernet-Ger&auml;t bekommt Adresse, Gateway und DNS von der Bridge. Die Bridge &uuml;bersetzt die Verbindung zum Router.",
        "The Ethernet device gets its address, gateway and DNS from the bridge. The bridge translates the connection to the router.")) + "</small></p>";

  const String html = pageHeader(T("WT32 Ethernet-WLAN-Bridge", "WT32 Ethernet WiFi Bridge")) + languageSwitchHtml() +
    "<h1>" + T("Ethernet-WLAN-Bridge", "Ethernet WiFi Bridge") + "</h1>" + apAlert + routerState + signalMeterHtml() + ethernetState + linkState +
    "<p>" + T("Diese Seite bleibt &uuml;ber das Einrichtungs-WLAN erreichbar: ", "This page stays reachable through the setup WiFi: ") + "<b>192.168.4.1</b>.</p>"
    "<h2>" + T("Router-WLAN", "Router WiFi") + "</h2><button class='secondary' type='button' onclick='scan()'>" + T("Verf&uuml;gbare WLANs suchen", "Search for WiFi networks") + "</button><div id='networks'></div>"
    "<form method='post' action='/save'><label>" + T("WLAN-Name des Routers", "Router WiFi name") + "<input name='ssid' maxlength='32' value='" + htmlEscape(routerSsid) + "' required></label>"
    "<label>" + T("WLAN-Passwort des Routers", "Router WiFi password") + "<input name='password' type='password' maxlength='63' placeholder='" + T("Nur &auml;ndern, wenn n&ouml;tig", "Only change if needed") + "'></label>"
    "<button type='submit'>" + T("Speichern und verbinden", "Save and connect") + "</button></form>" +
    modeForm + apForm + footer + "<p><small>" + T("Firmware-Version ", "Firmware version ") + FIRMWARE_VERSION + "</small></p>" + script + pageFooter();
  webServer.send(200, "text/html; charset=utf-8", html);
}

void connectToRouter() {
  if (routerSsid.isEmpty()) return;
  WiFi.disconnect(false, false);
  delay(100);
  WiFi.begin(routerSsid.c_str(), routerPassword.c_str());
}

void saveSettings() {
  detectLanguage();
  routerSsid = webServer.arg("ssid");
  const String newPassword = webServer.arg("password");
  preferences.putString("ssid", routerSsid);
  if (!newPassword.isEmpty()) {
    routerPassword = newPassword;
    preferences.putString("password", routerPassword);
  }
  webServer.send(200, "text/html; charset=utf-8", pageHeader(T("Gespeichert", "Saved")) + "<h1>" + T("Gespeichert", "Saved") + "</h1><p>" +
    T("Die Bridge verbindet sich jetzt mit ", "The bridge is now connecting to ") + "<b>" + htmlEscape(routerSsid) + "</b>. " +
    T("Kehre nach ein paar Sekunden zur <a href='/'>Startseite</a> zur&uuml;ck.", "Return to the <a href='/'>start page</a> after a few seconds.") + "</p>" + pageFooter());
  connectToRouter();
}

void saveMode() {
  detectLanguage();
  const BridgeMode newMode = webServer.arg("mode") == "bridge" ? MODE_BRIDGE : MODE_NAT;
  preferences.putUChar("mode", newMode);
  const String name = newMode == MODE_BRIDGE ? "Bridge" : "NAT";
  webServer.send(200, "text/html; charset=utf-8", pageHeader(T("Neustart", "Restart")) + "<h1>" + T("Neustart", "Restarting") + "</h1><p>" +
    T("Betriebsart ", "Operating mode ") + "<b>" + name + "</b> " +
    T("gespeichert. Die Bridge startet neu. Verbinde dich danach wieder mit dem WLAN ", "saved. The bridge is restarting. Afterwards, reconnect to the WiFi ") + "<b>" + SETUP_AP_SSID + "</b> " +
    T("und &ouml;ffne", "and open") + " <a href='/'>192.168.4.1</a>.</p><p><small>" +
    T("Ziehe am LAN-Ger&auml;t kurz das Kabel ab oder erneuere dort die IP-Adresse, damit es eine Adresse aus dem neuen Netz holt.", "Briefly unplug the cable of the LAN device or renew its IP address so it gets an address from the new network.") +
    "</small></p>" + pageFooter());
  restartAtMs = millis() + 1500;  // Antwort erst noch ausliefern
}

// WPA2-Passphrase: 8 bis 63 druckbare ASCII-Zeichen
bool isValidWifiPassword(const String &password) {
  if (password.length() < 8 || password.length() > 63) return false;
  for (size_t i = 0; i < password.length(); ++i) {
    const char c = password[i];
    if (c < 32 || c > 126) return false;
  }
  return true;
}

void sendApPasswordError(const String &message) {
  webServer.send(400, "text/html; charset=utf-8", pageHeader(T("Fehler", "Error")) + "<h1>" + T("Passwort nicht ge&auml;ndert", "Password not changed") + "</h1><p class='bad'>" + message + "</p><p><a href='/'>" + T("Zur&uuml;ck zur Startseite", "Back to the start page") + "</a></p>" + pageFooter());
}

void saveApPassword() {
  detectLanguage();
  const String newPassword = webServer.arg("ap_new");
  const String repeat = webServer.arg("ap_repeat");
  if (newPassword != repeat) {
    sendApPasswordError(T("Die beiden Eingaben stimmen nicht &uuml;berein.", "The two entries do not match."));
    return;
  }
  if (!isValidWifiPassword(newPassword)) {
    sendApPasswordError(T("Das Passwort muss 8 bis 63 Zeichen lang sein und darf nur Buchstaben, Ziffern, Leerzeichen und die &uuml;blichen Sonderzeichen enthalten (keine Umlaute).",
                          "The password must be 8 to 63 characters long and may only contain letters, digits, spaces and common special characters (ASCII only)."));
    return;
  }
  preferences.putString("ap_pass", newPassword);
  setupApPassword = newPassword;
  Serial.println("Neues Passwort fuer das Einrichtungs-WLAN gespeichert");
  webServer.send(200, "text/html; charset=utf-8", pageHeader(T("Gespeichert", "Saved")) + "<h1>" + T("Passwort ge&auml;ndert", "Password changed") + "</h1><p>" +
    T("Das neue Passwort f&uuml;r das WLAN ", "The new password for the WiFi ") + "<b>" + SETUP_AP_SSID + "</b> " +
    T("ist gespeichert. Die Bridge startet jetzt neu.", "has been saved. The bridge is restarting now.") + "</p><p>" +
    T("Verbinde dich danach <b>mit dem neuen Passwort</b> wieder mit dem WLAN und &ouml;ffne <a href='/'>192.168.4.1</a>. Eventuell musst du das WLAN auf deinem Ger&auml;t vorher &bdquo;vergessen&ldquo;.",
      "Afterwards, reconnect to the WiFi <b>with the new password</b> and open <a href='/'>192.168.4.1</a>. You may have to \"forget\" the WiFi on your device first.") +
    "</p>" + pageFooter());
  restartAtMs = millis() + 1500;
}

// ---------------------------------------------------------------------------
// Ereignisse
// ---------------------------------------------------------------------------

void onEthernetEvent(void *argument, esp_event_base_t eventBase, int32_t eventId, void *eventData) {
  if (eventId == ETHERNET_EVENT_CONNECTED) {
    esp_eth_handle_t handle = *static_cast<esp_eth_handle_t *>(eventData);
    eth_speed_t speed = ETH_SPEED_10M;
    eth_duplex_t duplex = ETH_DUPLEX_HALF;
    esp_eth_ioctl(handle, ETH_CMD_G_SPEED, &speed);
    esp_eth_ioctl(handle, ETH_CMD_G_DUPLEX_MODE, &duplex);
    ethernetSpeedMbit = (speed == ETH_SPEED_100M) ? 100 : 10;
    ethernetFullDuplex = (duplex == ETH_DUPLEX_FULL);
    ethernetLinkSinceMs = millis();
    ethernetLinkUp = true;
    if (bridgeMode == MODE_NAT) ethernetServicesPending = true;
    Serial.printf("Ethernet-Kabel verbunden (%d Mbit/s, %s)\n", ethernetSpeedMbit, ethernetFullDuplex ? "Vollduplex" : "Halbduplex");
  } else if (eventId == ETHERNET_EVENT_DISCONNECTED) {
    ethernetLinkUp = false;
    if (bridgeMode == MODE_NAT) ethernetLanReady = false;
    clearLanClients();  // im Bridge-Modus darf danach ein anderes Geraet angesteckt werden
    Serial.println("Ethernet-Kabel getrennt");
  } else if (eventId == ETHERNET_EVENT_STOP) {
    ethernetLanReady = false;
  }
}

// NAT-Modus: wird bei jeder DHCP-Vergabe ausgeloest (auch fuer das Setup-WLAN, daher der Filter).
void onIpEvent(void *argument, esp_event_base_t eventBase, int32_t eventId, void *eventData) {
  if (eventId != IP_EVENT_AP_STAIPASSIGNED) return;
  const ip_event_ap_staipassigned_t *info = static_cast<const ip_event_ap_staipassigned_t *>(eventData);
  if (ethernetNetif == nullptr || info->esp_netif != ethernetNetif) return;
  rememberLanClient(info->mac, info->ip.addr);
  Serial.printf("LAN-Geraet %s hat " IPSTR " bekommen\n", macToString(info->mac).c_str(), IP2STR(&info->ip));
}

// Bridge-Modus: WLAN-Treiber-Ereignisse. Wird NACH den Standard-Handlern von ESP-IDF aufgerufen.
void onWifiDriverEvent(void *argument, esp_event_base_t eventBase, int32_t eventId, void *eventData) {
  if (eventId == WIFI_EVENT_STA_CONNECTED) {
    // Die Bridge selbst soll im Router-Netz keine Adresse holen - das macht das LAN-Geraet.
    esp_netif_t *staNetif = WiFi.STA.netif();
    if (staNetif != nullptr) esp_netif_dhcpc_stop(staNetif);
    // Alle WLAN-Frames an die Bridge statt an den eigenen TCP/IP-Stack liefern
    esp_wifi_internal_reg_rxcb(WIFI_IF_STA, onWifiFrame);
    bridgeWifiLinked = true;
  } else if (eventId == WIFI_EVENT_STA_DISCONNECTED) {
    bridgeWifiLinked = false;
  }
}

void onNetworkEvent(WiFiEvent_t event) {
  if (event == ARDUINO_EVENT_WIFI_STA_CONNECTED && bridgeMode == MODE_BRIDGE) {
    wifiConnected = true;
    Serial.println("WLAN verbunden (Bridge-Modus)");
  } else if (event == ARDUINO_EVENT_WIFI_STA_GOT_IP && bridgeMode == MODE_NAT) {
    wifiConnected = true;
    Serial.print("Router-IP: ");
    Serial.println(WiFi.localIP());
  } else if (event == ARDUINO_EVENT_WIFI_STA_DISCONNECTED) {
    wifiConnected = false;
  }
}

// ---------------------------------------------------------------------------
// Ethernet
// ---------------------------------------------------------------------------

bool installEthernetDriver() {
  // WT32-ETH01: GPIO16 schaltet den 50-MHz-Oszillator des LAN8720 ein.
  // Der Takt muss laufen, bevor der EMAC initialisiert wird.
  pinMode(ETH_PHY_POWER_PIN, OUTPUT);
  digitalWrite(ETH_PHY_POWER_PIN, HIGH);
  delay(50);

  eth_mac_config_t macConfig = ETH_MAC_DEFAULT_CONFIG();
  eth_phy_config_t phyConfig = ETH_PHY_DEFAULT_CONFIG();
  phyConfig.phy_addr = ETH_PHY_ADDRESS;
  phyConfig.reset_gpio_num = -1;
  esp_eth_phy_t *phy = esp_eth_phy_new_lan87xx(&phyConfig);
  eth_esp32_emac_config_t emacConfig = ETH_ESP32_EMAC_DEFAULT_CONFIG();
  // MDC=23 und MDIO=18 sind bereits die Standardwerte von ETH_ESP32_EMAC_DEFAULT_CONFIG().
  emacConfig.clock_config.rmii.clock_mode = EMAC_CLK_EXT_IN;
  emacConfig.clock_config.rmii.clock_gpio = EMAC_CLK_IN_GPIO;
  esp_eth_mac_t *mac = esp_eth_mac_new_esp32(&emacConfig, &macConfig);
  esp_eth_config_t ethConfig = ETH_DEFAULT_CONFIG(mac, phy);
  ethConfig.check_link_period_ms = 2000;

  if (phy == nullptr || mac == nullptr || esp_eth_driver_install(&ethConfig, &ethernetHandle) != ESP_OK) {
    Serial.println("LAN8720-Treiber konnte nicht gestartet werden");
    return false;
  }
  esp_event_handler_register(ETH_EVENT, ESP_EVENT_ANY_ID, &onEthernetEvent, nullptr);
  return true;
}

void startNatLan() {
  esp_netif_ip_info_t ipInfo{};
  ipInfo.ip.addr = ESP_IP4TOADDR(192, 168, 50, 1);
  ipInfo.gw.addr = ESP_IP4TOADDR(192, 168, 50, 1);
  ipInfo.netmask.addr = ESP_IP4TOADDR(255, 255, 255, 0);
  esp_netif_inherent_config_t netifConfig = ESP_NETIF_INHERENT_DEFAULT_ETH();
  // AUTOUP ist noetig: Der DHCP-Server startet nur, wenn die Schnittstelle beim Start "up" ist.
  netifConfig.flags = (esp_netif_flags_t)(ESP_NETIF_DHCP_SERVER | ESP_NETIF_FLAG_AUTOUP);
  netifConfig.ip_info = &ipInfo;
  netifConfig.if_key = "ETH_LAN";
  netifConfig.if_desc = "ethernet-lan";
  esp_netif_config_t config{};
  config.base = &netifConfig;
  config.stack = ESP_NETIF_NETSTACK_DEFAULT_ETH;
  ethernetNetif = esp_netif_new(&config);
  if (ethernetNetif == nullptr) {
    Serial.println("Ethernet-LAN-Schnittstelle konnte nicht angelegt werden");
    return;
  }
  if (esp_netif_attach(ethernetNetif, esp_eth_new_netif_glue(ethernetHandle)) != ESP_OK) {
    Serial.println("Ethernet-LAN konnte nicht mit dem Netzwerk verbunden werden");
    return;
  }

  // DNS fuer die LAN-Clients: erst das Angebot aktivieren, dann die Adresse setzen.
  esp_netif_dns_info_t dns{};
  dns.ip.u_addr.ip4.addr = ESP_IP4TOADDR(1, 1, 1, 1);
  dns.ip.type = ESP_IPADDR_TYPE_V4;
  dhcps_offer_t offerDns = OFFER_DNS;
  esp_netif_dhcps_option(ethernetNetif, ESP_NETIF_OP_SET, ESP_NETIF_DOMAIN_NAME_SERVER, &offerDns, sizeof(offerDns));
  esp_netif_set_dns_info(ethernetNetif, ESP_NETIF_DNS_MAIN, &dns);
  esp_event_handler_register(IP_EVENT, IP_EVENT_AP_STAIPASSIGNED, &onIpEvent, nullptr);
  if (esp_eth_start(ethernetHandle) != ESP_OK) {
    Serial.println("Ethernet-LAN konnte nicht eingeschaltet werden");
    return;
  }
  Serial.println("NAT-Modus: Ethernet-LAN gestartet, warte auf Kabel ...");
  startLanServices();
}

// NAT-Modus: startet DHCP-Server und NAPT. Wird beim Start und bei jedem Link-Up aufgerufen.
void startLanServices() {
  ethernetServicesPending = false;
  if (ethernetNetif == nullptr) return;

  esp_err_t result = esp_netif_dhcps_start(ethernetNetif);
  if (result != ESP_OK && result != ESP_ERR_ESP_NETIF_DHCP_ALREADY_STARTED) {
    Serial.printf("DHCP-Server konnte nicht gestartet werden: 0x%x\n", static_cast<unsigned>(result));
    ethernetLanReady = false;
    return;
  }
  esp_netif_dhcp_status_t status;
  if (esp_netif_dhcps_get_status(ethernetNetif, &status) == ESP_OK && status == ESP_NETIF_DHCP_STARTED) {
    if (!ethernetLanReady) Serial.println("Ethernet-LAN: 192.168.50.1, DHCP-Server laeuft");
    ethernetLanReady = true;
  }

  result = esp_netif_napt_enable(ethernetNetif);
  if (result != ESP_OK) Serial.printf("Internetfreigabe konnte nicht aktiviert werden: 0x%x\n", static_cast<unsigned>(result));
}

void startBridgeLan() {
  // Kein TCP/IP-Stack am Ethernet: alle Frames gehen direkt an onLanFrame().
  if (esp_eth_update_input_path(ethernetHandle, onLanFrame, nullptr) != ESP_OK) {
    Serial.println("Bridge: Ethernet-Empfang konnte nicht umgeleitet werden");
    return;
  }
  // Promiscuous: auch Frames an die MAC des Routers annehmen (nicht nur an die eigene).
  bool promiscuous = true;
  if (esp_eth_ioctl(ethernetHandle, ETH_CMD_S_PROMISCUOUS, &promiscuous) != ESP_OK) {
    Serial.println("Bridge: Promiscuous-Modus konnte nicht aktiviert werden");
    return;
  }
  if (esp_eth_start(ethernetHandle) != ESP_OK) {
    Serial.println("Bridge: Ethernet konnte nicht eingeschaltet werden");
    return;
  }
  ethernetLanReady = true;
  Serial.printf("Bridge-Modus: Ethernet <-> WLAN, Geraet erscheint im Router als %s\n", macToString(staMac).c_str());
}

// ---------------------------------------------------------------------------

void setup() {
  Serial.begin(115200);
  delay(250);
  WiFi.onEvent(onNetworkEvent);
  preferences.begin("bridge", false);
  routerSsid = preferences.getString("ssid", "");
  routerPassword = preferences.getString("password", "");
  setupApPassword = preferences.getString("ap_pass", SETUP_AP_PASSWORD);
  if (!isValidWifiPassword(setupApPassword)) setupApPassword = isValidWifiPassword(SETUP_AP_PASSWORD) ? SETUP_AP_PASSWORD : "";
  bridgeMode = preferences.getUChar("mode", MODE_NAT) == MODE_BRIDGE ? MODE_BRIDGE : MODE_NAT;
  Serial.printf("WT32-ETH01 Ethernet-WLAN-Bridge, Firmware %s\n", FIRMWARE_VERSION);
  Serial.printf("Betriebsart: %s\n", bridgeMode == MODE_BRIDGE ? "Bridge" : "NAT");

  WiFi.mode(WIFI_MODE_APSTA);
  WiFi.setSleep(false);  // kein WLAN-Energiesparen: geringere Latenz, stabilere Bridge
  WiFi.softAPConfig(IPAddress(192, 168, 4, 1), IPAddress(192, 168, 4, 1), IPAddress(255, 255, 255, 0));
  if (setupApPassword.isEmpty()) {
    WiFi.softAP(SETUP_AP_SSID);  // offenes WLAN bis ein Passwort festgelegt ist
    Serial.println("WARNUNG: Einrichtungs-WLAN ist OFFEN (kein Passwort). Bitte im Webinterface ein Passwort festlegen.");
  } else {
    WiFi.softAP(SETUP_AP_SSID, setupApPassword.c_str());
    if (setupApPassword == SETUP_AP_PASSWORD) Serial.println("Hinweis: Einrichtungs-WLAN nutzt noch das Standard-Passwort");
  }
  esp_wifi_get_mac(WIFI_IF_STA, staMac);
  if (bridgeMode == MODE_BRIDGE) {
    // Nach WiFi.mode() registrieren, damit dieser Handler nach denen von ESP-IDF/Arduino laeuft.
    esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &onWifiDriverEvent, nullptr);
  }

  webServer.on("/", HTTP_GET, showHome);
  webServer.on("/status", HTTP_GET, showStatus);
  webServer.on("/networks", HTTP_GET, showNetworks);
  webServer.on("/save", HTTP_POST, saveSettings);
  webServer.on("/mode", HTTP_POST, saveMode);
  webServer.on("/appass", HTTP_POST, saveApPassword);
  webServer.on("/lang", HTTP_GET, setLanguage);
  static const char *collectedHeaders[] = {"Cookie", "Accept-Language"};
  webServer.collectHeaders(collectedHeaders, 2);
  webServer.onNotFound(showHome);
  webServer.begin();

  if (installEthernetDriver()) {
    if (bridgeMode == MODE_BRIDGE) startBridgeLan();
    else startNatLan();
  }
  connectToRouter();
  Serial.println("Einrichtungsseite: http://192.168.4.1");
}

void loop() {
  if (ethernetServicesPending) startLanServices();
  webServer.handleClient();
  // Falls die Seite geschlossen wurde, bevor das Scan-Ergebnis abgeholt war
  if (scanPausedConnect && millis() - scanStartedMs > 20000) {
    WiFi.scanDelete();
    resumeRouterConnection();
  }
  if (restartAtMs != 0 && static_cast<int32_t>(millis() - restartAtMs) >= 0) {
    ESP.restart();
  }
}
