#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <Preferences.h>
#include <esp_event.h>
#include <esp_netif.h>
#include <esp_eth.h>
#include <esp_eth_netif_glue.h>
#include <dhcpserver/dhcpserver.h>

// Ethernet-LAN nutzt ein eigenes 192.168.50.0/24-Netz; WLAN bleibt der Upstream.

String htmlEscape(const String &text);
String jsonEscape(const String &text);
int wifiPercent();
String signalClass(int percent);
String encryptionName(wifi_auth_mode_t mode);
String pageHeader(const String &title);
String signalMeterHtml();
void showStatus();
void showNetworks();
void showHome();
void connectToRouter();
void saveSettings();
void onEthernetEvent(void *argument, esp_event_base_t eventBase, int32_t eventId, void *eventData);
void onNetworkEvent(WiFiEvent_t event);
void startEthernetLan();
void startLanServices();
void onIpEvent(void *argument, esp_event_base_t eventBase, int32_t eventId, void *eventData);
String lanStatusJson();

constexpr int ETH_PHY_POWER_PIN = 16;
constexpr int ETH_MDC_PIN = 23;
constexpr int ETH_MDIO_PIN = 18;
constexpr int ETH_PHY_ADDRESS = 1;
constexpr int SERIAL_RX_PIN = 3;
constexpr int SERIAL_TX_PIN = 1;
constexpr char SETUP_AP_SSID[] = "WT32-Bridge-Setup";
constexpr char SETUP_AP_PASSWORD[] = "BridgeSetup26";

WebServer webServer(80);
Preferences preferences;
esp_netif_t *ethernetNetif = nullptr;
esp_eth_handle_t ethernetHandle = nullptr;
String routerSsid;
String routerPassword;
bool ethernetLinkUp = false;
bool ethernetLanReady = false;
bool wifiConnected = false;
volatile bool ethernetServicesPending = false;

// Informationen ueber die Geraete am LAN-Port (per DHCP-Lease erfasst)
struct LanClient {
  bool used;
  uint8_t mac[6];
  uint32_t ip;          // Netzwerk-Byte-Reihenfolge wie esp_ip4_addr_t
  uint32_t assignedMs;  // millis() bei der letzten Vergabe
};
constexpr int MAX_LAN_CLIENTS = 4;
LanClient lanClients[MAX_LAN_CLIENTS] = {};
portMUX_TYPE lanClientsLock = portMUX_INITIALIZER_UNLOCKED;
volatile int ethernetSpeedMbit = 0;
volatile bool ethernetFullDuplex = false;
volatile uint32_t ethernetLinkSinceMs = 0;

String htmlEscape(const String &text) {
  String escaped;
  for (size_t i = 0; i < text.length(); ++i) {
    const char c = text[i];
    if (c == '&') escaped += "&amp;";
    else if (c == '<') escaped += "&lt;";
    else if (c == '>') escaped += "&gt;";
    else if (c == '\"') escaped += "&quot;";
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
    else if (c != '\n' && c != '\r') escaped += c;
  }
  return escaped;
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

String pageHeader(const String &title) {
  return "<!doctype html><html lang='de'><head><meta name='viewport' content='width=device-width,initial-scale=1'><title>" + title + "</title><style>body{font-family:Arial,sans-serif;max-width:700px;margin:30px auto;padding:0 18px;background:#f2f6fa;color:#17212b}.card{background:#fff;border-radius:16px;padding:24px;box-shadow:0 4px 18px #0002}h1{margin-top:0;color:#1263a6}.status{padding:12px 14px;margin:12px 0;border-radius:10px;background:#edf5fd}.ok{color:#08783d}.wait{color:#875b00}.bad{color:#a32020}.meter{display:flex;align-items:flex-end;gap:4px;height:38px;margin:10px 0 3px}.bar{width:13px;border-radius:3px 3px 0 0;background:#d3dae1}.bar.on.good{background:#1a9b59}.bar.on.fair{background:#dd9a17}.bar.on.weak{background:#ce3e3e}label{display:block;font-weight:bold;margin-top:16px}input{box-sizing:border-box;width:100%;padding:12px;margin-top:6px;border:1px solid #aac;border-radius:8px;font-size:16px}button{margin-top:22px;background:#1263a6;color:#fff;border:0;border-radius:8px;padding:12px 18px;font-size:16px;cursor:pointer}.secondary{margin-top:12px;background:#587080}.network{display:block;width:100%;text-align:left;margin-top:8px;padding:11px;border:1px solid #cbd8e3;border-radius:8px;background:#f8fbfe;color:#17212b}.network b{display:block}.network small,small{color:#4b5865}table.info{width:100%;border-collapse:collapse;margin-top:8px}table.info td{padding:5px 4px;border-top:1px solid #d6e2ee;vertical-align:top}table.info td:first-child{color:#4b5865;width:45%}</style></head><body><div class='card'>";
}

String signalMeterHtml() {
  return "<div class='status'><b>WLAN-Empfang</b><div class='meter' id='meter'><i class='bar' style='height:20%'></i><i class='bar' style='height:40%'></i><i class='bar' style='height:65%'></i><i class='bar' style='height:100%'></i></div><span id='signalText'>Wird geladen...</span></div>";
}

void showStatus() {
  const String json = "{\"wifi\":" + String(wifiConnected ? "true" : "false") + ",\"ssid\":\"" + jsonEscape(WiFi.SSID()) + "\",\"ip\":\"" + WiFi.localIP().toString() + "\",\"rssi\":" + String(wifiConnected ? WiFi.RSSI() : 0) + ",\"percent\":" + String(wifiPercent()) + ",\"quality\":\"" + signalClass(wifiPercent()) + "\",\"ethLink\":" + String(ethernetLinkUp ? "true" : "false") + ",\"dhcp\":" + String(ethernetLanReady ? "true" : "false") + ",\"lan\":" + lanStatusJson() + "}";
  webServer.send(200, "application/json", json);
}

String macToString(const uint8_t *mac) {
  char text[18];
  snprintf(text, sizeof(text), "%02X:%02X:%02X:%02X:%02X:%02X", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
  return String(text);
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
    json += "{\"ip\":\"" + IPAddress(client.ip).toString() + "\",\"mac\":\"" + macToString(client.mac) + "\",\"seconds\":" + String((now - client.assignedMs) / 1000) + "}";
  }
  json += "]}";
  return json;
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
  const String routerState = wifiConnected ? "<p class='ok'>Mit Router verbunden: <b>" + htmlEscape(WiFi.SSID()) + "</b><br>Router-IP der Bridge: " + WiFi.localIP().toString() + "</p>" : "<p class='wait'>Noch nicht mit dem Router verbunden. Speichere die Zugangsdaten unten; die Bridge versucht die Verbindung automatisch.</p>";
  const String ethernetState = ethernetLanReady ? "<p class='ok'>Ethernet-DHCP ist aktiv. Das angeschlossene Ger&auml;t bekommt automatisch eine Adresse im Netz <b>192.168.50.x</b>; Gateway ist <b>192.168.50.1</b>.</p>" : "<p class='bad'>Der Ethernet-DHCP-Dienst ist noch nicht bereit.</p>";
  const String linkState = "<div class='status'><b>Ger&auml;t am LAN-Port</b><div id='lanInfo'>Wird geladen...</div></div>";
  const String script = "<script>function status(){fetch('/status').then(r=>r.json()).then(s=>{let bars=document.querySelectorAll('#meter .bar'),n=s.wifi?Math.ceil(s.percent/25):0;bars.forEach((b,i)=>b.className='bar '+(i<n?'on '+s.quality:''));document.getElementById('signalText').textContent=s.wifi?s.rssi+' dBm - '+s.percent+' %':'Nicht mit dem Router verbunden';showLan(s);});}function dur(t){let h=Math.floor(t/3600),m=Math.floor(t%3600/60),x=t%60;return (h?h+' h ':'')+(h||m?m+' min ':'')+x+' s';}function row(k,v){return '<tr><td>'+k+'</td><td><b>'+v+'</b></td></tr>';}function showLan(s){let l=s.lan,box=document.getElementById('lanInfo'),h='';if(!s.ethLink){box.innerHTML='<p class=\\'wait\\'>Kein Kabel erkannt. Stecke das Kabel am Endger\\u00e4t und an der Bridge fest ein.</p>';return;}h+=row('Verbindung',l.speed+' Mbit/s, '+(l.fullDuplex?'Vollduplex':'Halbduplex'));h+=row('Kabel steckt seit',dur(l.linkSeconds));if(!l.clients.length){h+=row('IP-Adresse','noch keine per DHCP vergeben');}l.clients.forEach((c,i)=>{let p=l.clients.length>1?' ('+(i+1)+')':'';h+=row('IP-Adresse'+p,c.ip);h+=row('MAC-Adresse'+p,c.mac);h+=row('Adresse vergeben vor'+p,dur(c.seconds));});h+=row('Gateway / DNS','192.168.50.1 / 1.1.1.1');if(l.leaseMinutes)h+=row('Lease-Dauer',l.leaseMinutes+' min');if(l.bridgeMac)h+=row('MAC der Bridge (LAN)',l.bridgeMac);box.innerHTML='<table class=\\'info\\'>'+h+'</table>';}function scan(){let box=document.getElementById('networks');box.textContent='Suche nach WLANs ...';fetch('/networks').then(r=>r.json()).then(list=>{box.textContent='';if(!list.length){box.textContent='Keine WLANs gefunden.';return;}list.forEach(n=>{let b=document.createElement('button');b.type='button';b.className='network';let name=document.createElement('b');name.textContent=n.ssid||'(verstecktes WLAN)';let detail=document.createElement('small');detail.textContent=n.rssi+' dBm - '+n.percent+' % - '+n.encryption+(n.secured?' (gesichert)':'');b.append(name,detail);b.onclick=()=>{document.querySelector('[name=ssid]').value=n.ssid;box.textContent='Ausgew\\u00e4hlt: '+n.ssid;};box.appendChild(b);});}).catch(()=>box.textContent='Die WLAN-Suche ist fehlgeschlagen.');}status();setInterval(status,2000);</script>";
  const String html = pageHeader("WT32 Ethernet-WLAN-Bridge") + "<h1>Ethernet-WLAN-Bridge</h1>" + routerState + signalMeterHtml() + ethernetState + linkState + "<p>Diese Seite bleibt &uuml;ber das Einrichtungs-WLAN erreichbar: <b>192.168.4.1</b>.</p><button class='secondary' type='button' onclick='scan()'>Verf&uuml;gbare WLANs suchen</button><div id='networks'></div><form method='post' action='/save'><label>WLAN-Name des Routers<input name='ssid' maxlength='32' value='" + htmlEscape(routerSsid) + "' required></label><label>WLAN-Passwort des Routers<input name='password' type='password' maxlength='63' placeholder='Nur &auml;ndern, wenn n&ouml;tig'></label><button type='submit'>Speichern und verbinden</button></form><p><small>Das Ethernet-Ger&auml;t bekommt Adresse, Gateway und DNS von der Bridge. Die Bridge &uuml;bersetzt die Verbindung zum Router.</small></p>" + script + "</div></body></html>";
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
    ethernetServicesPending = true;
    Serial.printf("Ethernet-Kabel verbunden (%d Mbit/s, %s)\n", ethernetSpeedMbit, ethernetFullDuplex ? "Vollduplex" : "Halbduplex");
  } else if (eventId == ETHERNET_EVENT_DISCONNECTED) {
    ethernetLinkUp = false;
    ethernetLanReady = false;
    portENTER_CRITICAL(&lanClientsLock);
    for (LanClient &client : lanClients) client.used = false;
    portEXIT_CRITICAL(&lanClientsLock);
    Serial.println("Ethernet-Kabel getrennt");
  } else if (eventId == ETHERNET_EVENT_STOP) {
    ethernetLanReady = false;
  }
}

// Wird von ESP-IDF bei jeder DHCP-Vergabe ausgeloest (auch fuer das Setup-WLAN, daher Filter auf die LAN-Schnittstelle).
void onIpEvent(void *argument, esp_event_base_t eventBase, int32_t eventId, void *eventData) {
  if (eventId != IP_EVENT_AP_STAIPASSIGNED) return;
  const ip_event_ap_staipassigned_t *info = static_cast<const ip_event_ap_staipassigned_t *>(eventData);
  if (info->esp_netif != ethernetNetif) return;

  portENTER_CRITICAL(&lanClientsLock);
  int slot = -1;
  for (int i = 0; i < MAX_LAN_CLIENTS; ++i) {
    if (lanClients[i].used && memcmp(lanClients[i].mac, info->mac, 6) == 0) { slot = i; break; }
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
  lanClients[slot].used = true;
  memcpy(lanClients[slot].mac, info->mac, 6);
  lanClients[slot].ip = info->ip.addr;
  lanClients[slot].assignedMs = millis();
  portEXIT_CRITICAL(&lanClientsLock);

  Serial.printf("LAN-Geraet %s hat " IPSTR " bekommen\n", macToString(info->mac).c_str(), IP2STR(&info->ip));
}

void onNetworkEvent(WiFiEvent_t event) {
  if (event == ARDUINO_EVENT_WIFI_STA_GOT_IP) {
    wifiConnected = true;
    Serial.print("Router-IP: ");
    Serial.println(WiFi.localIP());
  } else if (event == ARDUINO_EVENT_WIFI_STA_DISCONNECTED) {
    wifiConnected = false;
  }
}

void startEthernetLan() {
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
    return;
  }

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
  esp_event_handler_register(ETH_EVENT, ESP_EVENT_ANY_ID, &onEthernetEvent, nullptr);
  esp_event_handler_register(IP_EVENT, IP_EVENT_AP_STAIPASSIGNED, &onIpEvent, nullptr);
  if (esp_eth_start(ethernetHandle) != ESP_OK) {
    Serial.println("Ethernet-LAN konnte nicht eingeschaltet werden");
    return;
  }

  Serial.println("Ethernet-LAN gestartet, warte auf Kabel ...");
  startLanServices();
}

// Startet DHCP-Server und NAPT auf dem LAN. Wird beim Start und bei jedem Link-Up aufgerufen.
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

void setup() {
  Serial.begin(115200);
  delay(250);
  WiFi.onEvent(onNetworkEvent);
  preferences.begin("bridge", false);
  routerSsid = preferences.getString("ssid", "");
  routerPassword = preferences.getString("password", "");
  WiFi.mode(WIFI_MODE_APSTA);
  WiFi.softAPConfig(IPAddress(192, 168, 4, 1), IPAddress(192, 168, 4, 1), IPAddress(255, 255, 255, 0));
  WiFi.softAP(SETUP_AP_SSID, SETUP_AP_PASSWORD);
  webServer.on("/", HTTP_GET, showHome);
  webServer.on("/status", HTTP_GET, showStatus);
  webServer.on("/networks", HTTP_GET, showNetworks);
  webServer.on("/save", HTTP_POST, saveSettings);
  webServer.onNotFound(showHome);
  webServer.begin();
  startEthernetLan();
  connectToRouter();
  Serial.println("Einrichtungsseite: http://192.168.4.1");
}

void loop() {
  if (ethernetServicesPending) startLanServices();
  webServer.handleClient();
}