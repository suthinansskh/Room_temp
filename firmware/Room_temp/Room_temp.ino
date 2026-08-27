/*
 * Room Temperature Monitor
 * ESP8266 + DHT11 + OLED (SSD1306) + WiFiManager
 *  - HiveMQ Cloud (MQTT over TLS) + command channel (<base>/<device>/cmd)
 *  - Google Sheets logging (HTTPS POST to Apps Script Web App)
 *  - Local Web Server (JSON API + HTML config; Basic-auth on writes)
 *  - OLED status display with 1-px pixel-shift burn-in protection
 *  - Per-device temp/hum calibration offsets, persisted in EEPROM
 *  - DHT11 median-of-5 filter, sanity guard, and sensor watchdog
 *  - RTC-memory counters + /metrics endpoint
 *  - GPIO0 runtime: short-press = publish now, long-press 5s = wipe & reboot
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
#include <ESP8266WiFiMulti.h>
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

#define FW_VERSION "1.6.0"

// Optional compiled-in Wi-Fi defaults. Define any of these in secrets.h to
// pre-load known networks on every unit; leave undefined for portal-only setup.
// WiFiMulti auto-connects to whichever configured network is strongest/in range.
#ifndef DEFAULT_WIFI1_SSID
#define DEFAULT_WIFI1_SSID ""
#endif
#ifndef DEFAULT_WIFI1_PASS
#define DEFAULT_WIFI1_PASS ""
#endif
#ifndef DEFAULT_WIFI2_SSID
#define DEFAULT_WIFI2_SSID ""
#endif
#ifndef DEFAULT_WIFI2_PASS
#define DEFAULT_WIFI2_PASS ""
#endif
#ifndef DEFAULT_WIFI3_SSID
#define DEFAULT_WIFI3_SSID ""
#endif
#ifndef DEFAULT_WIFI3_PASS
#define DEFAULT_WIFI3_PASS ""
#endif

// Number of stored Wi-Fi networks tried at boot (slot 0 = primary/scan-picked).
#define WIFI_SLOTS 3

// ---------------- Pins ----------------
//   GPIO2  = D4 (also onboard LED on most dev boards)
//   GPIO4  = D2 (I2C SDA)
//   GPIO5  = D1 (I2C SCL)
//   GPIO0  = D3 / FLASH button — held LOW at boot opens the WiFiManager
//          portal; at runtime: short press = publish, 5s hold = wipe + reboot
#define DHT_PIN     2
#define DHT_TYPE    DHT11
#define I2C_SDA     4
#define I2C_SCL     5
#define BOOT_BTN    0

// ---------------- OLED ----------------
#define OLED_W      128
#define OLED_H      32
#define OLED_ADDR   0x3C
Adafruit_SSD1306 display(OLED_W, OLED_H, &Wire, -1);

// ---------------- Defaults / Config ----------------
char mqtt_server[64]   = DEFAULT_MQTT_HOST;
char mqtt_port_s[6]    = DEFAULT_MQTT_PORT;
char mqtt_user[40]     = DEFAULT_MQTT_USER;
char mqtt_pass[40]     = DEFAULT_MQTT_PASS;
char mqtt_base[40]     = DEFAULT_MQTT_BASE;        // topic prefix; device publishes to <base>/<device_id>
char gs_host[80]       = DEFAULT_GS_HOST;
char gs_path[140]      = DEFAULT_GS_PATH;
char project_id[24]    = DEFAULT_PROJECT;
char device_id[24]     = DEFAULT_DEVICE_ID;

// Up to WIFI_SLOTS saved networks. Slot 0 is the primary (set via the portal's
// Wi-Fi scanner); slots 1..2 are extras typed in the portal / /config page.
// WiFiMulti connects to whichever is in range with the strongest signal.
char wifi_ssid[WIFI_SLOTS][33] = { DEFAULT_WIFI1_SSID, DEFAULT_WIFI2_SSID, DEFAULT_WIFI3_SSID };
char wifi_pass[WIFI_SLOTS][65] = { DEFAULT_WIFI1_PASS, DEFAULT_WIFI2_PASS, DEFAULT_WIFI3_PASS };

// Per-device calibration. Added to raw DHT reading before publish/display.
// Editable in /config after comparing against a reference thermometer.
float temp_offset = 0.0f;
float hum_offset  = 0.0f;

// HTTP basic-auth password for /config, /reset, /metrics. Empty falls back
// to OTA_PASSWORD so first boot still works without an extra setup step.
char admin_pass[24] = "";
const char* ADMIN_USER = "admin";

// EEPROM layout. v2 adds calibration offsets + admin password. v1 magics
// are migrated forward in place by zero-filling the new fields.
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
  // v2 additions:
  float temp_offset;
  float hum_offset;
  char  admin_pass[24];
  // v3 additions:
  char  wifi_ssid[WIFI_SLOTS][33];
  char  wifi_pass[WIFI_SLOTS][65];
} cfg;
const uint32_t CFG_MAGIC_V1 = 0xC0FFEE05;
const uint32_t CFG_MAGIC_V2 = 0xC0FFEE06;
const uint32_t CFG_MAGIC_V3 = 0xC0FFEE07;

// ESP8266 has ~512 B of RTC user memory that survives soft reboots
// (not power-cycle). Used for boot/reconnect/fault counters so we don't
// burn flash writes on telemetry that doesn't need to persist forever.
struct Stats {
  uint32_t magic;
  uint32_t boots;
  uint32_t mqttReconnects;
  uint32_t sensorFaults;
};
const uint32_t STATS_MAGIC = 0x53544154;  // 'STAT'
Stats stats;

void statsLoad() {
  ESP.rtcUserMemoryRead(0, (uint32_t*)&stats, sizeof(stats));
  if (stats.magic != STATS_MAGIC) {
    stats.magic = STATS_MAGIC;
    stats.boots = 0;
    stats.mqttReconnects = 0;
    stats.sensorFaults = 0;
  }
}
void statsSave() {
  ESP.rtcUserMemoryWrite(0, (uint32_t*)&stats, sizeof(stats));
}

// strncpy leaves the destination unterminated when the source fills the
// buffer, and EEPROM content can be garbage even when the magic matches.
// Every config copy goes through this instead.
void copyStr(char* dst, const char* src, size_t size) {
  if (!size) return;
  strncpy(dst, src, size - 1);
  dst[size - 1] = '\0';
}

void saveConfig();   // forward decl: loadConfig calls saveConfig on v1→v2 migration

void loadConfig() {
  EEPROM.begin(sizeof(Config));
  EEPROM.get(0, cfg);
  EEPROM.end();
  bool migrated = false;
  if (cfg.magic == CFG_MAGIC_V1) {
    cfg.temp_offset = 0.0f;
    cfg.hum_offset  = 0.0f;
    cfg.admin_pass[0] = '\0';
    migrated = true;
  }
  if (cfg.magic == CFG_MAGIC_V1 || cfg.magic == CFG_MAGIC_V2) {
    // v3 adds the Wi-Fi slots. Seed them from the compiled-in secrets.h
    // defaults so an OTA upgrade keeps any known networks baked into firmware.
    memcpy(cfg.wifi_ssid, wifi_ssid, sizeof(cfg.wifi_ssid));
    memcpy(cfg.wifi_pass, wifi_pass, sizeof(cfg.wifi_pass));
    migrated = true;
  }
  if (cfg.magic == CFG_MAGIC_V1 || cfg.magic == CFG_MAGIC_V2 || cfg.magic == CFG_MAGIC_V3) {
    copyStr(mqtt_server, cfg.mqtt_server, sizeof(mqtt_server));
    copyStr(mqtt_port_s, cfg.mqtt_port_s, sizeof(mqtt_port_s));
    copyStr(mqtt_user,   cfg.mqtt_user,   sizeof(mqtt_user));
    copyStr(mqtt_pass,   cfg.mqtt_pass,   sizeof(mqtt_pass));
    copyStr(mqtt_base,   cfg.mqtt_base,   sizeof(mqtt_base));
    copyStr(gs_host,     cfg.gs_host,     sizeof(gs_host));
    copyStr(gs_path,     cfg.gs_path,     sizeof(gs_path));
    copyStr(project_id,  cfg.project_id,  sizeof(project_id));
    copyStr(device_id,   cfg.device_id,   sizeof(device_id));
    temp_offset = cfg.temp_offset;
    hum_offset  = cfg.hum_offset;
    copyStr(admin_pass,  cfg.admin_pass,  sizeof(admin_pass));
    memcpy(wifi_ssid, cfg.wifi_ssid, sizeof(wifi_ssid));
    memcpy(wifi_pass, cfg.wifi_pass, sizeof(wifi_pass));
  }
  if (migrated) saveConfig();
}

void saveConfig() {
  cfg.magic = CFG_MAGIC_V3;
  copyStr(cfg.mqtt_server, mqtt_server, sizeof(cfg.mqtt_server));
  copyStr(cfg.mqtt_port_s, mqtt_port_s, sizeof(cfg.mqtt_port_s));
  copyStr(cfg.mqtt_user,   mqtt_user,   sizeof(cfg.mqtt_user));
  copyStr(cfg.mqtt_pass,   mqtt_pass,   sizeof(cfg.mqtt_pass));
  copyStr(cfg.mqtt_base,   mqtt_base,   sizeof(cfg.mqtt_base));
  copyStr(cfg.gs_host,     gs_host,     sizeof(cfg.gs_host));
  copyStr(cfg.gs_path,     gs_path,     sizeof(cfg.gs_path));
  copyStr(cfg.project_id,  project_id,  sizeof(cfg.project_id));
  copyStr(cfg.device_id,   device_id,   sizeof(cfg.device_id));
  cfg.temp_offset = temp_offset;
  cfg.hum_offset  = hum_offset;
  copyStr(cfg.admin_pass,  admin_pass,  sizeof(cfg.admin_pass));
  memcpy(cfg.wifi_ssid, wifi_ssid, sizeof(cfg.wifi_ssid));
  memcpy(cfg.wifi_pass, wifi_pass, sizeof(cfg.wifi_pass));
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
ESP8266WiFiMulti wifiMulti;

float lastTemp = NAN;
float lastHum  = NAN;
unsigned long lastReadMs      = 0;
unsigned long lastPubMs       = 0;
unsigned long lastSheetMs     = 0;
unsigned long lastReconnMs    = 0;
unsigned long lastValidReadMs = 0;
unsigned long lastWifiOkMs    = 0;   // for runtime Wi-Fi failover throttling
unsigned long lastShiftMs     = 0;
bool sensorReinited = false;
bool needPublishNow = false;
bool needReboot     = false;
bool needWifiWipe   = false;

const unsigned long READ_INTERVAL    = 2000;        // 2s
const unsigned long PUB_INTERVAL     = 10000;       // 10s MQTT
const unsigned long SHEET_INTERVAL   = 1800000UL;   // 30 min Google Sheets
const unsigned long SHIFT_INTERVAL   = 60000UL;     // 1 min OLED Y-shift toggle
const unsigned long FAULT_REINIT_MS  = 300000UL;    // 5 min NaN → dht.begin()
const unsigned long FAULT_REBOOT_MS  = 1800000UL;   // 30 min NaN → ESP.restart()

// DHT11 ring buffer for median-of-5 filtering.
const uint8_t HIST_N = 5;
float tHist[HIST_N], hHist[HIST_N];
uint8_t histIdx = 0, histCount = 0;

// Vertical pixel shift for OLED anti-burn. Cycles 0 ↔ 1 once per minute.
int yShift = 0;

// ---------------- OLED helpers ----------------
void oledMsg(const String& l1, const String& l2 = "", const String& l3 = "") {
  display.clearDisplay();
  display.setTextSize(1);
  display.setTextColor(SSD1306_WHITE);
  display.setCursor(0, yShift);       display.println(l1);
  display.setCursor(0, 12 + yShift);  display.println(l2);
  display.setCursor(0, 24 + yShift);  display.println(l3);
  display.display();
}

void oledData() {
  display.clearDisplay();
  display.setTextColor(SSD1306_WHITE);
  display.setTextSize(1);
  display.setCursor(0, yShift);
  display.print(device_id);
  display.print(' ');
  if (WiFi.status() == WL_CONNECTED) {
    display.print(WiFi.localIP());
  } else {
    display.print(mqtt.connected() ? "MQTT:OK" : "...");
  }
  display.setTextSize(2);
  display.setCursor(0, 14 + yShift);
  if (isnan(lastTemp)) display.print("--.-");
  else                 display.print(lastTemp, 1);
  display.print((char)247);
  display.print('C');
  display.setTextSize(2);
  display.setCursor(80, 14 + yShift);
  if (isnan(lastHum)) display.print("--%");
  else { display.print(lastHum, 0); display.print('%'); }
  display.display();
}

bool sensorFaultActive = false;   // a sensor_fault ack is outstanding

// <base>/<device>/status is the LWT channel: only "online"/"offline" ever go
// here, retained, so a subscriber can treat any other payload as a bug.
void publishStatus(const char* state) {
  if (!mqtt.connected()) return;
  char topic[80];
  snprintf(topic, sizeof(topic), "%s/%s/status", mqtt_base, device_id);
  mqtt.publish(topic, state, true);
}

// <base>/<device>/ack carries command acks and sensor events. Not retained:
// these are events, and a retained one would outlive the condition it reports.
void publishAck(const char* msg) {
  if (!mqtt.connected()) return;
  char topic[80];
  snprintf(topic, sizeof(topic), "%s/%s/ack", mqtt_base, device_id);
  mqtt.publish(topic, msg, false);
}

// ---------------- Sensor reading ----------------
// Small insertion sort on the live samples; returns the middle one.
float median5(const float* arr, uint8_t n) {
  float a[HIST_N];
  for (uint8_t i = 0; i < n; i++) a[i] = arr[i];
  for (uint8_t i = 1; i < n; i++) {
    float v = a[i]; int j = i - 1;
    while (j >= 0 && a[j] > v) { a[j+1] = a[j]; j--; }
    a[j+1] = v;
  }
  return a[n / 2];
}

// Read DHT once, apply sanity guard + calibration offset, push into the
// median ring. Returns true if the sample was usable.
bool readSensorSample() {
  float t = dht.readTemperature();
  float h = dht.readHumidity();
  if (isnan(t) || isnan(h)) return false;
  if (t < -10.0f || t > 60.0f) return false;
  if (h < 0.0f   || h > 100.0f) return false;
  t += temp_offset;
  h += hum_offset;
  tHist[histIdx] = t;
  hHist[histIdx] = h;
  histIdx = (histIdx + 1) % HIST_N;
  if (histCount < HIST_N) histCount++;
  lastTemp = median5(tHist, histCount);
  lastHum  = median5(hHist, histCount);
  lastValidReadMs = millis();
  sensorReinited = false;
  if (sensorFaultActive) {
    // The retained status may still say the device was faulty (older firmware
    // published sensor_fault there); re-assert online now that data is flowing.
    sensorFaultActive = false;
    publishAck("sensor_ok");
    publishStatus("online");
  }
  return true;
}

// If no valid reading for FAULT_REINIT_MS, re-init the bus and publish a
// status flag. If still nothing by FAULT_REBOOT_MS, restart.
void sensorWatchdog() {
  // lastValidReadMs is seeded at boot, so a sensor that never produced a
  // single reading still trips the re-init and the reboot below.
  unsigned long age = millis() - lastValidReadMs;
  if (age >= FAULT_REBOOT_MS) {
    Serial.println("Sensor dead > 30 min, restarting");
    oledMsg("Sensor fault", "Restarting...");
    delay(400);
    ESP.restart();
  }
  if (age >= FAULT_REINIT_MS && !sensorReinited) {
    Serial.println("Sensor NaN > 5 min, re-init dht");
    stats.sensorFaults++;
    statsSave();
    sensorFaultActive = true;
    publishAck("sensor_fault");
    dht.begin();
    sensorReinited = true;
  }
}

// ---------------- Wi-Fi / Portal ----------------
// Register every non-empty saved network with WiFiMulti, plus whatever the
// SDK persisted (covers pre-1.5 firmware where the slots may be blank but a
// primary AP is still stored in flash). WiFiMulti then connects to the
// strongest of them and can fail over between them.
void buildWifiList() {
  for (uint8_t i = 0; i < WIFI_SLOTS; i++) {
    if (wifi_ssid[i][0]) wifiMulti.addAP(wifi_ssid[i], wifi_pass[i]);
  }
  String saved = WiFi.SSID();
  if (saved.length()) wifiMulti.addAP(saved.c_str(), WiFi.psk().c_str());
}

// Runs the WiFiManager captive portal (always in config mode). The built-in
// scanner picks the primary network; extra networks are typed into custom
// fields. Blocks until the user saves or the portal times out.
void startPortal() {
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
  WiFiManagerParameter p_w2_ssid    ("w2s",   "Wi-Fi #2 SSID (optional)", wifi_ssid[1], 32);
  WiFiManagerParameter p_w2_pass    ("w2p",   "Wi-Fi #2 password",        wifi_pass[1], 64);
  WiFiManagerParameter p_w3_ssid    ("w3s",   "Wi-Fi #3 SSID (optional)", wifi_ssid[2], 32);
  WiFiManagerParameter p_w3_pass    ("w3p",   "Wi-Fi #3 password",        wifi_pass[2], 64);

  wm.addParameter(&p_mqtt_server);
  wm.addParameter(&p_mqtt_port);
  wm.addParameter(&p_mqtt_user);
  wm.addParameter(&p_mqtt_pass);
  wm.addParameter(&p_mqtt_base);
  wm.addParameter(&p_gs_host);
  wm.addParameter(&p_gs_path);
  wm.addParameter(&p_proj);
  wm.addParameter(&p_dev);
  wm.addParameter(&p_w2_ssid);
  wm.addParameter(&p_w2_pass);
  wm.addParameter(&p_w3_ssid);
  wm.addParameter(&p_w3_pass);

  oledMsg("WiFi Setup", "AP: RoomTemp-Setup", "192.168.4.1");

  // Never park in the portal forever: if nobody configures the device within
  // 3 minutes, reboot and retry the saved networks (the AP may just be down).
  wm.setConfigPortalTimeout(180);

  // Always config mode: ensureWifi() only calls us when we actually need the
  // portal (multi-connect failed or the FLASH button forced it).
  bool ok = wm.startConfigPortal("RoomTemp-Setup");
  if (!ok) {
    oledMsg("WiFi timeout", "Retrying...");
    delay(1500); ESP.restart();
  }

  if (shouldSaveConfig) {
    copyStr(mqtt_server, p_mqtt_server.getValue(), sizeof(mqtt_server));
    copyStr(mqtt_port_s, p_mqtt_port.getValue(),   sizeof(mqtt_port_s));
    copyStr(mqtt_user,   p_mqtt_user.getValue(),   sizeof(mqtt_user));
    copyStr(mqtt_pass,   p_mqtt_pass.getValue(),   sizeof(mqtt_pass));
    copyStr(mqtt_base,   p_mqtt_base.getValue(),   sizeof(mqtt_base));
    copyStr(gs_host,     p_gs_host.getValue(),     sizeof(gs_host));
    copyStr(gs_path,     p_gs_path.getValue(),     sizeof(gs_path));
    copyStr(project_id,  p_proj.getValue(),        sizeof(project_id));
    copyStr(device_id,   p_dev.getValue(),         sizeof(device_id));
    copyStr(wifi_ssid[1], p_w2_ssid.getValue(), sizeof(wifi_ssid[1]));
    copyStr(wifi_pass[1], p_w2_pass.getValue(), sizeof(wifi_pass[1]));
    copyStr(wifi_ssid[2], p_w3_ssid.getValue(), sizeof(wifi_ssid[2]));
    copyStr(wifi_pass[2], p_w3_pass.getValue(), sizeof(wifi_pass[2]));
  }

  // The portal just associated with the primary AP; capture those creds into
  // slot 0 so WiFiMulti can auto-connect to it (or fail over) next boot.
  String primarySsid = WiFi.SSID();
  if (primarySsid.length()) {
    copyStr(wifi_ssid[0], primarySsid.c_str(), sizeof(wifi_ssid[0]));
    String primaryPsk = WiFi.psk();
    copyStr(wifi_pass[0], primaryPsk.c_str(), sizeof(wifi_pass[0]));
  }
  saveConfig();
}

// Bring Wi-Fi up. Normal boot: try every saved network via WiFiMulti and
// return once connected. If none are reachable (or the FLASH button forced
// it), fall back to the captive portal.
void ensureWifi(bool forcePortal) {
  if (!forcePortal) {
    WiFi.mode(WIFI_STA);
    buildWifiList();
    oledMsg("WiFi", "Connecting...");
    unsigned long t0 = millis();
    while (millis() - t0 < 25000UL) {
      if (wifiMulti.run() == WL_CONNECTED) return;
      delay(300);
    }
    if (WiFi.status() == WL_CONNECTED) return;
    Serial.println("No known Wi-Fi in range; opening portal");
  }
  startPortal();
  buildWifiList();   // refresh list with any networks changed in the portal
}

// ---------------- MQTT ----------------
// Commands on <base>/<device>/cmd (retained=false recommended on publisher):
//   "read"             -> publish a reading immediately
//   "reboot"           -> ESP.restart() at top of next loop turn
//   "offset <T> <H>"   -> set calibration offsets (float, float), save, ack
//   anything else      -> ignored
void mqttCallback(char* topic, byte* payload, unsigned int len) {
  char body[48] = {0};
  size_t n = len < sizeof(body) - 1 ? len : sizeof(body) - 1;
  memcpy(body, payload, n);
  Serial.printf("MQTT cmd %s -> %s\n", topic, body);

  if (!strcmp(body, "read")) {
    needPublishNow = true;
  } else if (!strcmp(body, "reboot")) {
    needReboot = true;
  } else if (!strncmp(body, "offset", 6)) {
    float t = 0, h = 0;
    if (sscanf(body + 6, "%f %f", &t, &h) == 2) {
      temp_offset = t;
      hum_offset  = h;
      saveConfig();
      char ack[64];
      snprintf(ack, sizeof(ack), "offset t=%.2f h=%.2f", t, h);
      publishAck(ack);
    }
  }
}

void mqttConnect() {
  if (mqtt.connected()) return;
  if (millis() - lastReconnMs < 5000) return;
  lastReconnMs = millis();

  mqtt.setServer(mqtt_server, atoi(mqtt_port_s));

  char cid[48];
  snprintf(cid, sizeof(cid), "esp-%s-%08x", device_id, ESP.getChipId());
  char willTopic[80];
  snprintf(willTopic, sizeof(willTopic), "%s/%s/status", mqtt_base, device_id);

  Serial.printf("MQTT connect to %s:%s as %s ...\n", mqtt_server, mqtt_port_s, cid);
  if (mqtt.connect(cid, mqtt_user, mqtt_pass, willTopic, 0, true, "offline")) {
    Serial.println("MQTT connected");
    publishStatus("online");
    char cmdTopic[80];
    snprintf(cmdTopic, sizeof(cmdTopic), "%s/%s/cmd", mqtt_base, device_id);
    mqtt.subscribe(cmdTopic);
    stats.mqttReconnects++;
    statsSave();
  } else {
    Serial.printf("MQTT failed rc=%d\n", mqtt.state());
  }
}

void publishMQTT(float t, float h) {
  if (!mqtt.connected()) return;
  IPAddress ip = WiFi.localIP();
  char ipbuf[16];
  snprintf(ipbuf, sizeof(ipbuf), "%u.%u.%u.%u", ip[0], ip[1], ip[2], ip[3]);
  char buf[224];
  int n = snprintf(buf, sizeof(buf),
    "{\"device\":\"%s\",\"temp\":%.1f,\"hum\":%.0f,\"rssi\":%d,\"ip\":\"%s\",\"uptime\":%lu,\"fw\":\"%s\"}",
    device_id, t, h, WiFi.RSSI(), ipbuf, (unsigned long)(millis() / 1000), FW_VERSION);
  if (n < 0 || n >= (int)sizeof(buf)) return;
  char topic[80];
  snprintf(topic, sizeof(topic), "%s/%s", mqtt_base, device_id);
  mqtt.publish(topic, (const uint8_t*)buf, n, true);   // retained: latest reading per device
  Serial.printf("MQTT pub %s: %s\n", topic, buf);
}

// ---------------- Google Sheets ----------------
// Apps Script /exec returns a 302 to script.googleusercontent.com. The
// ESP8266 HTTPClient won't re-POST across hosts, so we follow redirects
// manually. Each hop is its own TLS handshake (~1–2 s); pumping the rest
// of the loop between hops keeps MQTT/web/OTA alive.
void pumpLoops() {
  mqtt.loop();
  server.handleClient();
  ArduinoOTA.handle();
}

void postToSheet(float t, float h) {
  StaticJsonDocument<256> doc;
  doc["project"] = project_id;
  doc["device"]  = device_id;
  doc["temp"]    = t;
  doc["hum"]     = h;
  doc["rssi"]    = WiFi.RSSI();
  doc["ip"]      = WiFi.localIP().toString();
  doc["uptime"]  = (uint32_t)(millis() / 1000);
  doc["fw"]      = FW_VERSION;
  String body; serializeJson(doc, body);

  String host = gs_host;
  String path = gs_path;

  for (int hop = 0; hop < 4; hop++) {
    pumpLoops();
    WiFiClientSecure client;
    client.setInsecure();
    client.setBufferSizes(4096, 512);

    HTTPClient https;
    String url = String("https://") + host + path;
    if (!https.begin(client, url)) {
      Serial.printf("GS begin fail (hop %d) %s\n", hop, url.c_str());
      return;
    }
    https.addHeader("Content-Type", "application/json");
    const char* collect[] = { "Location" };
    https.collectHeaders(collect, 1);

    int code = https.POST(body);
    Serial.printf("GS POST %d (hop %d) %s%s\n", code, hop, host.c_str(), path.c_str());

    if (code == 301 || code == 302 || code == 303 || code == 307 || code == 308) {
      String loc = https.header("Location");
      https.end();
      if (!loc.startsWith("https://")) {
        Serial.printf("GS redirect not https: '%s'\n", loc.c_str());
        return;
      }
      int slash = loc.indexOf('/', 8);
      if (slash < 0) { Serial.println("GS redirect URL has no path"); return; }
      host = loc.substring(8, slash);
      path = loc.substring(slash);
      continue;
    }

    if (code > 0 && code < 400) {
      Serial.printf("GS body: %s\n", https.getString().c_str());
    }
    https.end();
    return;
  }
  Serial.println("GS too many redirects");
}

// ---------------- Web Server ----------------
#include "index_html.h"

const char* adminPassActive() {
  if (admin_pass[0]) return admin_pass;
#ifdef OTA_PASSWORD
  return OTA_PASSWORD;
#else
  return "admin";
#endif
}

bool requireAuth() {
  if (!server.authenticate(ADMIN_USER, adminPassActive())) {
    server.requestAuthentication();
    return false;
  }
  return true;
}

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
  StaticJsonDocument<224> doc;
  doc["device"] = device_id;
  if (isnan(lastTemp)) doc["temp"] = nullptr; else doc["temp"] = lastTemp;
  if (isnan(lastHum))  doc["hum"]  = nullptr; else doc["hum"]  = lastHum;
  doc["rssi"]   = WiFi.RSSI();
  doc["ip"]     = WiFi.localIP().toString();
  doc["uptime"] = (uint32_t)(millis() / 1000);
  doc["mqtt"]   = mqtt.connected();
  doc["fault"]  = sensorFaultActive;
  doc["fw"]     = FW_VERSION;
  String s; serializeJson(doc, s);
  server.send(200, "application/json", s);
}

void handleMetrics() {
  if (!requireAuth()) return;
  StaticJsonDocument<256> doc;
  doc["boots"]            = stats.boots;
  doc["mqtt_reconnects"]  = stats.mqttReconnects;
  doc["sensor_faults"]    = stats.sensorFaults;
  doc["free_heap"]        = ESP.getFreeHeap();
  doc["max_free_block"]   = ESP.getMaxFreeBlockSize();
  doc["heap_frag"]        = ESP.getHeapFragmentation();
  doc["uptime"]           = (uint32_t)(millis() / 1000);
  doc["fw"]               = FW_VERSION;
  doc["last_valid_age"]   = lastValidReadMs ? (uint32_t)((millis() - lastValidReadMs) / 1000) : 0;
  String s; serializeJson(doc, s);
  server.send(200, "application/json", s);
}

void handleConfigPage() {
  if (!requireAuth()) return;
  String html;
  html.reserve(8200);
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
  html += F("</code> &middot; fw <code>" FW_VERSION "</code></p><form method='post' action='/config/save'><div class='grid'>");
  html += F("<label>Device name<input name='device_id' maxlength='23' value='"); html += htmlEscape(device_id); html += F("'></label>");
  html += F("<label>Project<input name='project_id' maxlength='23' value='"); html += htmlEscape(project_id); html += F("'></label>");
  html += F("<label>MQTT host<input name='mqtt_server' maxlength='63' value='"); html += htmlEscape(mqtt_server); html += F("'></label>");
  html += F("<label>MQTT port<input name='mqtt_port_s' maxlength='5' value='"); html += htmlEscape(mqtt_port_s); html += F("'></label>");
  html += F("<label>MQTT user<input name='mqtt_user' maxlength='39' value='"); html += htmlEscape(mqtt_user); html += F("'></label>");
  html += F("<label>MQTT password<small>Blank = keep current</small><input name='mqtt_pass' maxlength='39' type='password'></label>");
  html += F("<label>MQTT base topic<input name='mqtt_base' maxlength='39' value='"); html += htmlEscape(mqtt_base); html += F("'></label>");
  html += F("<label>Google Script host<input name='gs_host' maxlength='79' value='"); html += htmlEscape(gs_host); html += F("'></label>");
  html += F("</div><label>Google Script path<input name='gs_path' maxlength='139' value='"); html += htmlEscape(gs_path); html += F("'></label>");
  html += F("<p style='color:var(--text);margin:14px 0 0'>Wi-Fi networks <small style='color:var(--mut)'>&mdash; auto-connects to the strongest in range</small></p><div class='grid'>");
  char wn[8];
  for (uint8_t i = 0; i < WIFI_SLOTS; i++) {
    html += F("<label>SSID #"); html += String(i + 1);
    snprintf(wn, sizeof(wn), "w%u_ssid", i);
    html += F("<input name='"); html += wn; html += F("' maxlength='32' value='");
    html += htmlEscape(wifi_ssid[i]); html += F("'></label>");
    html += F("<label>Password #"); html += String(i + 1);
    html += F("<small>Blank = keep current</small>");
    snprintf(wn, sizeof(wn), "w%u_pass", i);
    html += F("<input name='"); html += wn; html += F("' maxlength='64' type='password'></label>");
  }
  html += F("</div>");
  char off[16];
  html += F("<div class='grid'>");
  snprintf(off, sizeof(off), "%.2f", temp_offset);
  html += F("<label>Temp offset (°C)<small>Added to raw DHT reading</small><input name='temp_offset' value='"); html += off; html += F("'></label>");
  snprintf(off, sizeof(off), "%.2f", hum_offset);
  html += F("<label>Humidity offset (%)<input name='hum_offset' value='"); html += off; html += F("'></label>");
  html += F("<label>Admin password<small>Blank = keep current (defaults to OTA password)</small><input name='admin_pass' maxlength='23' type='password'></label>");
  html += F("</div><div class='actions'><button type='submit'>Save and restart</button><a class='btn' href='/'>Dashboard</a><a class='btn' href='/api'>API</a><a class='btn' href='/metrics'>Metrics</a></div>");
  html += F("</form></div></main></body></html>");
  server.send(200, "text/html", html);
}

void handleConfigSave() {
  if (!requireAuth()) return;
  copyArg(device_id,  sizeof(device_id),  "device_id");
  copyArg(project_id, sizeof(project_id), "project_id");
  copyArg(mqtt_server, sizeof(mqtt_server), "mqtt_server");
  copyArg(mqtt_port_s, sizeof(mqtt_port_s), "mqtt_port_s");
  copyArg(mqtt_user,   sizeof(mqtt_user),   "mqtt_user");
  if (server.hasArg("mqtt_pass") && server.arg("mqtt_pass").length() > 0) {
    server.arg("mqtt_pass").toCharArray(mqtt_pass, sizeof(mqtt_pass));
  }
  copyArg(mqtt_base, sizeof(mqtt_base), "mqtt_base");
  copyArg(gs_host,   sizeof(gs_host),   "gs_host");
  copyArg(gs_path,   sizeof(gs_path),   "gs_path");
  if (server.hasArg("temp_offset")) temp_offset = server.arg("temp_offset").toFloat();
  if (server.hasArg("hum_offset"))  hum_offset  = server.arg("hum_offset").toFloat();
  if (server.hasArg("admin_pass") && server.arg("admin_pass").length() > 0) {
    server.arg("admin_pass").toCharArray(admin_pass, sizeof(admin_pass));
  }
  char wn[8];
  for (uint8_t i = 0; i < WIFI_SLOTS; i++) {
    snprintf(wn, sizeof(wn), "w%u_ssid", i);
    copyArg(wifi_ssid[i], sizeof(wifi_ssid[i]), wn);   // SSID: blank clears the slot
    snprintf(wn, sizeof(wn), "w%u_pass", i);
    if (server.hasArg(wn) && server.arg(wn).length() > 0) {
      server.arg(wn).toCharArray(wifi_pass[i], sizeof(wifi_pass[i]));
    }
  }
  saveConfig();
  server.send(200, "text/html", "<!doctype html><meta name='viewport' content='width=device-width,initial-scale=1'><body style='font-family:system-ui;background:#0f1218;color:#edf1f5;padding:2rem'><h1>Saved</h1><p>Restarting device...</p></body>");
  delay(600);
  ESP.restart();
}

void handleReset() {
  if (!requireAuth()) return;
  server.send(200, "text/plain", "Erasing Wi-Fi & restarting...");
  delay(300);
  WiFi.disconnect(true);
  ESP.restart();
}

// ---------------- Button (runtime) ----------------
// Short press (50ms..2s) -> trigger an immediate publish.
// Long  press (>=5s)     -> wipe Wi-Fi credentials and reboot into portal.
void serviceButton() {
  static int prev = HIGH;
  static unsigned long downAt = 0;
  int v = digitalRead(BOOT_BTN);
  unsigned long now = millis();
  if (v == LOW && prev == HIGH) {
    downAt = now;
  } else if (v == LOW && prev == LOW) {
    if (downAt && now - downAt >= 5000) {
      needWifiWipe = true;
      downAt = 0;                          // arm only once per press
    }
  } else if (v == HIGH && prev == LOW) {
    if (downAt) {
      unsigned long held = now - downAt;
      if (held > 50 && held < 2000) needPublishNow = true;
    }
    downAt = 0;
  }
  prev = v;
}

// ---------------- Setup / Loop ----------------
void setup() {
  Serial.begin(115200);
  delay(200);
  Serial.println("\n=== Room Temp ESP8266 fw " FW_VERSION " ===");

  Wire.begin(I2C_SDA, I2C_SCL);
  if (!display.begin(SSD1306_SWITCHCAPVCC, OLED_ADDR)) {
    Serial.println("OLED init failed");
  }
  display.clearDisplay(); display.display();
  oledMsg("Booting...", "Room Temp", FW_VERSION);

  dht.begin();
  loadConfig();

  statsLoad();
  stats.boots++;
  statsSave();

  pinMode(BOOT_BTN, INPUT_PULLUP);
  bool force = (digitalRead(BOOT_BTN) == LOW);
  ensureWifi(force);

  oledMsg("WiFi OK", WiFi.SSID(), WiFi.localIP().toString());
  Serial.print("IP: "); Serial.println(WiFi.localIP());

  secureClient.setInsecure();
  // BearSSL takes a 16 KB receive buffer by default, which has to coexist with
  // the second TLS session postToSheet() opens. If the broker supports RFC 6066
  // max-fragment-length we can run the MQTT session on ~1 KB instead.
  if (secureClient.probeMaxFragmentLength(mqtt_server, atoi(mqtt_port_s), 1024)) {
    secureClient.setBufferSizes(1024, 512);
    Serial.println("MQTT TLS: MFLN 1024 negotiated");
  }
  mqtt.setBufferSize(512);
  // postToSheet() blocks for several seconds per TLS hop and cannot pump
  // mqtt.loop() while it does. PubSubClient defaults to a 15 s keepalive, so
  // the broker would drop us mid-POST and fire the LWT; 90 s leaves headroom.
  mqtt.setKeepAlive(90);
  mqtt.setCallback(mqttCallback);

  // Seed the sensor watchdog so a DHT that is dead from power-on still trips it.
  lastValidReadMs = millis();

  server.on("/",            handleRoot);
  server.on("/api",         handleApi);
  server.on("/metrics",     handleMetrics);
  server.on("/config",      HTTP_GET,  handleConfigPage);
  server.on("/config/save", HTTP_POST, handleConfigSave);
  server.on("/reset",       HTTP_POST, handleReset);
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
    unsigned int pct = total ? (unsigned int)((uint64_t)progress * 100 / total) : 0;
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
  serviceButton();
  if (!mqtt.connected()) mqttConnect();
  mqtt.loop();

  unsigned long now = millis();

  // Runtime Wi-Fi failover: the SDK auto-reconnects to the same AP, but if
  // that AP stays down for 30 s, re-run WiFiMulti to switch to another saved
  // network that's in range. Throttled so the blocking scan runs at most
  // once every 30 s while disconnected.
  if (WiFi.status() == WL_CONNECTED) {
    lastWifiOkMs = now;
  } else if (now - lastWifiOkMs > 30000UL) {
    lastWifiOkMs = now;
    wifiMulti.run();
  }

  if (now - lastShiftMs > SHIFT_INTERVAL) {
    lastShiftMs = now;
    yShift = (yShift == 0) ? 1 : 0;
  }

  if (now - lastReadMs > READ_INTERVAL) {
    lastReadMs = now;
    readSensorSample();
    oledData();
  }

  sensorWatchdog();

  if (!isnan(lastTemp) && (needPublishNow || now - lastPubMs > PUB_INTERVAL)) {
    lastPubMs = now;
    needPublishNow = false;
    publishMQTT(lastTemp, lastHum);
  }

  // First valid reading triggers an immediate POST; thereafter every SHEET_INTERVAL.
  if (!isnan(lastTemp) && (lastSheetMs == 0 || now - lastSheetMs > SHEET_INTERVAL)) {
    lastSheetMs = now;
    postToSheet(lastTemp, lastHum);
  }

  // Deferred actions — handled here so we never reboot mid-publish.
  if (needWifiWipe) {
    Serial.println("Long-press: wiping Wi-Fi credentials");
    oledMsg("WiFi reset", "Long press", "Rebooting...");
    WiFi.disconnect(true);
    delay(400);
    ESP.restart();
  }
  if (needReboot) {
    Serial.println("MQTT cmd: reboot");
    oledMsg("Reboot", "MQTT cmd");
    delay(400);
    ESP.restart();
  }
}
