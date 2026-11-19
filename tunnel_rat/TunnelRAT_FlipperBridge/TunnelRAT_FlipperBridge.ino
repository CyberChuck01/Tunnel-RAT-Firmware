// Device: ESP32 WiFi bridge for Flipper Zero (Tunnel R.A.T. firmware)
// TunnelRAT_FlipperBridge.ino  -  cyberchuck01
//
// Sits on the Flipper's GPIO header (official WiFi Dev Board / ESP32-S2, or any
// ESP32 wired to pins 13/14/GND), joins the tunnel-rat AP and turns the
// Flipper CLI (started by the "Tunnel R.A.T. Link" app) into a TCP socket.
// It announces itself to the dashboard so the FLIPPER tab finds it on its own.
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
    connectWifi();
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
