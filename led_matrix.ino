// 64x64 HUB75 WiFi clock for the Huidu HD-WF1 (ESP32-S2).
// Library: "ESP32 HUB75 LED MATRIX PANEL DMA Display" (+ Adafruit GFX)
//
// - Time from NTP (written to the on-board RTC), RTC/build time as fallback
// - Modes: clock, Game of Life, plasma
// - Web page at http://ledclock.local to pick the mode, set message, color, brightness
// - Button: short press = next mode, long press = deep sleep, press again to wake
// - Firmware upload at http://ledclock.local/update (user admin, OTA_PASSWORD)

#include <Wire.h>
#include <sys/time.h>
#include <driver/gpio.h>
#include <driver/rtc_io.h>
#include <esp_sleep.h>
#include <esp_partition.h>
#include <esp_task_wdt.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
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

// On-board BM8563 RTC (battery backed), stores UTC
#define RTC_SDA  41
#define RTC_SCL  42
#define RTC_ADDR 0x51

#define BUTTON_PIN 11  // on-board push button, active low

#define HOSTNAME "ledclock"
#define TIMEZONE "EST5EDT,M3.2.0,M11.1.0"  // America/New_York

#define MSG_Y 52

MatrixPanel_I2S_DMA *display;
WebServer server(80);
Preferences prefs;

String message;
uint32_t msgColor;  // 0xRRGGBB
uint8_t brightness;
uint8_t nightStart, nightEnd, nightBrightness;  // hours; start == end disables

enum Mode { MODE_CLOCK, MODE_LIFE, MODE_PLASMA, MODE_TICKER, MODE_COUNT };
const char *modeNames[] = {"Clock", "Game of Life", "Plasma", "Ticker"};
uint8_t mode;
bool forceRedraw = true;  // set on mode change so the clock redraws everything
bool screenOn = true;     // off = panel blank but WiFi stays up (unlike deep sleep)
int wifiState = 0;        // 0 = offline, 1 = connected but no traffic, 2 = ok

String infoText;          // shown instead of the message for a while (e.g. IP)
unsigned long infoUntil = 0;
volatile bool ntpSynced = false;
volatile int disconnectReason = 0;  // set from the WiFi event task

// Free heap (KB) at boot / after display init / after WiFi+web init
uint32_t heapBoot, heapDisplay, heapWifi;

// Conway's Game of Life on a wrapping 64x64 grid, one bit per cell.
// Restarts the scene when it dies out, freezes, blinks in place, or after 1500 generations.
uint64_t lifeCur[PANEL_H], lifePrev[PANEL_H];
bool lifeReseed = true;
uint8_t lifeScene;   // index into lifeScenes, or LIFE_CYCLE
uint16_t lifeSpeed;  // ms per generation

// Famous patterns as RLE (b = dead, o = alive, $ = next row, number = repeat).
// Placements were checked in a simulator on the 64x64 torus: the gun's eater
// catches every glider, and the oscillators/spaceships never collide.
struct LifePart { const char *rle; int8_t x, y; };
struct LifeScene { const char *name; const LifePart *parts; uint8_t count; };

#define GOSPER_GUN "24bo$22bobo$12b2o6b2o12b2o$11bo3bo4b2o12b2o$2o8bo5bo3b2o$2o8bo3bob2o4bobo$10bo5bo7bo$11bo3bo$12b2o!"
#define EATER      "2o$obo$2bo$2b2o!"
#define PULSAR     "2b3o3b3o2$o4bobo4bo$o4bobo4bo$o4bobo4bo$2b3o3b3o2$2b3o3b3o$o4bobo4bo$o4bobo4bo$o4bobo4bo2$2b3o3b3o!"
#define PENTADECA  "2bo4bo$2ob4ob2o$2bo4bo!"
#define LWSS       "bo2bo$o$o3bo$4o!"
#define MWSS       "3bo$bo3bo$o$o4bo$5o!"
#define HWSS       "3b2o$bo4bo$o$o5bo$6o!"

const LifePart gunParts[] = {{GOSPER_GUN, 1, 1}, {EATER, 39, 25}};
const LifePart oscillatorParts[] = {
  {PULSAR, 3, 3}, {PULSAR, 25, 3}, {PENTADECA, 47, 8}, {PENTADECA, 47, 20},
  {"2o$2o$2b2o$2b2o!", 6, 30}, {"2o$2o$2b2o$2b2o!", 16, 30}, {"b3o$3o!", 28, 31}, {"3o!", 38, 32},
  {PULSAR, 3, 42}, {PENTADECA, 22, 47}, {PULSAR, 45, 42}};
const LifePart shipParts[] = {
  {LWSS, 5, 4}, {LWSS, 37, 4}, {MWSS, 20, 15}, {MWSS, 52, 15}, {HWSS, 10, 27},
  {HWSS, 42, 27}, {MWSS, 30, 40}, {LWSS, 5, 53}, {LWSS, 37, 53}};
const LifePart rPentomino[] = {{"b2o$2o$bo!", 30, 30}};
const LifePart acorn[] = {{"bo$3bo$2o2b3o!", 28, 30}};
const LifePart diehard[] = {{"6bo$2o$bo3b3o!", 28, 30}};

#define PARTS(p) p, sizeof(p) / sizeof(p[0])
const LifeScene lifeScenes[] = {
  {"Random", nullptr, 0},
  {"Glider gun", PARTS(gunParts)},
  {"Oscillators", PARTS(oscillatorParts)},
  {"Spaceships", PARTS(shipParts)},
  {"R-pentomino", PARTS(rPentomino)},
  {"Acorn", PARTS(acorn)},
  {"Diehard", PARTS(diehard)},
};
#define LIFE_SCENE_COUNT (sizeof(lifeScenes) / sizeof(lifeScenes[0]))
#define LIFE_CYCLE LIFE_SCENE_COUNT  // pseudo-scene: next scene each restart

// Stock ticker: quotes from Yahoo's chart API over HTTPS (~45 KB RAM for TLS).
#define MAX_QUOTES 12
#define QUOTE_REFRESH_MS (5 * 60 * 1000UL)
#define DEFAULT_TICKERS "^GSPC:S&P500,^FCHI:CAC40,AAPL,NVDA,GOOGL,TSM:TSMC,AMZN,META,TSLA,BTC-USD:BTC"
struct Quote { char sym[12], label[12]; float price, changePct; bool valid; const uint16_t *logo; };
enum TickerLayout { TICKER_BIG, TICKER_COMPACT, TICKER_CLOCK, TICKER_LAYOUT_COUNT };
const char *tickerLayoutNames[] = {"2 big rows", "3 compact rows", "2 compact rows + clock"};
uint8_t tickerLayout;

// Ticker logos: 16x16 RGB565, uploaded by tools/logos.py into the unused "spiffs"
// flash partition, one sector per logo so replacing one is a single erase.
// The partition is memory-mapped, so drawing reads flash directly and costs no RAM.
#define LOGO_SIZE 16
#define LOGO_SLOTS 64
#define LOGO_SLOT_BYTES 4096
struct LogoSlot { char sym[12]; uint16_t pixels[LOGO_SIZE * LOGO_SIZE]; };
const esp_partition_t *logoPartition;
const uint8_t *logoMap;
esp_partition_mmap_handle_t logoMapHandle;
uint16_t *logoUpload;  // staging buffer, only allocated during an upload
size_t logoUploadLen;
Quote quotes[MAX_QUOTES];
int quoteCount = 0;
SemaphoreHandle_t quotesLock;
String tickerSymbols;
bool tickerOpposite;  // bottom half scrolls the other way
volatile bool quotesFetching = false;
unsigned long quotesFetchedAt = 0;  // 0 = fetch as soon as possible
volatile int lastQuoteHttpCode = 0;

uint8_t bcd2dec(uint8_t v) { return (v >> 4) * 10 + (v & 0x0F); }
uint8_t dec2bcd(uint8_t v) { return ((v / 10) << 4) | (v % 10); }

// Days since 1970-01-01 for a civil date (newlib has no timegm)
long daysFromCivil(int y, int m, int d) {
  y -= m <= 2;
  long era = (y >= 0 ? y : y - 399) / 400;
  int yoe = y - era * 400;
  int doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
  int doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  return era * 146097 + doe - 719468;
}

// Returns false if the RTC is unreachable or its time is invalid (battery was lost)
bool rtcRead(time_t &t) {
  Wire.beginTransmission(RTC_ADDR);
  Wire.write(0x02);
  if (Wire.endTransmission(false) != 0) return false;
  if (Wire.requestFrom(RTC_ADDR, 7) != 7) return false;
  uint8_t s = Wire.read(), m = Wire.read(), h = Wire.read(), d = Wire.read();
  Wire.read();  // weekday
  uint8_t mo = Wire.read(), y = Wire.read();
  t = (time_t)daysFromCivil(2000 + bcd2dec(y), bcd2dec(mo & 0x1F), bcd2dec(d & 0x3F)) * 86400 +
      bcd2dec(h & 0x3F) * 3600 + bcd2dec(m & 0x7F) * 60 + bcd2dec(s & 0x7F);
  return !(s & 0x80);  // VL flag: low voltage, time not reliable
}

void rtcWrite(time_t now) {
  struct tm t;
  gmtime_r(&now, &t);
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

// Time the sketch was compiled (PC local time, so TZ must be set first)
time_t buildTime() {
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
  return mktime(&t);
}

bool isNight(int hour) {
  if (nightStart == nightEnd) return false;
  return nightStart < nightEnd ? hour >= nightStart && hour < nightEnd
                               : hour >= nightStart || hour < nightEnd;
}

void applyBrightness(int hour) {
  static int current = -1;
  int target = !screenOn ? 0 : isNight(hour) ? nightBrightness : brightness;
  if (target != current) {
    current = target;
    display->setBrightness8(target);
  }
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

// esp_timer instead of millis(), which wraps after 49 days
String uptimeText() {
  uint32_t mins = esp_timer_get_time() / 60000000ULL;
  if (mins < 60) return String(mins) + " min";
  if (mins < 24 * 60) return String(mins / 60) + " h " + (mins % 60) + " min";
  return String(mins / (24 * 60)) + " d " + (mins / 60 % 24) + " h";
}

void handleRoot() {
  char color[8];
  snprintf(color, sizeof(color), "#%06X", (unsigned)msgColor);
  String modes = "<form action=/mode>";
  for (int i = 0; i < MODE_COUNT; i++) {
    modes += String("<button name=m value=") + i + (i == mode ? " class=on>" : ">") +
             modeNames[i] + "</button> ";
  }
  modes += "</form>";
  String layoutOptions;
  for (int i = 0; i < TICKER_LAYOUT_COUNT; i++) {
    layoutOptions += String("<option value=") + i + (i == tickerLayout ? " selected>" : ">") +
                     tickerLayoutNames[i] + "</option>";
  }
  String sceneOptions;
  for (int i = 0; i <= LIFE_CYCLE; i++) {
    sceneOptions += String("<option value=") + i + (i == lifeScene ? " selected>" : ">") +
                    (i == LIFE_CYCLE ? "Cycle through all" : lifeScenes[i].name) + "</option>";
  }
  String page =
    "<!doctype html><meta name=viewport content='width=device-width,initial-scale=1'>"
    "<title>LED clock</title><style>"
    // border-box keeps width:100% inputs inside the page on narrow phone screens
    "*{box-sizing:border-box}"
    "body{font-family:sans-serif;max-width:420px;margin:0 auto;padding:1em}"
    "input,select,button{font-size:1em;max-width:100%}"
    "input[type=range],input[name=msg],select{width:100%}"
    "input[type=number]{width:4.5em}"
    "button{padding:.5em .8em;margin:.15em 0}"
    ".on{font-weight:bold;outline:2px solid}"
    "label{display:block;margin-top:1em}"
    "</style><h2>LED clock</h2>" + modes +
    "<form action=/power>Screen "
    "<button name=on value=1" + (screenOn ? " class=on" : "") + ">On</button> "
    "<button name=on value=0" + (!screenOn ? " class=on" : "") + ">Off</button> "
    "<button formaction=/sleep onclick=\"return confirm('Deep sleep? Only the button on the board can wake it.')\">"
    "Deep sleep</button></form>"
    "<form action=/set>"
    "<label>Message<input name=msg maxlength=100 value=\"" + htmlEscape(message) + "\"></label>"
    "<label>Color <input type=color name=color value='" + color + "'></label>"
    "<label>Brightness<input type=range name=bright min=1 max=255 value=" + brightness + "></label>"
    "<label>Night from <input type=number name=nstart min=0 max=23 value=" + nightStart + "> to "
    "<input type=number name=nend min=0 max=23 value=" + nightEnd + "> h<br><small>same hour = off</small></label>"
    "<label>Night brightness<input type=range name=nbright min=1 max=255 value=" + nightBrightness + "></label>"
    "<h3>Game of Life</h3>"
    "<label>Scene<select name=scene>" + sceneOptions + "</select></label>"
    "<label>Speed <small>(left = faster)</small>"
    "<input type=range name=lspeed min=20 max=1000 step=10 value=" + lifeSpeed + "></label>"
    "<h3>Ticker</h3>"
    "<label>Symbols <small>(comma separated, up to 12, Yahoo symbols, optional :Label, "
    "e.g. AAPL, ^GSPC:S&amp;P500, BTC-USD)</small>"
    "<input name=tickers maxlength=200 value=\"" + htmlEscape(tickerSymbols) + "\"></label>"
    "<label>Layout<select name=tlayout>" + layoutOptions + "</select></label>"
    "<label>Every other row scrolls<select name=topp>"
    "<option value=1" + (tickerOpposite ? " selected" : "") + ">the other way</option>"
    "<option value=0" + (!tickerOpposite ? " selected" : "") + ">the same way</option></select></label>"
    "<p><button>Save</button></form>"
    "<p><a href=/update>Firmware update</a>"
    // "lowest" is the one to watch: at ~5 KB WiFi silently stopped passing traffic
    "<p><small>Free RAM " + ESP.getFreeHeap() / 1024 + " KB &middot; lowest " +
    ESP.getMinFreeHeap() / 1024 + " KB &middot; WiFi " + WiFi.RSSI() + " dBm &middot; up " + uptimeText() +
    " &middot; <a href=/status>details</a></small>";
  server.send(200, "text/html", page);
}

void handleSet() {
  if (server.hasArg("msg")) message = server.arg("msg");
  if (server.hasArg("color")) msgColor = strtoul(server.arg("color").c_str() + 1, nullptr, 16);
  if (server.hasArg("bright")) brightness = constrain(server.arg("bright").toInt(), 1, 255);
  if (server.hasArg("nstart")) nightStart = constrain(server.arg("nstart").toInt(), 0, 23);
  if (server.hasArg("nend")) nightEnd = constrain(server.arg("nend").toInt(), 0, 23);
  if (server.hasArg("nbright")) nightBrightness = constrain(server.arg("nbright").toInt(), 1, 255);
  if (server.hasArg("lspeed")) lifeSpeed = constrain(server.arg("lspeed").toInt(), 20, 1000);
  if (server.hasArg("scene")) {
    uint8_t scene = constrain(server.arg("scene").toInt(), 0, (int)LIFE_CYCLE);
    if (scene != lifeScene) lifeReseed = true;
    lifeScene = scene;
  }
  if (server.hasArg("topp")) tickerOpposite = server.arg("topp") == "1";
  if (server.hasArg("tlayout")) {
    uint8_t layout = constrain(server.arg("tlayout").toInt(), 0, TICKER_LAYOUT_COUNT - 1);
    if (layout != tickerLayout && mode == MODE_TICKER) {
      display->clearScreen();
      forceRedraw = true;
    }
    tickerLayout = layout;
  }
  prefs.putUChar("tlayout", tickerLayout);
  if (server.hasArg("tickers") && server.arg("tickers") != tickerSymbols) {
    tickerSymbols = server.arg("tickers");
    parseSymbols();
  }
  prefs.putString("tickers", tickerSymbols);
  prefs.putBool("topp", tickerOpposite);
  prefs.putUShort("lspeed", lifeSpeed);
  prefs.putUChar("scene", lifeScene);
  prefs.putString("msg", message);
  prefs.putUInt("color", msgColor);
  prefs.putUChar("bright", brightness);
  prefs.putUChar("nstart", nightStart);
  prefs.putUChar("nend", nightEnd);
  prefs.putUChar("nbright", nightBrightness);
  server.sendHeader("Location", "/");
  server.send(303);
}

void setupWeb() {
  server.on("/", handleRoot);
  server.on("/set", handleSet);
  server.on("/mode", []() {
    if (server.hasArg("m")) setMode(server.arg("m").toInt());
    server.sendHeader("Location", "/");
    server.send(303);
  });
  server.on("/tickers", []() { server.send(200, "text/plain", tickerSymbols); });
  // Raw 16x16 RGB565 (little endian) logo for ?sym=, as a multipart file upload
  server.on("/logo", HTTP_POST, []() {
    if (!server.authenticate("admin", OTA_PASSWORD)) return server.requestAuthentication();
    bool ok = logoUpload && logoUploadLen == sizeof(LogoSlot::pixels) && server.hasArg("sym") &&
              saveLogo(server.arg("sym"), logoUpload);
    free(logoUpload);
    logoUpload = nullptr;
    server.send(ok ? 200 : 400, "text/plain", ok ? "OK\n" : "Bad logo\n");
  }, []() {
    if (!server.authenticate("admin", OTA_PASSWORD)) return;
    HTTPUpload &up = server.upload();
    if (up.status == UPLOAD_FILE_START) {
      free(logoUpload);
      logoUpload = (uint16_t *)malloc(sizeof(LogoSlot::pixels));
      logoUploadLen = 0;
    } else if (up.status == UPLOAD_FILE_WRITE && logoUpload) {
      if (logoUploadLen + up.currentSize <= sizeof(LogoSlot::pixels)) {
        memcpy((uint8_t *)logoUpload + logoUploadLen, up.buf, up.currentSize);
      }
      logoUploadLen += up.currentSize;  // oversized uploads fail the length check
    }
  });
  server.on("/status", []() {
    String s = String("heap ") + ESP.getFreeHeap() + " min " + ESP.getMinFreeHeap() +
               " largest " + ESP.getMaxAllocHeap() + "\nquotes fetching " + quotesFetching +
               " last http " + lastQuoteHttpCode + " fetched " +
               (quotesFetchedAt ? String((millis() - quotesFetchedAt) / 1000) + "s ago" : String("never")) + "\n";
    xSemaphoreTake(quotesLock, portMAX_DELAY);
    for (int i = 0; i < quoteCount; i++) {
      s += String(quotes[i].sym) + " " + (quotes[i].valid ? String(quotes[i].price, 2) + " " + String(quotes[i].changePct, 2) + "%" : String("-")) + "\n";
    }
    xSemaphoreGive(quotesLock);
    server.send(200, "text/plain", s);
  });
  server.on("/power", []() {
    if (server.hasArg("on")) setScreen(server.arg("on") == "1");
    server.sendHeader("Location", "/");
    server.send(303);
  });
  server.on("/sleep", []() {
    server.send(200, "text/html",
      "<meta name=viewport content='width=device-width,initial-scale=1'>"
      "<p style='font-family:sans-serif'>Sleeping. Press the button on the board to wake it.");
    delay(200);  // let the response go out before WiFi shuts down
    enterDeepSleep();
  });
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
    esp_task_wdt_reset();  // a full upload runs inside one handleClient() call
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
  static int failedChecks = 0, rejoins = 0, stalledRejoins = 0, offlineRetries = 0;
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
    // Still offline after many tries: a reboot is the reliable way back
    if (++offlineRetries > 10) ESP.restart();
    lostSince = millis();
    rejoinWifi();
  } else if (now && millis() - lastCheck > 15000) {
    lastCheck = millis();
    bool ok = gatewayReachable();
    failedChecks = ok ? 0 : failedChecks + 1;
    if (ok) offlineRetries = stalledRejoins = 0;
    if (failedChecks >= 3) {
      // Rejoining didn't bring traffic back (e.g. heap exhausted): reboot
      if (++stalledRejoins > 3) ESP.restart();
      failedChecks = 0;
      rejoins++;
      infoText = "No traffic, rejoin #" + String(rejoins);
      infoUntil = millis() + 20000;
      rejoinWifi();
      now = false;
    }
  }
  connected = now;
  wifiState = !now ? 0 : failedChecks ? 1 : 2;
}

void drawClock(const struct tm *t) {
  static int lastSec = -1;
  if (t->tm_sec == lastSec && !forceRedraw) return;
  lastSec = t->tm_sec;

  // Top-right dot: WiFi state
  uint16_t dotColors[] = {display->color565(255, 0, 0), display->color565(255, 200, 0),
                          display->color565(0, 255, 0)};
  display->fillRect(62, 0, 2, 2, dotColors[wifiState]);

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

  if (text != lastText || color != lastColor || forceRedraw) {
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

void lifePlace(const char *rle, int x0, int y0) {
  int x = x0, y = y0, n = 0;
  for (const char *p = rle; *p && *p != '!'; p++) {
    if (isdigit(*p)) {
      n = n * 10 + (*p - '0');
      continue;
    }
    int count = n ? n : 1;
    n = 0;
    if (*p == '$') {
      y += count;
      x = x0;
      continue;
    }
    for (int i = 0; i < count; i++, x++) {
      if (*p == 'o') lifeCur[y % PANEL_H] |= 1ULL << (x % PANEL_W);
    }
  }
}

void lifeSeed(const LifeScene &scene) {
  for (int y = 0; y < PANEL_H; y++) {
    lifePrev[y] = 0;
    // Two random words ANDed: ~25% of cells alive
    lifeCur[y] = scene.parts ? 0
                             : ((uint64_t)esp_random() << 32 | esp_random()) &
                                   ((uint64_t)esp_random() << 32 | esp_random());
  }
  for (int i = 0; i < scene.count; i++) lifePlace(scene.parts[i].rle, scene.parts[i].x, scene.parts[i].y);
}

void drawLife() {
  static unsigned long lastStep = 0;
  static int generation = 0, cycleIndex = -1;
  if (millis() - lastStep < lifeSpeed) return;
  lastStep = millis();

  if (lifeReseed) {
    lifeReseed = false;
    generation = 0;
    if (lifeScene == LIFE_CYCLE) cycleIndex = (cycleIndex + 1) % LIFE_SCENE_COUNT;
    lifeSeed(lifeScenes[lifeScene == LIFE_CYCLE ? cycleIndex : lifeScene]);
  }

  uint64_t next[PANEL_H];
  bool still = true, blinking = true, empty = true;
  for (int y = 0; y < PANEL_H; y++) {
    uint64_t up = lifeCur[(y + PANEL_H - 1) % PANEL_H], mid = lifeCur[y], down = lifeCur[(y + 1) % PANEL_H];
    uint64_t row = 0;
    for (int x = 0; x < PANEL_W; x++) {
      int l = (x + PANEL_W - 1) % PANEL_W, r = (x + 1) % PANEL_W;
      int n = ((up >> l) & 1) + ((up >> x) & 1) + ((up >> r) & 1) +
              ((mid >> l) & 1) + ((mid >> r) & 1) +
              ((down >> l) & 1) + ((down >> x) & 1) + ((down >> r) & 1);
      bool alive = (mid >> x) & 1;
      if (n == 3 || (alive && n == 2)) row |= 1ULL << x;
    }
    next[y] = row;
    still &= row == lifeCur[y];
    blinking &= row == lifePrev[y];
    empty &= row == 0;
  }

  uint16_t born = display->color565(120, 255, 80), alive = display->color565(0, 120, 255);
  for (int y = 0; y < PANEL_H; y++) {
    for (int x = 0; x < PANEL_W; x++) {
      bool on = (next[y] >> x) & 1, wasOn = (lifeCur[y] >> x) & 1;
      display->drawPixel(x, y, on ? (wasOn ? alive : born) : 0);
    }
    lifePrev[y] = lifeCur[y];
    lifeCur[y] = next[y];
  }
  if (still || blinking || empty || ++generation > 1500) lifeReseed = true;
}

// Plasma: overlapping sine waves mapped onto a rainbow palette.
// Integer lookup tables only: the S2 has no FPU.
void drawPlasma() {
  static uint8_t sine[256];
  static uint16_t palette[256];
  static bool ready = false;
  static unsigned long lastFrame = 0;
  static uint8_t t = 0;
  if (!ready) {
    ready = true;
    for (int i = 0; i < 256; i++) {
      sine[i] = 128 + 127 * sin(i * 2 * PI / 256);
      // Hue wheel: three phase-shifted sines give a smooth rainbow
      palette[i] = display->color565(sine[i], sine[(i + 85) & 255], sine[(i + 170) & 255]);
    }
  }
  if (millis() - lastFrame < 33) return;
  lastFrame = millis();
  t++;

  for (int y = 0; y < PANEL_H; y++) {
    for (int x = 0; x < PANEL_W; x++) {
      int v = sine[(x * 4 + t) & 255] + sine[(y * 3 - t) & 255] +
              sine[((x + y) * 2 + t * 2) & 255] + sine[(sine[(y * 4 + t) & 255] / 2 + x * 3) & 255];
      display->drawPixel(x, y, palette[(v / 4 + t) & 255]);
    }
  }
}

void setMode(uint8_t m) {
  mode = m % MODE_COUNT;
  prefs.putUChar("mode", mode);
  display->clearScreen();
  forceRedraw = true;
  lifeReseed = true;
}

const LogoSlot *logoSlot(int i) { return (const LogoSlot *)(logoMap + i * LOGO_SLOT_BYTES); }

const uint16_t *findLogo(const char *sym) {
  if (!logoMap) return nullptr;
  for (int i = 0; i < LOGO_SLOTS; i++) {
    if (!strncmp(logoSlot(i)->sym, sym, sizeof(LogoSlot::sym))) return logoSlot(i)->pixels;
  }
  return nullptr;
}

// (Re)map after writing, so reads can't come from stale cached flash
void mapLogos() {
  if (logoMap) esp_partition_munmap(logoMapHandle);
  logoMap = nullptr;
  const void *p;
  if (logoPartition && esp_partition_mmap(logoPartition, 0, LOGO_SLOTS * LOGO_SLOT_BYTES,
                                          ESP_PARTITION_MMAP_DATA, &p, &logoMapHandle) == ESP_OK) {
    logoMap = (const uint8_t *)p;
  }
}

// Call with quotesLock held
void refreshQuoteLogos() {
  for (int i = 0; i < quoteCount; i++) quotes[i].logo = findLogo(quotes[i].sym);
}

bool saveLogo(String sym, const uint16_t *pixels) {
  sym.trim();
  sym.toUpperCase();
  if (!logoMap || sym.length() == 0 || sym.length() >= sizeof(LogoSlot::sym)) return false;
  int slot = -1;
  for (int i = 0; i < LOGO_SLOTS && slot < 0; i++) {
    if (!strncmp(logoSlot(i)->sym, sym.c_str(), sizeof(LogoSlot::sym))) slot = i;
  }
  // Free = no valid name: erased (0xFF) or leftovers from the stock Huidu firmware
  for (int i = 0; i < LOGO_SLOTS && slot < 0; i++) {
    const char *name = logoSlot(i)->sym;
    int len = strnlen(name, sizeof(LogoSlot::sym));
    bool valid = len > 0 && len < (int)sizeof(LogoSlot::sym);
    for (int c = 0; c < len && valid; c++) valid = isprint((unsigned char)name[c]);
    if (!valid) slot = i;
  }
  if (slot < 0) slot = esp_random() % LOGO_SLOTS;

  LogoSlot *s = (LogoSlot *)calloc(1, sizeof(LogoSlot));
  if (!s) return false;
  strcpy(s->sym, sym.c_str());
  memcpy(s->pixels, pixels, sizeof(s->pixels));
  bool ok = esp_partition_erase_range(logoPartition, slot * LOGO_SLOT_BYTES, LOGO_SLOT_BYTES) == ESP_OK &&
            esp_partition_write(logoPartition, slot * LOGO_SLOT_BYTES, s, sizeof(LogoSlot)) == ESP_OK;
  free(s);

  xSemaphoreTake(quotesLock, portMAX_DELAY);
  mapLogos();
  refreshQuoteLogos();
  xSemaphoreGive(quotesLock);
  return ok;
}

// Comma-separated list -> quote slots (prices cleared until the next fetch)
void parseSymbols() {
  xSemaphoreTake(quotesLock, portMAX_DELAY);
  quoteCount = 0;
  int start = 0;
  while (start <= (int)tickerSymbols.length() && quoteCount < MAX_QUOTES) {
    int end = tickerSymbols.indexOf(',', start);
    if (end < 0) end = tickerSymbols.length();
    // "SYMBOL" or "SYMBOL:Label", e.g. "^GSPC:S&P500"
    String sym = tickerSymbols.substring(start, end), label;
    int colon = sym.indexOf(':');
    if (colon >= 0) {
      label = sym.substring(colon + 1);
      sym = sym.substring(0, colon);
    }
    sym.trim();
    sym.toUpperCase();
    label.trim();
    if (label.length() == 0) label = sym;
    if (sym.length() > 0 && sym.length() < sizeof(quotes[0].sym)) {
      Quote &q = quotes[quoteCount++];
      strcpy(q.sym, sym.c_str());
      strlcpy(q.label, label.c_str(), sizeof(q.label));
      q.valid = false;
    }
    start = end + 1;
  }
  refreshQuoteLogos();
  xSemaphoreGive(quotesLock);
  quotesFetchedAt = 0;
}

bool jsonNumber(const String &body, const char *key, float &out) {
  int i = body.indexOf(key);
  if (i < 0) return false;
  out = strtof(body.c_str() + i + strlen(key), nullptr);
  return true;
}

void fetchQuotes() {
  WiFiClientSecure client;
  client.setInsecure();  // no CA bundle in RAM; it's public quote data
  HTTPClient http;
  http.setReuse(true);  // one TLS handshake for all symbols
  http.setUserAgent("Mozilla/5.0");  // Yahoo rejects requests without one
  for (int i = 0;; i++) {
    char sym[sizeof(quotes[0].sym)];
    xSemaphoreTake(quotesLock, portMAX_DELAY);
    bool more = i < quoteCount;
    if (more) strcpy(sym, quotes[i].sym);
    xSemaphoreGive(quotesLock);
    if (!more) break;
    esp_task_wdt_reset();

    String path = sym;
    path.replace("^", "%5E");  // index symbols like ^GSPC
    float price, pct;
    bool ok = false;
    if (http.begin(client, "https://query1.finance.yahoo.com/v8/finance/chart/" + path + "?range=1d&interval=1d")) {
      lastQuoteHttpCode = http.GET();
      if (lastQuoteHttpCode == 200) {
        String body = http.getString();
        float prev;
        ok = jsonNumber(body, "\"regularMarketPrice\":", price);
        if (ok && !jsonNumber(body, "\"regularMarketChangePercent\":", pct)) {
          pct = jsonNumber(body, "\"chartPreviousClose\":", prev) && prev ? (price - prev) / prev * 100 : 0;
        }
      }
      http.end();
    }

    xSemaphoreTake(quotesLock, portMAX_DELAY);
    // The list may have been edited from the web page meanwhile
    if (ok && i < quoteCount && !strcmp(quotes[i].sym, sym)) {
      quotes[i].price = price;
      quotes[i].changePct = pct;
      quotes[i].valid = true;
    }
    xSemaphoreGive(quotesLock);
  }
  quotesFetchedAt = millis();
}

// Runs in the loop task: a separate task's 8 KB stack left too little heap for TLS.
// mDNS is paused meanwhile too: the prebuilt mbedTLS uses two fixed 16 KB buffers and
// keeps the peer certificate, which took free heap down to 2 KB.
void maybeFetchQuotes() {
  if (quotesFetching || wifiState != 2) return;
  if (quotesFetchedAt && millis() - quotesFetchedAt < QUOTE_REFRESH_MS) return;
  // TLS needs two 16 KB buffers in one piece each; with less, skip this round
  // rather than starve WiFi
  if (ESP.getMaxAllocHeap() < 36 * 1024 || ESP.getFreeHeap() < 50 * 1024) {
    quotesFetchedAt = millis();
    return;
  }
  quotesFetching = true;
  MDNS.end();
  fetchQuotes();
  MDNS.begin(HOSTNAME);
  MDNS.addService("http", "tcp", 80);
  quotesFetching = false;
}

// "AAPL 347.10" over "\x1E+3.19 +0.93%" (up/down arrow from CP437)
void formatQuote(const Quote &q, char *line1, char *line2) {
  if (!q.valid) {
    snprintf(line1, 24, "%s ...", q.label);
    line2[0] = 0;
  } else {
    float change = q.price - q.price / (1 + q.changePct / 100);
    snprintf(line1, 24, q.price < 1000 ? "%s %.2f" : "%s %.0f", q.label, q.price);
    snprintf(line2, 24, "%c%+.2f %+.2f%%", q.changePct >= 0 ? 0x1E : 0x1F, change, q.changePct);
  }
}

// Item width in px, including the gap to the next item. Widths are whole character
// cells so every pixel of the row gets painted by some cell (no clearing, no flicker).
int quoteWidth(const Quote &q, bool big) {
  char line1[24], line2[24];
  formatQuote(q, line1, line2);
  int l1 = strlen(line1), l2 = strlen(line2);
  if (big) return (max(l1, (l2 + 1) / 2) + 2 + (q.logo ? 2 : 0)) * 12;
  return (max(l1, l2) + 2 + (q.logo ? 3 : 0)) * 6;
}

// One scrolling row of every step-th quote starting at first, scrolled by offset px.
// Big: line 1 in 2x font (16px) over line 2 in 1x font, 28px tall.
// Compact: both lines in 1x font next to the 16x16 logo, 16px tall.
void drawTickerRow(int y, bool big, int first, int step, int offset) {
  Quote items[MAX_QUOTES];
  int n = 0;
  xSemaphoreTake(quotesLock, portMAX_DELAY);
  for (int i = first; i < quoteCount; i += step) items[n++] = quotes[i];
  if (n == 0) {  // fewer symbols than rows: repeat them all
    for (int i = 0; i < quoteCount; i++) items[n++] = quotes[i];
  }
  xSemaphoreGive(quotesLock);
  if (n == 0) return;

  int total = 0;
  for (int i = 0; i < n; i++) total += quoteWidth(items[i], big);
  int x = -(((offset % total) + total) % total);

  uint16_t black = 0, labelColor = display->color565(255, 200, 0), white = display->color565(255, 255, 255);
  int cw = big ? 12 : 6;               // line 1 cell width
  int logoPx = big ? 24 : 18;          // logo + margins, in whole cells
  int logoX = big ? 2 : 0, logoY = big ? y + 4 : y;
  int line1Y = big ? y + 4 : y, line2Y = big ? y + 22 : y + 8;
  char line1[24], line2[24];

  for (int i = 0; x < PANEL_W; i = (i + 1) % n) {
    const Quote &q = items[i];
    int width = quoteWidth(q, big);
    formatQuote(q, line1, line2);
    int labelLen = strlen(q.label), l1 = strlen(line1), l2 = strlen(line2);
    uint16_t changeColor = !q.valid ? display->color565(100, 100, 100)
                           : q.changePct >= 0 ? display->color565(0, 255, 0)
                                              : display->color565(255, 0, 0);
    int lp = q.logo ? logoPx : 0;
    if (lp && x > -lp && x < PANEL_W) {
      if (logoX) display->fillRect(x, logoY, logoX, LOGO_SIZE, black);
      display->fillRect(x + logoX + LOGO_SIZE, logoY, lp - logoX - LOGO_SIZE, LOGO_SIZE, black);
      display->drawRGBBitmap(x + logoX, logoY, q.logo, LOGO_SIZE, LOGO_SIZE);
    }
    for (int c = 0; c * cw < width - lp; c++) {
      int cx = x + lp + c * cw;
      if (cx <= -cw || cx >= PANEL_W) continue;
      display->drawChar(cx, line1Y, c < l1 ? line1[c] : ' ', c < labelLen ? labelColor : white, black, big ? 2 : 1);
    }
    // Big: line 2 runs under the logo too (padded with blanks), compact: next to it
    int l2Start = big ? 0 : lp;
    for (int c = 0; c * 6 < width - l2Start; c++) {
      int cx = x + l2Start + c * 6, sc = big ? c - lp / 6 : c;
      if (cx <= -6 || cx >= PANEL_W) continue;
      display->drawChar(cx, line2Y, sc >= 0 && sc < l2 ? line2[sc] : ' ', changeColor, black, 1);
    }
    x += width;
  }
}

void drawTicker(const struct tm *t) {
  static unsigned long lastStep = 0;
  static int offset = 0, lastMin = -1;
  maybeFetchQuotes();
  uint16_t divider = display->color565(40, 40, 40);
  if (forceRedraw) {
    if (tickerLayout == TICKER_BIG) {
      display->drawFastHLine(0, 31, PANEL_W, divider);
    } else {
      display->drawFastHLine(0, 21, PANEL_W, divider);
      display->drawFastHLine(0, 42, PANEL_W, divider);
    }
  }
  if (tickerLayout == TICKER_CLOCK && (t->tm_min != lastMin || forceRedraw)) {
    lastMin = t->tm_min;
    char buf[8];
    snprintf(buf, sizeof(buf), "%02d:%02d", t->tm_hour, t->tm_min);
    display->setTextSize(2);
    display->setTextColor(display->color565(0, 200, 255), 0);
    display->setCursor(3, 24);
    display->print(buf);
  }
  if (millis() - lastStep < 30 && !forceRedraw) return;
  lastStep = millis();
  offset++;
  // Odd rows go the other way when the "other way" option is on
  auto dir = [](int row) { return tickerOpposite && row % 2 ? -1 : 1; };
  switch (tickerLayout) {
    case TICKER_BIG:
      drawTickerRow(0, true, 0, 2, offset);
      drawTickerRow(32, true, 1, 2, offset * dir(1));
      break;
    case TICKER_COMPACT:
      drawTickerRow(3, false, 0, 3, offset);
      drawTickerRow(24, false, 1, 3, offset * dir(1));
      drawTickerRow(45, false, 2, 3, offset);
      break;
    case TICKER_CLOCK:
      drawTickerRow(3, false, 0, 2, offset);
      drawTickerRow(45, false, 1, 2, offset * dir(1));
      break;
  }
}

void setScreen(bool on) {
  screenOn = on;
  display->clearScreen();
  forceRedraw = true;
}

void enterDeepSleep() {
  display->clearScreen();
  delay(50);  // let a full black frame latch into the panel
  display->stopDMAoutput();
  // OE is active low: hold it high so the panel stays dark while the chip sleeps
  pinMode(OE, OUTPUT);
  digitalWrite(OE, HIGH);
  gpio_hold_en((gpio_num_t)OE);
  gpio_deep_sleep_hold_en();

  // The wake is level-triggered, so sleeping with the button still down would wake instantly
  while (digitalRead(BUTTON_PIN) == LOW) delay(10);
  delay(50);

  WiFi.disconnect(true);
  WiFi.mode(WIFI_OFF);
  rtc_gpio_pullup_en((gpio_num_t)BUTTON_PIN);
  rtc_gpio_pulldown_dis((gpio_num_t)BUTTON_PIN);
  esp_sleep_enable_ext0_wakeup((gpio_num_t)BUTTON_PIN, 0);
  esp_deep_sleep_start();
}

// Short press (released before 2s) = next mode (or screen on if off), hold 2s = deep sleep
void checkButton() {
  // Starting from the current level ignores a press still held from waking up
  static bool raw = digitalRead(BUTTON_PIN) == LOW, stable = raw, armed = false;
  static unsigned long changedAt = 0, pressedAt = 0;
  bool down = digitalRead(BUTTON_PIN) == LOW;
  if (down != raw) {
    raw = down;
    changedAt = millis();
  }
  if (raw != stable && millis() - changedAt > 30) {
    stable = raw;
    if (stable) {
      pressedAt = millis();
      armed = true;
    } else if (armed) {
      armed = false;
      if (screenOn) setMode(mode + 1);
      else setScreen(true);
    }
  }
  if (stable && armed && millis() - pressedAt > 2000) {
    armed = false;
    enterDeepSleep();
  }
}

void timeSynced(struct timeval *) { ntpSynced = true; }

void setup() {
  heapBoot = ESP.getFreeHeap() / 1024;
  prefs.begin("clock");
  message = prefs.getString("msg", "Hello!");
  msgColor = prefs.getUInt("color", 0x00FF60);
  brightness = prefs.getUChar("bright", 80);
  nightStart = prefs.getUChar("nstart", 22);
  nightEnd = prefs.getUChar("nend", 7);
  nightBrightness = prefs.getUChar("nbright", 8);
  mode = prefs.getUChar("mode", MODE_CLOCK) % MODE_COUNT;
  lifeSpeed = constrain(prefs.getUShort("lspeed", 100), 20, 1000);
  lifeScene = min<int>(prefs.getUChar("scene", 0), LIFE_CYCLE);
  quotesLock = xSemaphoreCreateMutex();
  logoPartition = esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_SPIFFS, nullptr);
  mapLogos();
  tickerSymbols = prefs.getString("tickers", DEFAULT_TICKERS);
  tickerOpposite = prefs.getBool("topp", true);
  tickerLayout = min<int>(prefs.getUChar("tlayout", TICKER_CLOCK), TICKER_LAYOUT_COUNT - 1);
  parseSymbols();

  // Undo what enterDeepSleep() set up, in case we just woke from it
  gpio_hold_dis((gpio_num_t)OE);
  gpio_deep_sleep_hold_dis();
  rtc_gpio_deinit((gpio_num_t)BUTTON_PIN);
  pinMode(BUTTON_PIN, INPUT_PULLUP);

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
  time_t rtc, build = buildTime();
  if (!rtcRead(rtc) || rtc < build) {
    rtc = build;
    rtcWrite(rtc);
  }
  struct timeval tv = {rtc, 0};
  settimeofday(&tv, nullptr);

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

  // Reboot if loop() ever hangs. 60 s, not the default 5 s: quote fetches and OTA
  // uploads block the loop for a while (they feed the watchdog as they go).
  esp_task_wdt_config_t wdt = {.timeout_ms = 60000, .idle_core_mask = 0, .trigger_panic = true};
  esp_task_wdt_reconfigure(&wdt);
  esp_task_wdt_add(nullptr);
}

void loop() {
  esp_task_wdt_reset();
  server.handleClient();
  checkWifi();
  checkButton();
  if (ntpSynced) {
    ntpSynced = false;
    rtcWrite(time(nullptr));
  }

  time_t now = time(nullptr);
  struct tm *t = localtime(&now);
  applyBrightness(t->tm_hour);
  switch (screenOn ? mode : MODE_COUNT) {
    case MODE_CLOCK:
      drawClock(t);
      drawMessage();
      break;
    case MODE_LIFE:
      drawLife();
      break;
    case MODE_PLASMA:
      drawPlasma();
      break;
    case MODE_TICKER:
      drawTicker(t);
      break;
  }
  forceRedraw = false;
  delay(2);
}
