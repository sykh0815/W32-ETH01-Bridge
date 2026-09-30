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
String signalMeterHtml();
String macToString(const uint8_t *mac);
String lanStatusJson();
void showStatus();
void showNetworks();
void showHome();
void connectToRouter();
void saveSettings();
void saveMode();
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
constexpr char SETUP_AP_SSID[] = "WT32-Bridge-Setup";
constexpr char SETUP_AP_PASSWORD[] = "BridgeSetup26";

WebServer webServer(80);
Preferences preferences;
BridgeMode bridgeMode = MODE_NAT;
esp_netif_t *ethernetNetif = nullptr;
esp_eth_handle_t ethernetHandle = nullptr;
String routerSsid;
String routerPassword;
volatile bool ethernetLinkUp = false;
volatile bool ethernetLanReady = false;
volatile bool wifiConnected = false;
volatile bool ethernetServicesPending = false;
uint32_t restartAtMs = 0;

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
    case WIFI_AUTH_OPEN: return "Offen";
    case WIFI_AUTH_WEP: return "WEP";
    case WIFI_AUTH_WPA_PSK: return "WPA";
    case WIFI_AUTH_WPA2_PSK: return "WPA2";
    case WIFI_AUTH_WPA_WPA2_PSK: return "WPA/WPA2";
    case WIFI_AUTH_WPA2_ENTERPRISE: return "WPA2-Enterprise";
    case WIFI_AUTH_WPA3_PSK: return "WPA3";
    case WIFI_AUTH_WPA2_WPA3_PSK: return "WPA2/WPA3";
    default: return "Unbekannt";
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
// Webinterface
// ---------------------------------------------------------------------------

String pageHeader(const String &title) {
  return "<!doctype html><html lang='de'><head><meta name='viewport' content='width=device-width,initial-scale=1'><title>" + title + "</title><style>body{font-family:Arial,sans-serif;max-width:700px;margin:30px auto;padding:0 18px;background:#f2f6fa;color:#17212b}.card{background:#fff;border-radius:16px;padding:24px;box-shadow:0 4px 18px #0002}h1{margin-top:0;color:#1263a6}h2{font-size:18px;margin:26px 0 4px}.status{padding:12px 14px;margin:12px 0;border-radius:10px;background:#edf5fd}.ok{color:#08783d}.wait{color:#875b00}.bad{color:#a32020}.meter{display:flex;align-items:flex-end;gap:4px;height:38px;margin:10px 0 3px}.bar{width:13px;border-radius:3px 3px 0 0;background:#d3dae1}.bar.on.good{background:#1a9b59}.bar.on.fair{background:#dd9a17}.bar.on.weak{background:#ce3e3e}label{display:block;font-weight:bold;margin-top:16px}input{box-sizing:border-box;width:100%;padding:12px;margin-top:6px;border:1px solid #aac;border-radius:8px;font-size:16px}label.opt{font-weight:normal;margin-top:10px}label.opt input{width:auto;margin:0 8px 0 0}button{margin-top:22px;background:#1263a6;color:#fff;border:0;border-radius:8px;padding:12px 18px;font-size:16px;cursor:pointer}.secondary{margin-top:12px;background:#587080}.network{display:block;width:100%;text-align:left;margin-top:8px;padding:11px;border:1px solid #cbd8e3;border-radius:8px;background:#f8fbfe;color:#17212b}.network b{display:block}.network small,small{color:#4b5865}table.info{width:100%;border-collapse:collapse;margin-top:8px}table.info td{padding:5px 4px;border-top:1px solid #d6e2ee;vertical-align:top}table.info td:first-child{color:#4b5865;width:45%}</style></head><body><div class='card'>";
}

String signalMeterHtml() {
  return "<div class='status'><b>WLAN-Empfang</b><div class='meter' id='meter'><i class='bar' style='height:20%'></i><i class='bar' style='height:40%'></i><i class='bar' style='height:65%'></i><i class='bar' style='height:100%'></i></div><span id='signalText'>Wird geladen...</span></div>";
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
  const String json = "{\"mode\":\"" + String(bridgeMode == MODE_BRIDGE ? "bridge" : "nat") + "\",\"wifi\":" + String(wifiConnected ? "true" : "false") + ",\"ssid\":\"" + jsonEscape(WiFi.SSID()) + "\",\"ip\":\"" + (hasRouterIp ? WiFi.localIP().toString() : String("")) + "\",\"rssi\":" + String(wifiConnected ? WiFi.RSSI() : 0) + ",\"percent\":" + String(wifiPercent()) + ",\"quality\":\"" + signalClass(wifiPercent()) + "\",\"ethLink\":" + String(ethernetLinkUp ? "true" : "false") + ",\"dhcp\":" + String(ethernetLanReady ? "true" : "false") + ",\"lan\":" + lanStatusJson() + "}";
  webServer.send(200, "application/json", json);
}

void showNetworks() {
  const int count = WiFi.scanNetworks(false, true);
  String json = "[";
  for (int i = 0; i < count; ++i) {
    if (i) json += ',';
    const int rssi = WiFi.RSSI(i);
    const int percent = constrain((rssi + 90) * 100 / 60, 0, 100);
    const wifi_auth_mode_t encryption = WiFi.encryptionType(i);
    json += "{\"ssid\":\"" + jsonEscape(WiFi.SSID(i)) + "\",\"rssi\":" + String(rssi) + ",\"percent\":" + String(percent) + ",\"encryption\":\"" + jsonEscape(encryptionName(encryption)) + "\",\"secured\":" + String(encryption == WIFI_AUTH_OPEN ? "false" : "true") + "}";
  }
  json += "]";
  WiFi.scanDelete();
  webServer.send(200, "application/json", json);
}

void showHome() {
  const bool bridge = bridgeMode == MODE_BRIDGE;
  String routerState;
  if (!wifiConnected) {
    routerState = "<p class='wait'>Noch nicht mit dem Router verbunden. Speichere die Zugangsdaten unten; die Bridge versucht die Verbindung automatisch.</p>";
  } else if (bridge) {
    routerState = "<p class='ok'>Mit Router verbunden: <b>" + htmlEscape(WiFi.SSID()) + "</b></p>";
  } else {
    routerState = "<p class='ok'>Mit Router verbunden: <b>" + htmlEscape(WiFi.SSID()) + "</b><br>Router-IP der Bridge: " + WiFi.localIP().toString() + "</p>";
  }

  String ethernetState;
  if (bridge) {
    ethernetState = ethernetLanReady ? "<p class='ok'>Bridge-Modus aktiv. Das LAN-Ger&auml;t bekommt seine Adresse direkt vom Router.</p>" : "<p class='bad'>Die Ethernet-Bridge ist noch nicht bereit.</p>";
  } else {
    ethernetState = ethernetLanReady ? "<p class='ok'>NAT-Modus: Ethernet-DHCP ist aktiv. Das angeschlossene Ger&auml;t bekommt automatisch eine Adresse im Netz <b>192.168.50.x</b>; Gateway ist <b>192.168.50.1</b>.</p>" : "<p class='bad'>Der Ethernet-DHCP-Dienst ist noch nicht bereit.</p>";
  }
  const String linkState = "<div class='status'><b>Ger&auml;t am LAN-Port</b><div id='lanInfo'>Wird geladen...</div></div>";

  const String script = "<script>"
    "function status(){fetch('/status').then(r=>r.json()).then(s=>{let bars=document.querySelectorAll('#meter .bar'),n=s.wifi?Math.ceil(s.percent/25):0;bars.forEach((b,i)=>b.className='bar '+(i<n?'on '+s.quality:''));document.getElementById('signalText').textContent=s.wifi?s.rssi+' dBm - '+s.percent+' %':'Nicht mit dem Router verbunden';showLan(s);});}"
    "function dur(t){let h=Math.floor(t/3600),m=Math.floor(t%3600/60),x=t%60;return (h?h+' h ':'')+(h||m?m+' min ':'')+x+' s';}"
    "function row(k,v){return '<tr><td>'+k+'</td><td><b>'+v+'</b></td></tr>';}"
    "function showLan(s){let l=s.lan,br=s.mode=='bridge',box=document.getElementById('lanInfo'),h='';"
    "if(!s.ethLink){box.innerHTML='<p class=\\'wait\\'>Kein Kabel erkannt. Stecke das Kabel am Endger\\u00e4t und an der Bridge fest ein.</p>';return;}"
    "h+=row('Verbindung',l.speed+' Mbit/s, '+(l.fullDuplex?'Vollduplex':'Halbduplex'));h+=row('Kabel steckt seit',dur(l.linkSeconds));"
    "if(!l.clients.length){h+=row(br?'Ger\\u00e4t':'IP-Adresse',br?'noch nicht erkannt (sendet noch nichts)':'noch keine per DHCP vergeben');}"
    "l.clients.forEach((c,i)=>{let p=l.clients.length>1?' ('+(i+1)+')':'';h+=row('IP-Adresse'+p,c.ip||'noch unbekannt');h+=row('MAC-Adresse'+p,c.mac);h+=row((br?'Erkannt vor':'Adresse vergeben vor')+p,dur(c.seconds));});"
    "if(br){h+=row('Adressvergabe','direkt durch den Router');if(l.staMac)h+=row('Ger\\u00e4t erscheint im Router als MAC',l.staMac);h+=row('Frames LAN &rarr; WLAN',l.toWifi);h+=row('Frames WLAN &rarr; LAN',l.toLan);h+=row('Verworfen',l.dropped);}"
    "else{h+=row('Gateway / DNS','192.168.50.1 / 1.1.1.1');if(l.leaseMinutes)h+=row('Lease-Dauer',l.leaseMinutes+' min');if(l.bridgeMac)h+=row('MAC der Bridge (LAN)',l.bridgeMac);}"
    "box.innerHTML='<table class=\\'info\\'>'+h+'</table>';}"
    "function scan(){let box=document.getElementById('networks');box.textContent='Suche nach WLANs ...';fetch('/networks').then(r=>r.json()).then(list=>{box.textContent='';if(!list.length){box.textContent='Keine WLANs gefunden.';return;}list.forEach(n=>{let b=document.createElement('button');b.type='button';b.className='network';let name=document.createElement('b');name.textContent=n.ssid||'(verstecktes WLAN)';let detail=document.createElement('small');detail.textContent=n.rssi+' dBm - '+n.percent+' % - '+n.encryption+(n.secured?' (gesichert)':'');b.append(name,detail);b.onclick=()=>{document.querySelector('[name=ssid]').value=n.ssid;box.textContent='Ausgew\\u00e4hlt: '+n.ssid;};box.appendChild(b);});}).catch(()=>box.textContent='Die WLAN-Suche ist fehlgeschlagen.');}"
    "status();setInterval(status,2000);</script>";

  const String modeForm = "<h2>Betriebsart</h2><form method='post' action='/mode'>"
    "<label class='opt'><input type='radio' name='mode' value='nat'" + String(bridge ? "" : " checked") + ">NAT &ndash; eigenes Netz 192.168.50.x, mehrere Ger&auml;te m&ouml;glich</label>"
    "<label class='opt'><input type='radio' name='mode' value='bridge'" + String(bridge ? " checked" : "") + ">Bridge &ndash; IP direkt vom Router, nur <b>ein</b> Ger&auml;t, nur IPv4</label>"
    "<button type='submit'>&Uuml;bernehmen und neu starten</button></form>";

  const String footer = bridge
    ? "<p><small>Im Bridge-Modus reicht die Bridge die Daten direkt zum Router durch. Sie selbst hat im Router-Netz keine eigene Adresse; diese Seite ist nur &uuml;ber das Einrichtungs-WLAN erreichbar.</small></p>"
    : "<p><small>Das Ethernet-Ger&auml;t bekommt Adresse, Gateway und DNS von der Bridge. Die Bridge &uuml;bersetzt die Verbindung zum Router.</small></p>";

  const String html = pageHeader("WT32 Ethernet-WLAN-Bridge") + "<h1>Ethernet-WLAN-Bridge</h1>" + routerState + signalMeterHtml() + ethernetState + linkState +
    "<p>Diese Seite bleibt &uuml;ber das Einrichtungs-WLAN erreichbar: <b>192.168.4.1</b>.</p>"
    "<h2>Router-WLAN</h2><button class='secondary' type='button' onclick='scan()'>Verf&uuml;gbare WLANs suchen</button><div id='networks'></div>"
    "<form method='post' action='/save'><label>WLAN-Name des Routers<input name='ssid' maxlength='32' value='" + htmlEscape(routerSsid) + "' required></label>"
    "<label>WLAN-Passwort des Routers<input name='password' type='password' maxlength='63' placeholder='Nur &auml;ndern, wenn n&ouml;tig'></label>"
    "<button type='submit'>Speichern und verbinden</button></form>" +
    modeForm + footer + script + "</div></body></html>";
  webServer.send(200, "text/html; charset=utf-8", html);
}

void connectToRouter() {
  if (routerSsid.isEmpty()) return;
  WiFi.disconnect(false, false);
  delay(100);
  WiFi.begin(routerSsid.c_str(), routerPassword.c_str());
}

void saveSettings() {
  routerSsid = webServer.arg("ssid");
  const String newPassword = webServer.arg("password");
  preferences.putString("ssid", routerSsid);
  if (!newPassword.isEmpty()) {
    routerPassword = newPassword;
    preferences.putString("password", routerPassword);
  }
  webServer.send(200, "text/html; charset=utf-8", pageHeader("Gespeichert") + "<h1>Gespeichert</h1><p>Die Bridge verbindet sich jetzt mit <b>" + htmlEscape(routerSsid) + "</b>. Kehre nach ein paar Sekunden zur <a href='/'>Startseite</a> zur&uuml;ck.</p></div></body></html>");
  connectToRouter();
}

void saveMode() {
  const BridgeMode newMode = webServer.arg("mode") == "bridge" ? MODE_BRIDGE : MODE_NAT;
  preferences.putUChar("mode", newMode);
  const String name = newMode == MODE_BRIDGE ? "Bridge" : "NAT";
  webServer.send(200, "text/html; charset=utf-8", pageHeader("Neustart") + "<h1>Neustart</h1><p>Betriebsart <b>" + name + "</b> gespeichert. Die Bridge startet neu. Verbinde dich danach wieder mit dem WLAN <b>" + String(SETUP_AP_SSID) + "</b> und &ouml;ffne <a href='/'>192.168.4.1</a>.</p><p><small>Ziehe am LAN-Ger&auml;t kurz das Kabel ab oder erneuere dort die IP-Adresse, damit es eine Adresse aus dem neuen Netz holt.</small></p></div></body></html>");
  restartAtMs = millis() + 1500;  // Antwort erst noch ausliefern
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
  bridgeMode = preferences.getUChar("mode", MODE_NAT) == MODE_BRIDGE ? MODE_BRIDGE : MODE_NAT;
  Serial.printf("Betriebsart: %s\n", bridgeMode == MODE_BRIDGE ? "Bridge" : "NAT");

  WiFi.mode(WIFI_MODE_APSTA);
  WiFi.setSleep(false);  // kein WLAN-Energiesparen: geringere Latenz, stabilere Bridge
  WiFi.softAPConfig(IPAddress(192, 168, 4, 1), IPAddress(192, 168, 4, 1), IPAddress(255, 255, 255, 0));
  WiFi.softAP(SETUP_AP_SSID, SETUP_AP_PASSWORD);
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
  if (restartAtMs != 0 && static_cast<int32_t>(millis() - restartAtMs) >= 0) {
    ESP.restart();
  }
}
