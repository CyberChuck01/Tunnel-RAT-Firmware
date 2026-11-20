// Device: ESP32 WiFi bridge for Flipper Zero (Tunnel R.A.T. firmware)
// TunnelRAT_FlipperBridge.ino  -  cyberchuck01
//
// Sits on the Flipper's GPIO header (official WiFi Dev Board / ESP32-S2, or any
// ESP32 wired to pins 13/14/GND), joins the tunnel-rat AP and turns the
// Flipper CLI (started by the "Tunnel R.A.T. Link" app) into a TCP socket.
// It announces itself to the dashboard so the FLIPPER tab finds it on its own.
//
// v2: also answers the Tunnel R.A.T. Link app's "TR" lines itself, so WiFi Scan and the
// Flock / Axon spotter work from the Flipper even away from the NanoPi:
//   TR HELLO      -> TR HELLO TunnelRAT-ESP32 wifi[,ble]
//   TR WIFI SCAN  -> TR AP bssid|ch|rssi|enc|flag|ssid ...  TR END WIFI n
//   TR BLE SCAN   -> (ESP32 with Bluetooth only) TR BT mac|rssi|flag|name ... TR END BLE n
// flag: FLOCK (name match / Flock's registered block), FLOCK? (community suspect
// prefix - may be a false positive), AXON (00:25:DF), MESH (Meshtastic/MeshCore), -
//
//   Flipper pin 13 (TX)  ->  ESP32 RX pin
//   Flipper pin 14 (RX)  <-  ESP32 TX pin
//   Flipper GND          ->  ESP32 GND
//
// Board: "ESP32S2 Dev Module" for the WiFi Dev Board (USB CDC On Boot: Enabled)
// Libraries: none beyond the ESP32 Arduino core.

#include <WiFi.h>
#include <HTTPClient.h>

// ---- settings -------------------------------------------------------------
const char* WIFI_SSID   = "tunnel-rat";
const char* WIFI_PASS   = "tunnelrat";              // your AP password
const char* TR_HOST     = "192.168.50.1";           // NanoPi on the AP
const uint16_t TR_PORT  = 8080;
const char* TR_API_KEY  = "tunnelrat-change-me";    // = REMOTE_API_KEY on the NanoPi
const uint16_t BRIDGE_PORT = 2323;                  // dashboard connects here

#if defined(CONFIG_IDF_TARGET_ESP32S2)
  // Official Flipper WiFi Dev Board: UART0 pads go to the Flipper header
  #define BRIDGE_RX_PIN 44
  #define BRIDGE_TX_PIN 43
#else
  // Generic ESP32 / XIAO etc: wire these to Flipper 13/14
  #define BRIDGE_RX_PIN 16
  #define BRIDGE_TX_PIN 17
#endif
#define BRIDGE_BAUD 115200
// ---------------------------------------------------------------------------

HardwareSerial& FZ = Serial1;

#if defined(CONFIG_BT_ENABLED) && !defined(CONFIG_IDF_TARGET_ESP32S2)
  #include <BLEDevice.h>
  #define HAS_BLE 1
#else
  #define HAS_BLE 0
#endif

// --- surveillance flags -----------------------------------------------------
static const char* FLOCK_SUSPECT[] = {
  "70:C9:4E","3C:91:80","D8:F3:BC","80:30:49","B8:35:32","14:5A:FC","74:4C:A1","08:3A:88",
  "9C:2F:9D","C0:35:32","94:08:53","E4:AA:EA","F4:6A:DD","F8:A2:D6","24:B2:B9","00:F4:8D",
  "D0:39:57","E8:D0:FC","E0:4F:43","B8:1E:A4","70:08:94","58:8E:81","EC:1B:BD","3C:71:BF",
  "58:00:E3","90:35:EA","5C:93:A2","64:6E:69","48:27:EA","A4:CF:12","82:6B:F2"};

static bool icontains(const String& hay, const char* needle) {
  String h = hay; h.toLowerCase(); return h.indexOf(needle) >= 0;
}

static String flagFor(const String& mac, const String& name) {
  String pre = mac.substring(0, 8); pre.toUpperCase();
  if (icontains(name, "flock") || icontains(name, "penguin") || icontains(name, "pigvision")) return "FLOCK";
  if (icontains(name, "meshtastic") || icontains(name, "meshcore")) return "MESH";
  if (pre == "00:25:DF") return "AXON";
  if (pre == "B4:1E:52") return "FLOCK";
  for (auto p : FLOCK_SUSPECT) if (pre == p) return "FLOCK?";
  return "-";
}

static void trSend(const String& line) { FZ.print(line); FZ.print("\n"); }

static const char* encName(wifi_auth_mode_t m) {
  switch (m) {
    case WIFI_AUTH_OPEN: return "OPEN";
    case WIFI_AUTH_WEP: return "WEP";
    case WIFI_AUTH_WPA_PSK: return "WPA";
    case WIFI_AUTH_WPA2_PSK: return "WPA2";
    case WIFI_AUTH_WPA_WPA2_PSK: return "WPA/2";
    case WIFI_AUTH_WPA2_ENTERPRISE: return "WPA2-EAP";
    case WIFI_AUTH_WPA3_PSK: return "WPA3";
    case WIFI_AUTH_WPA2_WPA3_PSK: return "WPA2/3";
    default: return "?";
  }
}

static void trWifiScan() {
  int n = WiFi.scanNetworks(false, true);
  int sent = 0;
  for (int i = 0; i < n && sent < 40; i++, sent++) {
    String mac = WiFi.BSSIDstr(i); mac.toUpperCase();
    String ssid = WiFi.SSID(i); ssid.replace("|", "/");
    trSend("TR AP " + mac + "|" + String(WiFi.channel(i)) + "|" + String(WiFi.RSSI(i)) + "|" +
           encName(WiFi.encryptionType(i)) + "|" + flagFor(mac, ssid) + "|" + ssid);
  }
  WiFi.scanDelete();
  trSend("TR END WIFI " + String(sent));
}

static void trBleScan() {
#if HAS_BLE
  static bool inited = false;
  if (!inited) { BLEDevice::init(""); inited = true; }
  BLEScan* scan = BLEDevice::getScan();
  scan->setActiveScan(true);
  BLEScanResults* res = scan->start(6, false);
  int sent = 0;
  for (int i = 0; res && i < res->getCount() && sent < 40; i++, sent++) {
    BLEAdvertisedDevice d = res->getDevice(i);
    String mac = String(d.getAddress().toString().c_str()); mac.toUpperCase();
    String name = d.haveName() ? String(d.getName().c_str()) : String("");
    name.replace("|", "/");
    trSend("TR BT " + mac + "|" + String(d.getRSSI()) + "|" + flagFor(mac, name) + "|" + name);
  }
  scan->clearResults();
  trSend("TR END BLE " + String(sent));
#else
  trSend("TR ERR this board has no Bluetooth - use the NanoPi link for BLE");
#endif
}

static String trLine;
static void trFeed(uint8_t c) {
  if (c == '\r') return;
  if (c != '\n') { if (trLine.length() < 160) trLine += (char)c; return; }
  String l = trLine; trLine = "";
  if (!l.startsWith("TR ")) return;
  if (l.startsWith("TR HELLO")) trSend(String("TR HELLO TunnelRAT-ESP32 wifi") + (HAS_BLE ? ",ble" : ""));
  else if (l == "TR WIFI SCAN") trWifiScan();
  else if (l == "TR BLE SCAN") trBleScan();
  else trSend("TR ERR the WiFi board only does WIFI SCAN" + String(HAS_BLE ? " / BLE SCAN" : "") + " - use the NanoPi for the rest");
}
WiFiServer server(BRIDGE_PORT);
WiFiClient client;
unsigned long lastAnnounce = 0;
String deviceId;

void announce() {
  if (WiFi.status() != WL_CONNECTED) return;
  HTTPClient http;
  String url = String("http://") + TR_HOST + ":" + TR_PORT + "/api/remote/flipper/announce";
  http.begin(url);
  http.setTimeout(3000);
  http.addHeader("Content-Type", "application/json");
  http.addHeader("X-API-Key", TR_API_KEY);
  String body = String("{\"id\":\"") + deviceId + "\",\"ip\":\"" + WiFi.localIP().toString() +
                "\",\"port\":" + BRIDGE_PORT + ",\"rssi\":" + WiFi.RSSI() +
                ",\"client\":" + (client && client.connected() ? "true" : "false") + "}";
  int code = http.POST(body);
  Serial.printf("[bridge] announce -> %d\n", code);
  http.end();
}

void connectWifi() {
  if (WiFi.status() == WL_CONNECTED) return;
  Serial.printf("[bridge] joining %s", WIFI_SSID);
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  for (int i = 0; i < 40 && WiFi.status() != WL_CONNECTED; i++) {
    delay(250);
    Serial.print(".");
  }
  Serial.println();
  if (WiFi.status() == WL_CONNECTED) {
    Serial.printf("[bridge] %s  tcp:%u\n", WiFi.localIP().toString().c_str(), BRIDGE_PORT);
    lastAnnounce = 0;
  }
}

void setup() {
  Serial.begin(115200);   // USB debug console
  FZ.setRxBufferSize(4096);
  FZ.begin(BRIDGE_BAUD, SERIAL_8N1, BRIDGE_RX_PIN, BRIDGE_TX_PIN);
  uint8_t mac[6];
  WiFi.macAddress(mac);
  char id[24];
  snprintf(id, sizeof(id), "tr-flipper-%02x%02x%02x", mac[3], mac[4], mac[5]);
  deviceId = id;
  WiFi.setSleep(false);
  connectWifi();
  server.begin();
  server.setNoDelay(true);
}

void loop() {
  if (WiFi.status() != WL_CONNECTED) {
    if (client) client.stop();
    static unsigned long lastTry = 0;
    if (millis() - lastTry > 30000 || lastTry == 0) { lastTry = millis(); connectWifi(); }
    // no NanoPi around: still answer the Flipper (standalone WiFi scan / spotter)
    while (FZ.available()) trFeed(FZ.read());
    return;
  }

  // one dashboard connection at a time; a new one replaces the old
  if (server.hasClient()) {
    WiFiClient incoming = server.available();
    if (client && client.connected()) client.stop();
    client = incoming;
    client.setNoDelay(true);
    Serial.printf("[bridge] dashboard connected from %s\n", client.remoteIP().toString().c_str());
    FZ.write('\r');          // nudge the CLI so a fresh prompt appears
    lastAnnounce = 0;
  }

  uint8_t buf[256];
  // Flipper -> dashboard
  int n = FZ.available();
  if (n > 0) {
    n = FZ.readBytes(buf, min(n, (int)sizeof(buf)));
    if (client && client.connected()) client.write(buf, n);
    for (int i = 0; i < n; i++) trFeed(buf[i]);   // Link app TR lines are answered here
  }
  // dashboard -> Flipper
  if (client && client.connected()) {
    int m = client.available();
    if (m > 0) {
      m = client.read(buf, min(m, (int)sizeof(buf)));
      FZ.write(buf, m);
    }
  }

  if (millis() - lastAnnounce > 20000 || lastAnnounce == 0) {
    lastAnnounce = millis();
    announce();
  }
}
