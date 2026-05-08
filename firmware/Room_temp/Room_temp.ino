/*
 * Room Temperature Monitor
 * ESP8266 + DHT11 + OLED (SSD1306) + WiFiManager
 *  - HiveMQ Cloud (MQTT over TLS)
 *  - Google Sheets logging (HTTPS POST to Apps Script Web App)
 *  - Local Web Server (JSON API + simple HTML)
 *  - OLED status display
 *
 * Libraries (install via Library Manager):
 *   - WiFiManager by tzapu
 *   - PubSubClient by Nick O'Leary
 *   - DHT sensor library by Adafruit
 *   - Adafruit Unified Sensor
 *   - Adafruit GFX Library
 *   - Adafruit SSD1306
 *   - ArduinoJson (v6)
 *
 * Wiring (NodeMCU / Wemos D1 mini):
 *   DHT11    -> VCC=3V3, GND=GND, DATA=D4 (GPIO2)
 *   OLED I2C -> VCC=3V3, GND=GND, SDA=D2 (GPIO4), SCL=D1 (GPIO5)
 */

#include <ESP8266WiFi.h>
#include <ESP8266WebServer.h>
#include <WiFiClientSecureBearSSL.h>
#include <ESP8266HTTPClient.h>
#include <ESP8266mDNS.h>
#include <ArduinoOTA.h>
#include <DNSServer.h>
#include <WiFiManager.h>          // tzapu/WiFiManager
#include <PubSubClient.h>
#include <DHT.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <ArduinoJson.h>
#include <EEPROM.h>
#include "secrets.h"

// ---------------- Pins ----------------
// Use raw GPIO numbers so the sketch builds on any ESP8266 variant
// (NodeMCU, D1 mini, bare ESP-12E module). Mapping:
//   GPIO2  = D4 (also onboard LED on most dev boards)
//   GPIO4  = D2 (I2C SDA)
//   GPIO5  = D1 (I2C SCL)
//   GPIO0  = D3 / FLASH button (used to force config portal at boot)
#define DHT_PIN     2     // GPIO2
#define DHT_TYPE    DHT11
#define I2C_SDA     4     // GPIO4
#define I2C_SCL     5     // GPIO5
#define BOOT_BTN    0     // GPIO0 — held LOW at boot = open WiFiManager portal

// ---------------- OLED ----------------
#define OLED_W      128
#define OLED_H      32
#define OLED_ADDR   0x3C
Adafruit_SSD1306 display(OLED_W, OLED_H, &Wire, -1);

// ---------------- Defaults / Config ----------------
// These can be overridden in the WiFiManager captive portal.
char mqtt_server[64]   = DEFAULT_MQTT_HOST;
char mqtt_port_s[6]    = DEFAULT_MQTT_PORT;
char mqtt_user[40]     = DEFAULT_MQTT_USER;
char mqtt_pass[40]     = DEFAULT_MQTT_PASS;
char mqtt_base[40]     = DEFAULT_MQTT_BASE;        // base topic; device publishes to <base>/<device_id>
char gs_host[80]       = DEFAULT_GS_HOST;
char gs_path[140]      = DEFAULT_GS_PATH;
char project_id[24]    = DEFAULT_PROJECT;
char device_id[24]     = DEFAULT_DEVICE_ID;

// EEPROM layout: simple struct dump
struct Config {
  uint32_t magic;
  char mqtt_server[64];
  char mqtt_port_s[6];
  char mqtt_user[40];
  char mqtt_pass[40];
  char mqtt_base[40];
  char gs_host[80];
  char gs_path[140];
  char project_id[24];
  char device_id[24];
} cfg;
const uint32_t CFG_MAGIC = 0xC0FFEE05;

void loadConfig() {
  EEPROM.begin(sizeof(Config));
  EEPROM.get(0, cfg);
  EEPROM.end();
  if (cfg.magic == CFG_MAGIC) {
    strncpy(mqtt_server, cfg.mqtt_server, sizeof(mqtt_server));
    strncpy(mqtt_port_s, cfg.mqtt_port_s, sizeof(mqtt_port_s));
    strncpy(mqtt_user,   cfg.mqtt_user,   sizeof(mqtt_user));
    strncpy(mqtt_pass,   cfg.mqtt_pass,   sizeof(mqtt_pass));
    strncpy(mqtt_base,   cfg.mqtt_base,   sizeof(mqtt_base));
    strncpy(gs_host,     cfg.gs_host,     sizeof(gs_host));
    strncpy(gs_path,     cfg.gs_path,     sizeof(gs_path));
    strncpy(project_id,  cfg.project_id,  sizeof(project_id));
    strncpy(device_id,   cfg.device_id,   sizeof(device_id));
  }
}

void saveConfig() {
  cfg.magic = CFG_MAGIC;
  strncpy(cfg.mqtt_server, mqtt_server, sizeof(cfg.mqtt_server));
  strncpy(cfg.mqtt_port_s, mqtt_port_s, sizeof(cfg.mqtt_port_s));
  strncpy(cfg.mqtt_user,   mqtt_user,   sizeof(cfg.mqtt_user));
  strncpy(cfg.mqtt_pass,   mqtt_pass,   sizeof(cfg.mqtt_pass));
  strncpy(cfg.mqtt_base,   mqtt_base,   sizeof(cfg.mqtt_base));
  strncpy(cfg.gs_host,     gs_host,     sizeof(cfg.gs_host));
  strncpy(cfg.gs_path,     gs_path,     sizeof(cfg.gs_path));
  strncpy(cfg.project_id,  project_id,  sizeof(cfg.project_id));
  strncpy(cfg.device_id,   device_id,   sizeof(cfg.device_id));
  EEPROM.begin(sizeof(Config));
  EEPROM.put(0, cfg);
  EEPROM.commit();
  EEPROM.end();
}

bool shouldSaveConfig = false;
void saveConfigCallback() { shouldSaveConfig = true; }

// ---------------- Globals ----------------
DHT dht(DHT_PIN, DHT_TYPE);
ESP8266WebServer server(80);
BearSSL::WiFiClientSecure secureClient;
PubSubClient mqtt(secureClient);

float lastTemp = NAN;
float lastHum  = NAN;
unsigned long lastReadMs   = 0;
unsigned long lastPubMs    = 0;
unsigned long lastSheetMs  = 0;
unsigned long lastReconnMs = 0;

const unsigned long READ_INTERVAL  = 2000;     // 2s
const unsigned long PUB_INTERVAL   = 10000;    // 10s MQTT
const unsigned long SHEET_INTERVAL = 3600000UL; // 60 min Google Sheets

// ---------------- OLED helpers ----------------
void oledMsg(const String& l1, const String& l2 = "", const String& l3 = "") {
  display.clearDisplay();
  display.setTextSize(1);
  display.setTextColor(SSD1306_WHITE);
  display.setCursor(0, 0);  display.println(l1);
  display.setCursor(0, 12); display.println(l2);
  display.setCursor(0, 24); display.println(l3);
  display.display();
}

void oledData() {
  display.clearDisplay();
  display.setTextColor(SSD1306_WHITE);
  // Top status line: device id + IP (or MQTT state if no IP yet)
  display.setTextSize(1);
  display.setCursor(0, 0);
  display.print(device_id);
  display.print(' ');
  if (WiFi.status() == WL_CONNECTED) {
    display.print(WiFi.localIP());
  } else {
    display.print(mqtt.connected() ? "MQTT:OK" : "...");
  }

  // Big temperature on the left
  display.setTextSize(2);
  display.setCursor(0, 14);
  if (isnan(lastTemp)) {
    display.print("--.-");
  } else {
    display.print(lastTemp, 1);
  }
  display.print((char)247);
  display.print('C');

  // Humidity on the right
  display.setTextSize(2);
  display.setCursor(80, 14);
  if (isnan(lastHum)) display.print("--%");
  else { display.print(lastHum, 0); display.print('%'); }
  display.display();
}

// ---------------- Wi-Fi / Portal ----------------
void startPortal(bool forceConfig) {
  WiFiManager wm;
  wm.setSaveConfigCallback(saveConfigCallback);

  WiFiManagerParameter p_mqtt_server("mqtts", "MQTT host",     mqtt_server, 64);
  WiFiManagerParameter p_mqtt_port  ("mqttp", "MQTT port",     mqtt_port_s, 6);
  WiFiManagerParameter p_mqtt_user  ("mqttu", "MQTT user",     mqtt_user, 40);
  WiFiManagerParameter p_mqtt_pass  ("mqttw", "MQTT pass",     mqtt_pass, 40);
  WiFiManagerParameter p_mqtt_base  ("mqttt", "MQTT base topic", mqtt_base, 40);
  WiFiManagerParameter p_gs_host    ("gsh",   "GScript host",  gs_host, 80);
  WiFiManagerParameter p_gs_path    ("gsp",   "GScript path",  gs_path, 140);
  WiFiManagerParameter p_proj       ("prj",   "Project (sheet tab)", project_id, 24);
  WiFiManagerParameter p_dev        ("dev",   "Room name (device ID)", device_id, 24);

  wm.addParameter(&p_mqtt_server);
  wm.addParameter(&p_mqtt_port);
  wm.addParameter(&p_mqtt_user);
  wm.addParameter(&p_mqtt_pass);
  wm.addParameter(&p_mqtt_base);
  wm.addParameter(&p_gs_host);
  wm.addParameter(&p_gs_path);
  wm.addParameter(&p_proj);
  wm.addParameter(&p_dev);

  oledMsg("WiFi Setup", "AP: RoomTemp-Setup", "192.168.4.1");

  bool ok;
  if (forceConfig) ok = wm.startConfigPortal("RoomTemp-Setup");
  else             ok = wm.autoConnect("RoomTemp-Setup");

  if (!ok) {
    oledMsg("WiFi failed", "Restarting...");
    delay(1500); ESP.restart();
  }

  if (shouldSaveConfig) {
    strncpy(mqtt_server, p_mqtt_server.getValue(), sizeof(mqtt_server));
    strncpy(mqtt_port_s, p_mqtt_port.getValue(),   sizeof(mqtt_port_s));
    strncpy(mqtt_user,   p_mqtt_user.getValue(),   sizeof(mqtt_user));
    strncpy(mqtt_pass,   p_mqtt_pass.getValue(),   sizeof(mqtt_pass));
    strncpy(mqtt_base,   p_mqtt_base.getValue(),   sizeof(mqtt_base));
    strncpy(gs_host,     p_gs_host.getValue(),     sizeof(gs_host));
    strncpy(gs_path,     p_gs_path.getValue(),     sizeof(gs_path));
    strncpy(project_id,  p_proj.getValue(),        sizeof(project_id));
    strncpy(device_id,   p_dev.getValue(),         sizeof(device_id));
    saveConfig();
  }
}

// ---------------- MQTT ----------------
void mqttConnect() {
  if (mqtt.connected()) return;
  if (millis() - lastReconnMs < 5000) return;
  lastReconnMs = millis();

  secureClient.setInsecure();          // skip cert validation (simple)
  mqtt.setServer(mqtt_server, atoi(mqtt_port_s));
  String cid = String("esp-") + device_id + "-" + String(ESP.getChipId(), HEX);
  Serial.printf("MQTT connect to %s:%s as %s ...\n", mqtt_server, mqtt_port_s, cid.c_str());
  String willTopic = String(mqtt_base) + "/" + device_id + "/status";
  if (mqtt.connect(cid.c_str(), mqtt_user, mqtt_pass,
                   willTopic.c_str(), 0, true, "offline")) {
    Serial.println("MQTT connected");
    mqtt.publish(willTopic.c_str(), "online", true);
  } else {
    Serial.printf("MQTT failed rc=%d\n", mqtt.state());
  }
}

void publishMQTT(float t, float h) {
  if (!mqtt.connected()) return;
  StaticJsonDocument<192> doc;
  doc["device"] = device_id;
  doc["temp"]   = t;
  doc["hum"]    = h;
  doc["rssi"]   = WiFi.RSSI();
  doc["ip"]     = WiFi.localIP().toString();
  doc["uptime"] = (uint32_t)(millis() / 1000);
  char buf[192];
  size_t n = serializeJson(doc, buf);
  String topic = String(mqtt_base) + "/" + device_id;
  mqtt.publish(topic.c_str(), (const uint8_t*)buf, n, true);  // retained: latest reading per device
  Serial.printf("MQTT pub %s: %s\n", topic.c_str(), buf);
}

// ---------------- Google Sheets ----------------
void postToSheet(float t, float h) {
  WiFiClientSecure client;
  client.setInsecure();
  HTTPClient https;
  String url = String("https://") + gs_host + gs_path;
  if (!https.begin(client, url)) { Serial.println("GS begin fail"); return; }
  https.addHeader("Content-Type", "application/json");
  https.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);

  StaticJsonDocument<256> doc;
  doc["project"] = project_id;
  doc["device"] = device_id;
  doc["temp"]   = t;
  doc["hum"]    = h;
  doc["rssi"]   = WiFi.RSSI();
  String body; serializeJson(doc, body);

  int code = https.POST(body);
  Serial.printf("GS POST %d\n", code);
  https.end();
}

// ---------------- Web Server ----------------
#include "index_html.h"

String htmlEscape(const String& value) {
  String out;
  out.reserve(value.length() + 8);
  for (size_t i = 0; i < value.length(); i++) {
    char c = value[i];
    if (c == '&') out += F("&amp;");
    else if (c == '<') out += F("&lt;");
    else if (c == '>') out += F("&gt;");
    else if (c == '"') out += F("&quot;");
    else if (c == '\'') out += F("&#39;");
    else out += c;
  }
  return out;
}

void copyArg(char* target, size_t targetSize, const String& name) {
  if (!server.hasArg(name)) return;
  server.arg(name).toCharArray(target, targetSize);
}

void handleRoot()  { server.send_P(200, "text/html", INDEX_HTML); }
void handleApi() {
  StaticJsonDocument<192> doc;
  doc["device"] = device_id;
  doc["temp"]   = isnan(lastTemp) ? 0 : lastTemp;
  doc["hum"]    = isnan(lastHum)  ? 0 : lastHum;
  doc["rssi"]   = WiFi.RSSI();
  doc["ip"]     = WiFi.localIP().toString();
  doc["uptime"] = (uint32_t)(millis() / 1000);
  doc["mqtt"]   = mqtt.connected();
  String s; serializeJson(doc, s);
  server.send(200, "application/json", s);
}
void handleConfigPage() {
  String html;
  html.reserve(5600);
  html += F("<!doctype html><html><head><meta charset='utf-8'><meta name='viewport' content='width=device-width,initial-scale=1'>");
  html += F("<title>Room Temp Config</title><style>");
  html += F(":root{--bg:#0f1218;--panel:#171c25;--line:#2b3444;--text:#edf1f5;--mut:#9aa7b7;--acc:#9fb8ff}");
  html += F("*{box-sizing:border-box}body{margin:0;padding:22px;background:var(--bg);color:var(--text);font-family:system-ui,Segoe UI,sans-serif}");
  html += F("main{max-width:760px;margin:auto}.card{border:1px solid var(--line);border-radius:8px;background:var(--panel);padding:18px}h1{margin:0 0 6px;font-size:1.35rem}p{color:var(--mut);margin:.2rem 0 1rem}");
  html += F("form{display:grid;gap:12px}.grid{display:grid;grid-template-columns:repeat(auto-fit,minmax(230px,1fr));gap:12px}label{display:grid;gap:5px;color:var(--mut);font-size:.82rem}input{width:100%;border:1px solid var(--line);border-radius:8px;background:#10151d;color:var(--text);padding:10px;font:inherit}");
  html += F("button,a.btn{display:inline-block;border:0;border-radius:8px;background:var(--acc);color:#0d1320;padding:10px 14px;font-weight:700;text-decoration:none;cursor:pointer}.actions{display:flex;gap:10px;flex-wrap:wrap;margin-top:4px}small{color:var(--mut)}code{background:#10151d;border:1px solid var(--line);border-radius:5px;padding:1px 5px}");
  html += F("</style></head><body><main><div class='card'><h1>Web server config</h1><p>Device <code>");
  html += htmlEscape(device_id);
  html += F("</code> at <code>");
  html += WiFi.localIP().toString();
  html += F("</code></p><form method='post' action='/config/save'><div class='grid'>");
  html += F("<label>Device name<input name='device_id' maxlength='23' value='"); html += htmlEscape(device_id); html += F("'></label>");
  html += F("<label>Project<input name='project_id' maxlength='23' value='"); html += htmlEscape(project_id); html += F("'></label>");
  html += F("<label>MQTT host<input name='mqtt_server' maxlength='63' value='"); html += htmlEscape(mqtt_server); html += F("'></label>");
  html += F("<label>MQTT port<input name='mqtt_port_s' maxlength='5' value='"); html += htmlEscape(mqtt_port_s); html += F("'></label>");
  html += F("<label>MQTT user<input name='mqtt_user' maxlength='39' value='"); html += htmlEscape(mqtt_user); html += F("'></label>");
  html += F("<label>MQTT password<small>Leave blank to keep current password</small><input name='mqtt_pass' maxlength='39' type='password'></label>");
  html += F("<label>MQTT base topic<input name='mqtt_base' maxlength='39' value='"); html += htmlEscape(mqtt_base); html += F("'></label>");
  html += F("<label>Google Script host<input name='gs_host' maxlength='79' value='"); html += htmlEscape(gs_host); html += F("'></label>");
  html += F("</div><label>Google Script path<input name='gs_path' maxlength='139' value='"); html += htmlEscape(gs_path); html += F("'></label>");
  html += F("<div class='actions'><button type='submit'>Save and restart</button><a class='btn' href='/'>Dashboard</a><a class='btn' href='/api'>API</a></div>");
  html += F("</form></div></main></body></html>");
  server.send(200, "text/html", html);
}
void handleConfigSave() {
  copyArg(device_id, sizeof(device_id), "device_id");
  copyArg(project_id, sizeof(project_id), "project_id");
  copyArg(mqtt_server, sizeof(mqtt_server), "mqtt_server");
  copyArg(mqtt_port_s, sizeof(mqtt_port_s), "mqtt_port_s");
  copyArg(mqtt_user, sizeof(mqtt_user), "mqtt_user");
  if (server.hasArg("mqtt_pass") && server.arg("mqtt_pass").length() > 0) {
    server.arg("mqtt_pass").toCharArray(mqtt_pass, sizeof(mqtt_pass));
  }
  copyArg(mqtt_base, sizeof(mqtt_base), "mqtt_base");
  copyArg(gs_host, sizeof(gs_host), "gs_host");
  copyArg(gs_path, sizeof(gs_path), "gs_path");
  saveConfig();
  server.send(200, "text/html", "<!doctype html><meta name='viewport' content='width=device-width,initial-scale=1'><body style='font-family:system-ui;background:#0f1218;color:#edf1f5;padding:2rem'><h1>Saved</h1><p>Restarting device...</p></body>");
  delay(600);
  ESP.restart();
}
void handleReset() {
  server.send(200, "text/plain", "Erasing Wi-Fi & restarting...");
  delay(300);
  WiFi.disconnect(true);
  ESP.restart();
}

// ---------------- Setup / Loop ----------------
void setup() {
  Serial.begin(115200);
  delay(200);
  Serial.println("\n=== Room Temp ESP8266 ===");

  Wire.begin(I2C_SDA, I2C_SCL);
  if (!display.begin(SSD1306_SWITCHCAPVCC, OLED_ADDR)) {
    Serial.println("OLED init failed");
  }
  display.clearDisplay(); display.display();
  oledMsg("Booting...", "Room Temp", "ESP8266");

  dht.begin();
  loadConfig();

  // Hold FLASH (GPIO0) low at boot to force config portal
  pinMode(BOOT_BTN, INPUT_PULLUP);
  bool force = (digitalRead(BOOT_BTN) == LOW);
  startPortal(force);

  oledMsg("WiFi OK", WiFi.SSID(), WiFi.localIP().toString());
  Serial.print("IP: "); Serial.println(WiFi.localIP());

  secureClient.setInsecure();
  mqtt.setBufferSize(512);

  server.on("/",      handleRoot);
  server.on("/api",   handleApi);
  server.on("/config", HTTP_GET, handleConfigPage);
  server.on("/config/save", HTTP_POST, handleConfigSave);
  server.on("/reset", handleReset);
  server.begin();

  // ---------------- OTA ----------------
  ArduinoOTA.setHostname(device_id);
#ifdef OTA_PASSWORD
  ArduinoOTA.setPassword(OTA_PASSWORD);
#endif
  ArduinoOTA.onStart([]() {
    String type = (ArduinoOTA.getCommand() == U_FLASH) ? "sketch" : "filesystem";
    Serial.println("OTA start: " + type);
    oledMsg("OTA Update", "Starting...", type);
  });
  ArduinoOTA.onEnd([]() {
    Serial.println("\nOTA end");
    oledMsg("OTA Done", "Rebooting...");
  });
  ArduinoOTA.onProgress([](unsigned int progress, unsigned int total) {
    unsigned int pct = (progress / (total / 100));
    Serial.printf("OTA: %u%%\r", pct);
    char line[24]; snprintf(line, sizeof(line), "%u%%", pct);
    oledMsg("OTA Update", line, WiFi.localIP().toString());
  });
  ArduinoOTA.onError([](ota_error_t error) {
    Serial.printf("OTA error[%u]\n", error);
    oledMsg("OTA Error", String((int)error));
  });
  ArduinoOTA.begin();
  Serial.printf("OTA ready: %s @ %s\n", device_id, WiFi.localIP().toString().c_str());
}

void loop() {
  ArduinoOTA.handle();
  server.handleClient();
  if (!mqtt.connected()) mqttConnect();
  mqtt.loop();

  unsigned long now = millis();

  if (now - lastReadMs > READ_INTERVAL) {
    lastReadMs = now;
    float t = dht.readTemperature();
    float h = dht.readHumidity();
    if (!isnan(t) && !isnan(h)) { lastTemp = t; lastHum = h; }
    oledData();
  }

  if (!isnan(lastTemp) && now - lastPubMs > PUB_INTERVAL) {
    lastPubMs = now;
    publishMQTT(lastTemp, lastHum);
  }

  if (!isnan(lastTemp) && now - lastSheetMs > SHEET_INTERVAL) {
    lastSheetMs = now;
    postToSheet(lastTemp, lastHum);
  }
}
