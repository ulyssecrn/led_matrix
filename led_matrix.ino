// 64x64 HUB75 WiFi clock for the Huidu HD-WF1 (ESP32-S2).
// Library: "ESP32 HUB75 LED MATRIX PANEL DMA Display" (+ Adafruit GFX)
//
// - Time from NTP (written to the on-board RTC), RTC/build time as fallback
// - Web page at http://ledclock.local to set message, color, brightness
// - Firmware upload at http://ledclock.local/update (user admin, OTA_PASSWORD)

#include <Wire.h>
#include <sys/time.h>
#include <WiFi.h>
#include <WebServer.h>
#include <ESPmDNS.h>
#include <Update.h>
#include <Preferences.h>
#include <esp_sntp.h>
#include <ESP32-HUB75-MatrixPanel-I2S-DMA.h>
#include "secrets.h"

#define PANEL_W 64
#define PANEL_H 64

// HD-WF1 HUB75 pins (from mrfaptastic/HD-WF1-LED-MatrixPanel-DMA)
#define R1 2
#define G1 6
#define B1 3
#define R2 4
#define G2 8
#define B2 5
#define A  39
#define B  38
#define C  37
#define D  36
#define E  12   // needed for 1/32 scan (64 rows) panels
#define LAT 33
#define OE  35
#define CLK 34

// On-board BM8563 RTC (battery backed), stores local time
#define RTC_SDA  41
#define RTC_SCL  42
#define RTC_ADDR 0x51

#define HOSTNAME "ledclock"
#define TIMEZONE "EST5EDT,M3.2.0,M11.1.0"  // America/New_York

#define MSG_Y 52

MatrixPanel_I2S_DMA *display;
WebServer server(80);
Preferences prefs;

String message;
uint32_t msgColor;  // 0xRRGGBB
uint8_t brightness;

String infoText;          // shown instead of the message for a while (e.g. IP)
unsigned long infoUntil = 0;
volatile bool ntpSynced = false;
volatile int disconnectReason = 0;  // set from the WiFi event task

// Free heap (KB) at boot / after display init / after WiFi+web init
uint32_t heapBoot, heapDisplay, heapWifi;

uint8_t bcd2dec(uint8_t v) { return (v >> 4) * 10 + (v & 0x0F); }
uint8_t dec2bcd(uint8_t v) { return ((v / 10) << 4) | (v % 10); }

// Returns false if the RTC is unreachable or its time is invalid (battery was lost)
bool rtcRead(struct tm &t) {
  Wire.beginTransmission(RTC_ADDR);
  Wire.write(0x02);
  if (Wire.endTransmission(false) != 0) return false;
  if (Wire.requestFrom(RTC_ADDR, 7) != 7) return false;
  uint8_t s = Wire.read(), m = Wire.read(), h = Wire.read(), d = Wire.read();
  Wire.read();  // weekday
  uint8_t mo = Wire.read(), y = Wire.read();
  t = {};
  t.tm_sec  = bcd2dec(s & 0x7F);
  t.tm_min  = bcd2dec(m & 0x7F);
  t.tm_hour = bcd2dec(h & 0x3F);
  t.tm_mday = bcd2dec(d & 0x3F);
  t.tm_mon  = bcd2dec(mo & 0x1F) - 1;
  t.tm_year = 100 + bcd2dec(y);
  t.tm_isdst = -1;
  return !(s & 0x80);  // VL flag: low voltage, time not reliable
}

void rtcWrite(const struct tm &t) {
  Wire.beginTransmission(RTC_ADDR);
  Wire.write(0x00);
  Wire.write(0x00);  // control 1: clock running
  Wire.write(0x00);  // control 2
  Wire.write(dec2bcd(t.tm_sec));
  Wire.write(dec2bcd(t.tm_min));
  Wire.write(dec2bcd(t.tm_hour));
  Wire.write(dec2bcd(t.tm_mday));
  Wire.write(t.tm_wday);
  Wire.write(dec2bcd(t.tm_mon + 1));
  Wire.write(dec2bcd(t.tm_year % 100));
  Wire.endTransmission();
}

// Time the sketch was compiled (PC local time)
struct tm buildTime() {
  static const char months[] = "JanFebMarAprMayJunJulAugSepOctNovDec";
  char mon[4];
  struct tm t = {};
  int day, year;
  sscanf(__DATE__, "%3s %d %d", mon, &day, &year);
  sscanf(__TIME__, "%d:%d:%d", &t.tm_hour, &t.tm_min, &t.tm_sec);
  t.tm_mon = (strstr(months, mon) - months) / 3;
  t.tm_mday = day;
  t.tm_year = year - 1900;
  t.tm_isdst = -1;
  mktime(&t);  // fills tm_wday
  return t;
}

void setSystemTime(struct tm t) {
  struct timeval tv = {mktime(&t), 0};
  settimeofday(&tv, nullptr);
}

uint16_t rgb(uint32_t c) { return display->color565(c >> 16, (c >> 8) & 0xFF, c & 0xFF); }

String htmlEscape(const String &s) {
  String out;
  for (char c : s) {
    if (c == '&') out += "&amp;";
    else if (c == '<') out += "&lt;";
    else if (c == '"') out += "&quot;";
    else out += c;
  }
  return out;
}

void handleRoot() {
  char color[8];
  snprintf(color, sizeof(color), "#%06X", (unsigned)msgColor);
  String page =
    "<!doctype html><meta name=viewport content='width=device-width'>"
    "<title>LED clock</title><body style='font-family:sans-serif;max-width:400px;margin:2em auto'>"
    "<h2>LED clock</h2><form action=/set>"
    "<p>Message<br><input name=msg maxlength=100 style='width:100%' value=\"" + htmlEscape(message) + "\">"
    "<p>Color <input type=color name=color value='" + color + "'>"
    "<p>Brightness<br><input type=range name=bright min=1 max=255 style='width:100%' value=" + brightness + ">"
    "<p><button>Save</button></form>"
    "<p><a href=/update>Firmware update</a>";
  server.send(200, "text/html", page);
}

void handleSet() {
  if (server.hasArg("msg")) message = server.arg("msg");
  if (server.hasArg("color")) msgColor = strtoul(server.arg("color").c_str() + 1, nullptr, 16);
  if (server.hasArg("bright")) brightness = constrain(server.arg("bright").toInt(), 1, 255);
  prefs.putString("msg", message);
  prefs.putUInt("color", msgColor);
  prefs.putUChar("bright", brightness);
  display->setBrightness8(brightness);
  server.sendHeader("Location", "/");
  server.send(303);
}

void setupWeb() {
  server.on("/", handleRoot);
  server.on("/set", handleSet);
  server.on("/update", HTTP_GET, []() {
    if (!server.authenticate("admin", OTA_PASSWORD)) return server.requestAuthentication();
    server.send(200, "text/html",
      "<form method=POST enctype=multipart/form-data>"
      "<input type=file name=firmware accept=.bin> <button>Upload</button></form>");
  });
  server.on("/update", HTTP_POST, []() {
    if (!server.authenticate("admin", OTA_PASSWORD)) return server.requestAuthentication();
    bool ok = !Update.hasError();
    server.send(200, "text/plain", ok ? "OK, rebooting\n" : "Update failed\n");
    if (ok) {
      delay(500);
      ESP.restart();
    }
  }, []() {
    if (!server.authenticate("admin", OTA_PASSWORD)) return;
    HTTPUpload &up = server.upload();
    if (up.status == UPLOAD_FILE_START) Update.begin(UPDATE_SIZE_UNKNOWN);
    else if (up.status == UPLOAD_FILE_WRITE) Update.write(up.buf, up.currentSize);
    else if (up.status == UPLOAD_FILE_END) Update.end(true);
  });
  server.begin();
}

// The router's web UI answering means traffic really flows (the WiFi link
// can look "connected" while nothing gets through)
bool gatewayReachable() {
  WiFiClient c;
  bool ok = c.connect(WiFi.gatewayIP(), 80, 500);
  c.stop();
  return ok;
}

void rejoinWifi() {
  WiFi.disconnect();
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
}

// Shows IP/signal on (re)connect, the reason on disconnect, rejoins when
// offline for 30s or when traffic stops flowing.
// Top-right dot: green = ok, yellow = connected but no traffic, red = offline.
void checkWifi() {
  static bool connected = false, mdnsStarted = false;
  static int dot = -1, failedChecks = 0, rejoins = 0;
  static unsigned long lostSince = 0, lastCheck = 0;
  bool now = WiFi.status() == WL_CONNECTED;

  if (now && !connected) {
    if (!mdnsStarted) {
      mdnsStarted = MDNS.begin(HOSTNAME);
      MDNS.addService("http", "tcp", 80);
    }
    failedChecks = 0;
    lastCheck = millis();
    infoText = "IP " + WiFi.localIP().toString() + " " + WiFi.RSSI() + "dBm heap " +
               heapBoot + "/" + heapDisplay + "/" + heapWifi + "/" + (ESP.getFreeHeap() / 1024) +
               " min " + (ESP.getMinFreeHeap() / 1024);
    infoUntil = millis() + 20000;
  } else if (!now && connected) {
    lostSince = millis();
    infoText = "WiFi lost, reason " + String(disconnectReason);
    infoUntil = millis() + 20000;
  } else if (!now && millis() - lostSince > 30000) {
    lostSince = millis();
    rejoinWifi();
  } else if (now && millis() - lastCheck > 15000) {
    lastCheck = millis();
    failedChecks = gatewayReachable() ? 0 : failedChecks + 1;
    if (failedChecks >= 3) {
      failedChecks = 0;
      rejoins++;
      infoText = "No traffic, rejoin #" + String(rejoins);
      infoUntil = millis() + 20000;
      rejoinWifi();
      now = false;
    }
  }
  connected = now;

  int state = !now ? 0 : failedChecks ? 1 : 2;
  if (dot != state) {
    dot = state;
    uint16_t colors[] = {display->color565(255, 0, 0), display->color565(255, 200, 0),
                         display->color565(0, 255, 0)};
    display->fillRect(62, 0, 2, 2, colors[state]);
  }
}

void drawClock() {
  static int lastSec = -1;
  time_t now = time(nullptr);
  struct tm *t = localtime(&now);
  if (t->tm_sec == lastSec) return;
  lastSec = t->tm_sec;

  static const char *months[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun",
                                 "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
  uint16_t black = display->color565(0, 0, 0);
  char buf[12];

  // Drawn with a background color so no clearing/flicker is needed
  display->setTextSize(1);
  display->setTextColor(display->color565(180, 180, 180), black);
  snprintf(buf, sizeof(buf), "%2d %s", t->tm_mday, months[t->tm_mon]);
  display->setCursor(14, 4);
  display->print(buf);

  // HH:MM, 2x size: 5 chars * 12px = 60px
  display->setTextSize(2);
  display->setTextColor(display->color565(0, 200, 255), black);
  snprintf(buf, sizeof(buf), "%02d:%02d", t->tm_hour, t->tm_min);
  display->setCursor(3, 18);
  display->print(buf);

  display->setTextSize(1);
  display->setTextColor(display->color565(255, 120, 0), black);
  snprintf(buf, sizeof(buf), "%02d", t->tm_sec);
  display->setCursor(26, 38);
  display->print(buf);
}

// Bottom line: static if it fits, scrolling otherwise
void drawMessage() {
  static unsigned long lastStep = 0;
  static int x = PANEL_W;
  static String lastText;
  static uint32_t lastColor;
  bool info = millis() < infoUntil;
  String text = info ? infoText : message;
  uint32_t color = info ? 0xFFFFFF : msgColor;
  int width = text.length() * 6;

  if (text != lastText || color != lastColor) {
    lastText = text;
    lastColor = color;
    x = width <= PANEL_W ? (PANEL_W - width) / 2 : PANEL_W;
  } else if (width <= PANEL_W || millis() - lastStep < 40) {
    return;
  } else {
    x = x < -width ? PANEL_W : x - 1;
  }
  lastStep = millis();

  display->fillRect(0, MSG_Y, PANEL_W, 8, 0);
  display->setTextSize(1);
  display->setTextColor(rgb(color));
  display->setCursor(x, MSG_Y);
  display->print(text);
}

void timeSynced(struct timeval *) { ntpSynced = true; }

void setup() {
  heapBoot = ESP.getFreeHeap() / 1024;
  prefs.begin("clock");
  message = prefs.getString("msg", "Hello!");
  msgColor = prefs.getUInt("color", 0x00FF60);
  brightness = prefs.getUChar("bright", 80);

  HUB75_I2S_CFG::i2s_pins pins = {R1, G1, B1, R2, G2, B2, A, B, C, D, E, LAT, OE, CLK};
  HUB75_I2S_CFG cfg(PANEL_W, PANEL_H, 1, pins);
  cfg.clkphase = false;  // fixes image shifted by one column

  display = new MatrixPanel_I2S_DMA(cfg);
  display->begin();
  display->setBrightness8(brightness);
  display->setTextWrap(false);
  display->clearScreen();
  heapDisplay = ESP.getFreeHeap() / 1024;

  setenv("TZ", TIMEZONE, 1);
  tzset();

  // Use the RTC time, or the build time if the RTC is unset or behind it
  Wire.begin(RTC_SDA, RTC_SCL);
  struct tm rtc, build = buildTime();
  if (rtcRead(rtc) && mktime(&rtc) >= mktime(&build)) {
    setSystemTime(rtc);
  } else {
    rtcWrite(build);
    setSystemTime(build);
  }

  WiFi.setHostname(HOSTNAME);  // must come before WiFi.mode()
  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);  // power save made it drop off the network
  WiFi.onEvent([](WiFiEvent_t, WiFiEventInfo_t info) {
    disconnectReason = info.wifi_sta_disconnected.reason;
  }, ARDUINO_EVENT_WIFI_STA_DISCONNECTED);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);  // reconnects automatically
  sntp_set_time_sync_notification_cb(timeSynced);
  configTzTime(TIMEZONE, "pool.ntp.org", "time.google.com");
  setupWeb();
  heapWifi = ESP.getFreeHeap() / 1024;
}

void loop() {
  server.handleClient();
  checkWifi();
  if (ntpSynced) {
    ntpSynced = false;
    time_t now = time(nullptr);
    rtcWrite(*localtime(&now));
  }
  drawClock();
  drawMessage();
  delay(2);
}
