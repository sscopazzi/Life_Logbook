/*
  Logbook  v2.0   (was HWOT / HouseWO hub)
  Sophie Scopazzi / Aspect Wave LLC

  A carry-around box: work orders, daily checklist, calendar, GPS track, house climate,
  expiring documents, kitchen timers, find-my-box and a BNWAS-style move reminder.

  Board : Adafruit ESP32-S2 Feather (LiPo + fuel gauge on board)
          + Adalogger FeatherWing (PCF8523 RTC + microSD, CS 10)   RTC, climate + GPS archive, log copy
          + Mini GPS PA1010D (Adafruit 4415) on I2C, 0x10           track + clock away from home
          + 1.5" 128x128 grayscale OLED (4741, SSD1327, STEMMA QT, I2C 0x3D)   status / checklist / today / notes / trip / join
          + piezo (1739) between pins 5 and 6                        timers, find-my-box, BNWAS
          + knob: I2C rotary encoder with push switch (5880, seesaw), STEMMA QT, 0x37 (A0 jumper closed)
            optional: its INT pad to pin 12 (set ENC_INT_PIN) so the box only reads it when it moves
          + button: momentary (558), pin 11 to GND; its LED ring + on A0 (pin 18)
          + I2C EEPROM breakout (24LC32 or larger, 0x50)             on/off record
          + SHT30 on STEMMA QT (optional)                            shows up as node "Hub"
  Power switch: latching button (916) - common to GND, normally-closed contact to EN: button out = off.
    Off = regulator off; the LiPo still charges on USB. (Terminal FeatherWing: leave its slide switch ON.)
  Arduino IDE: Tools > Partition Scheme > "Default 4MB with spiffs (1.2MB APP/1.5MB SPIFFS)"

  Wi-Fi: joins the house Wi-Fi whenever it is in range (scans for it every minute or so while away)
    and always runs its own hotspot. Away from home, connect the phone to the hotspot.

  Storage:
    Internal flash (LittleFS) holds the work-order log /hwo/log.jsonl — one JSON line per change,
    append-only, so it is also the full audit trail.
    SD card: log copy, climate CSVs (/env), GPS track CSVs (/gps), attachments (/att), checklist
    archive and imported calendars. Without a card, climate and GPS keep ENV_FLASH_DAYS in flash.

  House nodes: while the box is away they keep their own readings and send them in when it is back
    (node firmware 2.0+). That fill-in needs the SD card in the box.

  Dashboard: kept in flash as /www/index.html.gz, not in the firmware. Updates are one .lbk file
    (dashboard + firmware) uploaded from Setup > Firmware, or from the page the box shows when it has
    no dashboard yet (first install over USB).

  Documents: the daily checklist is one small JSON file (/hwo/doc/checklist.json, flash, copied to SD)
    rewritten on each change, so typing and checking never grow the work-order log. Archived days are
    /hwo/cl/YYYY-MM-DD.json, their search index one file a month /hwo/cx/YYYY-MM.json (the dashboard
    keeps it up to date), and imported calendars /hwo/ics/<id>.json, on the SD card when there is one.
    Notes: /hwo/nt/0.json (categories) and /hwo/nt/1.json … (notes packed into files under 32 kB), in
    flash with a copy on the card; archived notes /hwo/na/1.json … and earlier versions /hwo/nh/1.json …
    on the card (flash without one). Pinned notes reach the box's screen inside /hwo/doc/oled.json.

  EEPROM: a once-a-minute "still on" heartbeat, rotated across 240 slots for wear. After a
    power-on the box knows when it was switched off and logs the off period.

  Clock: NTP on the house Wi-Fi, else GPS, else the PCF8523, else set from the phone/laptop.

  Libraries: RTClib, Adafruit SHT31, Adafruit MAX1704X, Adafruit LC709203F, Adafruit BusIO,
             Adafruit SSD1327, Adafruit GFX, Adafruit seesaw, ArduinoJson (7.x).
  Core: esp32 by Espressif 3.x
*/

#include <WiFi.h>
#include <WebServer.h>
#include <ESPmDNS.h>
#include <Update.h>
#include <esp_now.h>
#include <esp_wifi.h>
#include <esp_sntp.h>
#include <esp_system.h>
#include <esp_idf_version.h>
#include <FS.h>
#include <LittleFS.h>
#include <SPI.h>
#include <SD.h>
#include <Wire.h>
#include <RTClib.h>
#include <Adafruit_SHT31.h>
#include <Adafruit_MAX1704X.h>
#include <Adafruit_seesaw.h>
#include <Adafruit_LC709203F.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1327.h>
#include <ArduinoJson.h>
#include <vector>
#include <time.h>
#include <sys/time.h>
#include <mbedtls/sha256.h>
#define HUB_AP_SSID "Logbook"         // the starting hotspot, the same on every new box
#define HUB_AP_PASS "logbook-setup"   // shown on the box's JOIN screen; first-boot setup makes you choose your own

#define FW_VERSION "2.0"

// ===================== USER SETTINGS =====================
const bool  AP_ALWAYS      = true;          // keep the hotspot up even when on house Wi-Fi
const uint8_t AP_CHANNEL   = 1;             // hotspot channel when NOT on house Wi-Fi
const char* MDNS_NAME      = "logbook";     // http://logbook.local
const char* TZ_POSIX       = "UTC0";        // until first-boot setup, which starts from the phone's time zone
const uint8_t  SD_CS          = 10;         // Adalogger FeatherWing SD chip select
const uint8_t  EE_ADDR        = 0x50;       // I2C EEPROM address
const uint32_t ENV_LOG_S      = 300;        // climate log interval (s), aligned to the clock
const uint8_t  ENV_FLASH_DAYS = 10;         // climate days kept in flash when there is no SD card
const uint32_t HUB_SENSOR_S   = 30;         // hub's own SHT30 read interval (if fitted)
const uint32_t OUTAGE_MIN_S   = 120;        // shorter off periods aren't logged (USB flashing, quick resets)
const uint8_t  GPS_ADDR       = 0x10;       // PA1010D on I2C
const uint32_t GPS_LOG_S      = 30;         // log a track point at most this often while moving...
const float    GPS_MOVE_M     = 15.0f;      // ...and only after moving at least this far
const uint32_t GPS_STILL_S    = 300;        // standing still: one point every 5 min, so a gap means no fix
const float    GPS_MAX_HDOP   = 5.0f;       // poorer fixes are not logged
const uint32_t BATT_READ_S    = 30;
// =========================================================

// ---------- ESP-NOW packets: keep identical in HouseWO_Node ----------
#define HWO_MAGIC 0x31574F48UL   // "HOW1" node -> box: live reading (v1 37 bytes, v2 45 bytes)
#define HWO_TIME  0x54574F48UL   // "HOWT" box -> node: clock, and whether it can take buffered readings
#define HWO_BFILL 0x42574F48UL   // "HOWB" node -> box: readings kept while the box was away
#define HWO_BACK  0x41574F48UL   // "HOWA" box -> node: that batch is written
typedef struct __attribute__((packed)) {
  uint32_t magic;
  uint8_t  ver;
  char     name[12];     // null-terminated, up to 11 characters
  uint32_t seq;
  float    tempC;
  float    rh;
  float    vbat;
  float    soc;
  uint16_t intervalS;
  uint8_t  flags;        // bit0 sensor ok, bit1 battery monitor ok
} EnvPacket;
typedef struct __attribute__((packed)) {
  EnvPacket b;
  uint32_t clockS;       // node's own clock (epoch once it has been set by the box)
  uint32_t pending;      // readings waiting to be sent in
} EnvPacketV2;
typedef struct __attribute__((packed)) {
  uint32_t magic;
  uint32_t epoch;
  uint8_t  flags;        // bit0 epoch valid, bit1 box can store buffered readings (SD card in)
} TimePacket;
typedef struct __attribute__((packed)) {
  uint32_t epoch;
  int16_t  t100;         // degC x100, INT16_MIN = no reading
  uint16_t rh100;        // %RH x100, 0xFFFF = no reading
  uint8_t  soc;          // battery %, 255 = unknown
} BfRec;
#define BF_MAX 24
typedef struct __attribute__((packed)) {
  uint32_t magic;
  char     name[12];
  uint16_t bseq;
  uint8_t  n;
  BfRec    r[BF_MAX];    // only n are sent
} BfPacket;
#define BF_HEAD 19
typedef struct __attribute__((packed)) {
  uint32_t magic;
  uint16_t bseq;
  uint8_t  ok;
} BfAck;
// ---------------------------------------------------------------------

WebServer server(80);
RTC_PCF8523 rtc;
Adafruit_SHT31 sht;
Adafruit_MAX17048 gaugeMax;
Adafruit_LC709203F gaugeLc;

bool rtcOK = false, hubSht = false;
bool timeValid = false;
struct Sess { String h; uint32_t exp; bool keep; };   // a login: hash of its token; keep: exp in epoch s (0 = no end), else millis
const char* timeSrc = "none";
volatile bool ntpSynced = false;
bool staUp = false, apUp = false;
volatile bool staLost = false;
uint32_t lastScan = 0, scanEvery = 45000;
bool scanning = false;
uint32_t lastNtpSync = 0;

// ---------- storage ----------
bool flashOK = false, sdOK = false;
fs::FS* store = nullptr;                 // where the work-order log lives
const char* storeName = "none";
const char* LOG_PATH = "/hwo/log.jsonl";
uint32_t logNextSeq = 1;
uint32_t logBytes = 0;
uint32_t lastSdRetry = 0;
uint32_t lastEnvSlot = 0;
String   lastPruneDay = "";
uint32_t lastHubRead = 0;
uint32_t docVer = 0;                       // bumps on every document write; the dashboard watches it
bool otaAuthorized = false;
// .lbk bundle: "LOGBOOK1 {...,"ui":N,"bin":M}\n" + N bytes of gzipped dashboard + M bytes of firmware
uint8_t upKind = 0;             // 0 unknown, 1 plain firmware, 2 bundle header, 3 bundle dashboard, 4 bundle firmware, 9 error
String upHead, otaErr;
size_t upUiLeft = 0, upBinSize = 0;
File upUi;
bool uiAuthorized = false;
File attFile;
String attPath = "";
const char* attWhere = "";
size_t attBytes = 0;
bool attOK = false;
String attErr = "";
size_t uiBytes = 0;
File uiFile;

// ---------- EEPROM heartbeat ----------
const uint16_t HB_BASE = 0x100, HB_SLOT = 16, HB_SLOTS = 240;   // 0x100-0xFFF, fits a 24LC32
const uint16_t HB_MAGIC = 0xB7A5;
typedef struct __attribute__((packed)) {
  uint32_t seq;
  uint32_t epoch;
  uint32_t upS;
  uint16_t magic;
  uint16_t crc;
} HbSlot;
bool eeOK = false;
uint32_t hbSeq = 0;
uint16_t hbNext = 0;
uint32_t lastAlive = 0;          // epoch of the newest heartbeat found at boot
uint32_t lastHbMs = 0;
bool outageHandled = false;
esp_reset_reason_t resetReason;

// ---------- sensor nodes ----------
struct Node {
  bool     used;
  char     name[12];
  uint8_t  mac[6];
  float    tempC, rh, vbat, soc;
  int8_t   rssi;
  uint32_t seq;
  uint16_t interval;
  uint8_t  flags;
  uint32_t lastMs;
  uint32_t lastEpoch;    // wall-clock time of the newest reading (0 = unknown)
  uint32_t count;
  bool     live;         // heard since this boot (false = last reading recovered from the archive)
  uint16_t bfSeq;        // last backfill batch written (resent batches are skipped)
  bool     bfAny;
  uint32_t bfRows;
};
const int MAX_NODES = 6;
Node nodes[MAX_NODES];

struct RxItem { uint8_t d[240]; uint8_t len; uint8_t mac[6]; int8_t rssi; };
RxItem rxq[8];
volatile uint8_t rxHead = 0, rxTail = 0;
portMUX_TYPE rxMux = portMUX_INITIALIZER_UNLOCKED;

// ======================================================================
//  Settings kept on the box (first-boot setup / Setup > Network). Nothing personal is compiled in: the starting
//  hotspot is public, and there is no default home Wi-Fi and no default box password.
// ======================================================================
struct Cfg { String apSsid, apPass, homeSsid, homePass, tz, passSalt, passHash; bool setupDone; int contrast; bool bnwasOn; int bnwasMin; int scrOffMin; bool clHideDone; } cfg;
const char* CFG_PATH = "/hwo/config.json";
volatile bool cfgApplyAp = false, cfgApplySta = false;
uint32_t cfgApplyAt = 0;

void cfgDefaults() {
  cfg.apSsid = HUB_AP_SSID; cfg.apPass = HUB_AP_PASS; cfg.homeSsid = ""; cfg.homePass = ""; cfg.passSalt = ""; cfg.passHash = "";
  cfg.tz = TZ_POSIX; cfg.setupDone = false; cfg.contrast = 0x7F; cfg.bnwasOn = false; cfg.bnwasMin = 30; cfg.scrOffMin = 5; cfg.clHideDone = false;
}
void cfgLoad() {
  cfgDefaults();
  File f = LittleFS.open(CFG_PATH, FILE_READ);
  if (!f) return;
  JsonDocument d; DeserializationError e = deserializeJson(d, f); f.close();
  if (e) return;
  cfg.apSsid = d["apSsid"] | cfg.apSsid; cfg.apPass = d["apPass"] | cfg.apPass;
  cfg.homeSsid = d["homeSsid"] | cfg.homeSsid; cfg.homePass = d["homePass"] | cfg.homePass;
  cfg.tz = d["tz"] | cfg.tz; cfg.setupDone = d["setupDone"] | false; cfg.contrast = d["contrast"] | cfg.contrast;
  cfg.bnwasOn = d["bnwasOn"] | false; cfg.bnwasMin = constrain((int)(d["bnwasMin"] | 30), 1, 240);
  cfg.scrOffMin = constrain((int)(d["scrOffMin"] | 5), 0, 240);   // 0 = never sleeps
  cfg.clHideDone = d["clHideDone"] | false;
  cfg.passSalt = d["passSalt"] | ""; cfg.passHash = d["passHash"] | "";
}
bool cfgSave() {
  JsonDocument d;
  d["apSsid"] = cfg.apSsid; d["apPass"] = cfg.apPass; d["homeSsid"] = cfg.homeSsid; d["homePass"] = cfg.homePass;
  d["tz"] = cfg.tz; d["setupDone"] = cfg.setupDone; d["contrast"] = cfg.contrast; d["bnwasOn"] = cfg.bnwasOn; d["bnwasMin"] = cfg.bnwasMin; d["scrOffMin"] = cfg.scrOffMin; d["clHideDone"] = cfg.clHideDone;
  d["passSalt"] = cfg.passSalt; d["passHash"] = cfg.passHash;
  String out; serializeJson(d, out);
  File f = LittleFS.open("/hwo/config.tmp", FILE_WRITE);
  if (!f) return false;
  bool ok = f.print(out) == out.length(); f.close();
  if (!ok) return false;
  LittleFS.remove(CFG_PATH);
  return LittleFS.rename("/hwo/config.tmp", CFG_PATH);
}


// ======================================================================
//  The box password and logins
//  The box keeps a salted SHA-256 of the password, never the password. Over the home Wi-Fi the dashboard
//  needs a login; the browser keeps a random token in a cookie, the box keeps the token's hash. "Remember
//  this device" keeps it for a year (in /hwo/sessions.json), otherwise until the box restarts or 12 h pass.
//  On the hotspot no login is asked. Before a password is set, nothing is asked anywhere.
// ======================================================================
String sha256hex(const String& in) {
  uint8_t h[32]; char o[65];
  mbedtls_sha256((const unsigned char*)in.c_str(), in.length(), h, 0);
  for (int i = 0; i < 32; i++) sprintf(o + 2 * i, "%02x", h[i]);
  return String(o);
}
String randHex(int bytes) { String o; char b[3]; for (int i = 0; i < bytes; i++) { sprintf(b, "%02x", (unsigned)(esp_random() & 0xFF)); o += b; } return o; }
bool passSet() { return cfg.passHash.length() == 64; }
void passSetTo(const String& p) { cfg.passSalt = randHex(8); cfg.passHash = sha256hex(cfg.passSalt + p); }
uint8_t pwFails = 0; uint32_t pwLockUntil = 0;
bool pwCheck(const String& p) {           // true if it's the box password (or none is set yet); five misses: 30 s pause
  if (!passSet()) return true;
  if (pwFails >= 5 && (int32_t)(millis() - pwLockUntil) < 0) return false;
  if (p.length() && sha256hex(cfg.passSalt + p) == cfg.passHash) { pwFails = 0; return true; }
  if (++pwFails >= 5) pwLockUntil = millis() + 30000;
  return false;
}
bool pwLocked() { return pwFails >= 5 && (int32_t)(millis() - pwLockUntil) < 0; }

std::vector<Sess> sess;
const char* SESS_PATH = "/hwo/sessions.json";
void sessLoad() {
  sess.clear();
  File f = LittleFS.open(SESS_PATH, FILE_READ); if (!f) return;
  JsonDocument d; DeserializationError e = deserializeJson(d, f); f.close();
  if (e) return;
  for (JsonArray x : d.as<JsonArray>()) sess.push_back({ String((const char*)(x[0] | "")), (uint32_t)(x[1] | 0UL), true });
}
void sessSave() {
  JsonDocument d; JsonArray a = d.to<JsonArray>();
  for (auto& x : sess) if (x.keep) { JsonArray r = a.add<JsonArray>(); r.add(x.h); r.add(x.exp); }
  String o; serializeJson(d, o);
  File f = LittleFS.open(SESS_PATH, FILE_WRITE); if (f) { f.print(o); f.close(); }
}
bool sessLive(const Sess& x) {
  if (x.keep) return !x.exp || !timeValid || (uint32_t)time(nullptr) < x.exp;
  return (int32_t)(x.exp - millis()) > 0;
}
String sessNew(bool keep) {
  for (size_t i = sess.size(); i-- > 0;) if (!sessLive(sess[i])) sess.erase(sess.begin() + i);
  while (sess.size() >= 24) sess.erase(sess.begin());
  String t = randHex(16);
  sess.push_back({ sha256hex(t), keep ? (timeValid ? (uint32_t)time(nullptr) + 365UL * 86400UL : 0UL) : millis() + 12UL * 3600000UL, keep });
  if (keep) sessSave();
  return t;
}
String cookieTok() {
  String c = server.header("Cookie"); int i = c.indexOf("lb_s="); if (i < 0) return "";
  String t = c.substring(i + 5); int e = t.indexOf(';'); if (e >= 0) t = t.substring(0, e); t.trim();
  return t.length() == 32 ? t : String("");
}
int sessFind() {
  String t = cookieTok(); if (!t.length()) return -1;
  String h = sha256hex(t);
  for (size_t i = 0; i < sess.size(); i++) if (sess[i].h == h) return sessLive(sess[i]) ? (int)i : -1;
  return -1;
}
int sessKept() { int n = 0; for (auto& x : sess) if (x.keep) n++; return n; }
bool viaHotspot() { return server.client().localIP() == WiFi.softAPIP(); }
bool needLogin() { return passSet() && !viaHotspot(); }
bool authed() { return !needLogin() || sessFind() >= 0; }
bool gate() { if (authed()) return true; sendErr(401, "Log in"); return false; }   // every API call over the home Wi-Fi
std::function<void()> G(std::function<void()> f) { return [f]() { if (gate()) f(); }; }
void setCookie(const String& t, bool keep) {
  server.sendHeader("Set-Cookie", "lb_s=" + t + "; Path=/; HttpOnly; SameSite=Strict" + (keep ? "; Max-Age=31536000" : ""));
}

// ======================================================================
//  Time
// ======================================================================
void timeSyncCb(struct timeval*) { ntpSynced = true; }

void setSystemUtc(time_t t) {
  struct timeval tv = { t, 0 };
  settimeofday(&tv, nullptr);
}

void writeRtc(time_t t) {
  if (!rtcOK) return;
  rtc.adjust(DateTime((uint32_t)t));
  rtc.start();
}

String isoLocal(time_t t) {
  struct tm tm; localtime_r(&t, &tm);
  char b[32]; strftime(b, sizeof b, "%Y-%m-%dT%H:%M:%S%z", &tm);   // ...-0800
  size_t n = strlen(b);
  if (n >= 5) { memmove(b + n - 1, b + n - 2, 3); b[n - 2] = ':'; } // ...-08:00
  return String(b);
}

String isoUtc(time_t t) {
  struct tm tm; gmtime_r(&t, &tm);
  char b[24]; strftime(b, sizeof b, "%Y-%m-%dT%H:%M:%SZ", &tm);
  return String(b);
}

String localDate(time_t t) {
  struct tm tm; localtime_r(&t, &tm);
  char b[12]; strftime(b, sizeof b, "%Y-%m-%d", &tm);
  return String(b);
}
String trackDate(time_t t) { return localDate(t); }   // a track day is a calendar day in the box's time zone

// ======================================================================
//  Storage: flash (primary) + SD (mirror / climate archive)
// ======================================================================
size_t fileSize(fs::FS& fs, const char* path) {
  if (!fs.exists(path)) return 0;
  File f = fs.open(path, FILE_READ);
  if (!f) return 0;
  size_t n = f.size();
  f.close();
  return n;
}

bool copyFile(fs::FS& from, fs::FS& to, const char* path) {
  File a = from.open(path, FILE_READ);
  if (!a) return false;
  File b = to.open(path, FILE_WRITE);
  if (!b) { a.close(); return false; }
  uint8_t buf[512];
  bool ok = true;
  while (true) {
    int n = a.read(buf, sizeof buf);
    if (n <= 0) break;
    if (b.write(buf, n) != (size_t)n) { ok = false; break; }
  }
  a.close(); b.close();
  return ok;
}

void scanLog() {
  logNextSeq = 1; logBytes = 0;
  if (!store) return;
  File f = store->open(LOG_PATH, FILE_READ);
  if (!f) return;
  uint8_t buf[512];
  uint32_t lines = 0; int last = -1;
  while (true) {
    int n = f.read(buf, sizeof buf);
    if (n <= 0) break;
    for (int i = 0; i < n; i++) if (buf[i] == '\n') lines++;
    last = buf[n - 1];
  }
  logBytes = f.size();
  f.close();
  // A line torn by a power cut gets terminated so the next event starts clean;
  // the dashboard skips any line it can't parse.
  if (logBytes > 0 && last != '\n') {
    File a = store->open(LOG_PATH, FILE_APPEND);
    if (a) { a.print('\n'); a.close(); lines++; logBytes++; }
  }
  logNextSeq = lines + 1;
}

void chooseStore() {
  if (flashOK)    { store = &LittleFS; storeName = "flash"; }
  else if (sdOK)  { store = &SD;       storeName = "sd"; }
  else            { store = nullptr;   storeName = "none"; }
  scanLog();
}

void ensureDirs(fs::FS& fs) {
  if (!fs.exists("/hwo")) fs.mkdir("/hwo");
  if (!fs.exists("/env")) fs.mkdir("/env");
  if (!fs.exists("/hwo/doc")) fs.mkdir("/hwo/doc");
  if (!fs.exists("/hwo/cl")) fs.mkdir("/hwo/cl");
  if (!fs.exists("/hwo/cx")) fs.mkdir("/hwo/cx");
  if (!fs.exists("/hwo/ics")) fs.mkdir("/hwo/ics");
  if (!fs.exists("/hwo/nt")) fs.mkdir("/hwo/nt");
  if (!fs.exists("/hwo/na")) fs.mkdir("/hwo/na");
  if (!fs.exists("/hwo/nh")) fs.mkdir("/hwo/nh");
  if (!fs.exists("/gps")) fs.mkdir("/gps");
}

bool startFlash() {
  if (!LittleFS.begin(true)) return false;     // formats on first use
  ensureDirs(LittleFS);
  return true;
}

void moveDirToSd(const char* dir, bool append) {
  File d = LittleFS.open(dir);
  if (!d) return;
  String names[40]; int n = 0;
  File e = d.openNextFile();
  while (e && n < 40) { String nm = e.name(); int sl = nm.lastIndexOf('/'); if (sl >= 0) nm = nm.substring(sl + 1); bool dirE = e.isDirectory(); e.close(); if (!dirE) names[n++] = nm; e = d.openNextFile(); }
  d.close();
  if (!SD.exists(dir)) SD.mkdir(dir);
  for (int i = 0; i < n; i++) {
    String p = String(dir) + "/" + names[i];
    bool ok;
    if (append && SD.exists(p)) {
      File a = LittleFS.open(p, FILE_READ), b = SD.open(p, FILE_APPEND);
      ok = a && b;
      if (ok) a.readStringUntil('\n');                            // skip the header line
      uint8_t buf[512];
      while (ok) { int k = a.read(buf, sizeof buf); if (k <= 0) break; if (b.write(buf, k) != (size_t)k) ok = false; }
      if (a) a.close(); if (b) b.close();
    } else ok = copyFile(LittleFS, SD, p.c_str());
    if (ok) LittleFS.remove(p);
  }
}

// file names (not paths) in one folder
int listNames(fs::FS& fs, const char* dir, String* names, int max) {
  File d = fs.open(dir); if (!d) return 0;
  int n = 0; File e = d.openNextFile();
  while (e && n < max) { String nm = e.name(); int sl = nm.lastIndexOf('/'); if (sl >= 0) nm = nm.substring(sl + 1); bool dirE = e.isDirectory(); e.close(); if (!dirE && nm.endsWith(".json")) names[n++] = nm; e = d.openNextFile(); }
  if (e) e.close();
  d.close();
  return n;
}
void syncNotes() {
  static String nf[48], ns[48];
  int a = listNames(LittleFS, "/hwo/nt", nf, 48);
  if (a == 0) { int b = listNames(SD, "/hwo/nt", ns, 48); for (int i = 0; i < b; i++) copyFile(SD, LittleFS, (String("/hwo/nt/") + ns[i]).c_str()); }
  else { if (!SD.exists("/hwo/nt")) SD.mkdir("/hwo/nt"); for (int i = 0; i < a; i++) copyFile(LittleFS, SD, (String("/hwo/nt/") + nf[i]).c_str()); }
}

// Card just mounted: make flash and card agree
void syncSdMirror() {
  if (!flashOK || !sdOK) return;
  size_t f = fileSize(LittleFS, LOG_PATH), s = fileSize(SD, LOG_PATH);
  if (f == 0 && s > 0) {                        // new/erased hub board: restore from the card
    if (copyFile(SD, LittleFS, LOG_PATH)) Serial.println("Log restored from SD card");
    scanLog();
  } else if (f != s) {                          // card is behind (or a different card): refresh it
    copyFile(LittleFS, SD, LOG_PATH);
  }
  // checklist: restore from the card if flash has none, otherwise keep the card's copy current
  const char* CLP = "/hwo/doc/checklist.json";
  if (!LittleFS.exists(CLP) && SD.exists(CLP)) copyFile(SD, LittleFS, CLP);
  else if (LittleFS.exists(CLP)) copyFile(LittleFS, SD, CLP);
  syncNotes();
  // files kept in flash while there was no card: move them to the card
  moveDirToSd("/hwo/cl", false);
  moveDirToSd("/hwo/na", false);
  moveDirToSd("/hwo/nh", false);
  moveDirToSd("/hwo/ics", false);
  moveDirToSd("/gps", true);       // a day's track may already be on the card: append
}

bool startSD() {
  if (!SD.begin(SD_CS)) return false;
  ensureDirs(SD);
  return true;
}

// Append event lines (one JSON object per line). Returns how many were written.
int appendEvents(const String& body, String& tsOut, uint32_t& firstOut) {
  if (!store) return -1;
  if (store == &LittleFS && LittleFS.totalBytes() - LittleFS.usedBytes() < body.length() * 2 + 16 * 1024) return -2;   // full: refuse, don't tear
  File f = store->open(LOG_PATH, FILE_APPEND);
  if (!f) return -1;
  File m;
  bool mirror = (store == &LittleFS) && sdOK;
  if (mirror) { m = SD.open(LOG_PATH, FILE_APPEND); if (!m) { sdOK = false; mirror = false; } }

  tsOut = isoLocal(time(nullptr));
  firstOut = logNextSeq;
  int count = 0, start = 0; bool bad = false;
  while (start < (int)body.length()) {
    int nl = body.indexOf('\n', start);
    if (nl < 0) nl = body.length();
    String line = body.substring(start, nl);
    start = nl + 1;
    line.trim();
    line.replace("\r", " ");
    if (line.length() < 2 || line[0] != '{' || line[line.length() - 1] != '}') continue;
    String rec = "{\"seq\":" + String(logNextSeq) + ",\"ts\":\"" + tsOut + "\",\"e\":" + line + "}\n";
    if (f.print(rec) != rec.length()) { bad = true; break; }
    if (mirror) m.print(rec);
    logNextSeq++;
    count++;
  }
  f.flush();
  logBytes = f.size();
  f.close();
  if (mirror) m.close();
  if (bad) { scanLog(); return -2; }                     // ends a torn line and recounts
  return count;
}

// ======================================================================
//  EEPROM heartbeat -> power-off record
// ======================================================================
uint16_t crc16(const uint8_t* d, size_t n) {
  uint16_t c = 0xFFFF;
  while (n--) { c ^= (uint16_t)(*d++) << 8; for (int i = 0; i < 8; i++) c = (c & 0x8000) ? (c << 1) ^ 0x1021 : c << 1; }
  return c;
}

bool eeRead(uint16_t a, uint8_t* b, size_t n) {
  while (n) {
    size_t c = n > 32 ? 32 : n;
    Wire.beginTransmission(EE_ADDR);
    Wire.write((uint8_t)(a >> 8)); Wire.write((uint8_t)(a & 0xFF));
    if (Wire.endTransmission(false) != 0) return false;
    if (Wire.requestFrom(EE_ADDR, c) != c) return false;
    for (size_t i = 0; i < c; i++) b[i] = Wire.read();
    a += c; b += c; n -= c;
  }
  return true;
}

// 16 bytes at a 16-byte-aligned address never cross an EEPROM page (32/64/128 B)
bool eeWrite16(uint16_t a, const uint8_t* b) {
  Wire.beginTransmission(EE_ADDR);
  Wire.write((uint8_t)(a >> 8)); Wire.write((uint8_t)(a & 0xFF));
  Wire.write(b, 16);
  if (Wire.endTransmission() != 0) return false;
  uint32_t t0 = millis();
  while (millis() - t0 < 20) {               // ACK polling: done when the chip answers again
    Wire.beginTransmission(EE_ADDR);
    if (Wire.endTransmission() == 0) return true;
    delay(1);
  }
  return false;
}

void eeStart() {
  Wire.beginTransmission(EE_ADDR);
  eeOK = (Wire.endTransmission() == 0);
  if (!eeOK) return;
  uint8_t buf[HB_SLOT * 16];
  int best = -1;
  for (uint16_t blk = 0; blk < HB_SLOTS; blk += 16) {
    if (!eeRead(HB_BASE + blk * HB_SLOT, buf, sizeof buf)) { eeOK = false; return; }
    for (int i = 0; i < 16; i++) {
      HbSlot s; memcpy(&s, buf + i * HB_SLOT, HB_SLOT);
      if (s.magic != HB_MAGIC || s.crc != crc16((uint8_t*)&s, 12)) continue;
      if (best < 0 || s.seq > hbSeq) { best = blk + i; hbSeq = s.seq; lastAlive = s.epoch; }
    }
  }
  hbNext = (best < 0) ? 0 : (best + 1) % HB_SLOTS;
}

void heartbeatTick() {
  if (!eeOK || !timeValid || !outageHandled) return;   // never overwrite an outage not yet logged
  if (lastHbMs && millis() - lastHbMs < 60000UL) return;
  lastHbMs = millis();
  HbSlot s;
  s.seq = ++hbSeq; s.epoch = (uint32_t)time(nullptr); s.upS = millis() / 1000; s.magic = HB_MAGIC;
  s.crc = crc16((uint8_t*)&s, 12);
  if (eeWrite16(HB_BASE + hbNext * HB_SLOT, (uint8_t*)&s)) hbNext = (hbNext + 1) % HB_SLOTS;
}

const char* resetName(esp_reset_reason_t r) {
  switch (r) {
    case ESP_RST_POWERON:  return "power-on";
    case ESP_RST_BROWNOUT: return "brownout";
    case ESP_RST_SW:       return "software";
    case ESP_RST_PANIC:    return "crash";
    case ESP_RST_INT_WDT:
    case ESP_RST_TASK_WDT:
    case ESP_RST_WDT:      return "watchdog";
    case ESP_RST_DEEPSLEEP:return "deep sleep";
    case ESP_RST_EXT:      return "reset pin";
    default:               return "other";
  }
}

// Runs once, the first time the clock is valid after boot
void outageCheck() {
  if (outageHandled || !timeValid) return;
  if (!store) return;                                  // wait until it can be written down
  outageHandled = true;
  if (!eeOK || lastAlive < 1735689600UL) return;       // nothing recorded yet
  uint32_t boot = (uint32_t)time(nullptr) - millis() / 1000;
  if (boot <= lastAlive || boot - lastAlive < OUTAGE_MIN_S) return;
  const char* op = nullptr;
  if (resetReason == ESP_RST_POWERON || resetReason == ESP_RST_BROWNOUT) op = "power.out";
  else if (resetReason == ESP_RST_PANIC || resetReason == ESP_RST_INT_WDT ||
           resetReason == ESP_RST_TASK_WDT || resetReason == ESP_RST_WDT) op = "hub.reset";
  if (!op) return;
  String ev = String("{\"op\":\"") + op + "\",\"from\":\"" + isoLocal(lastAlive) + "\",\"to\":\"" + isoLocal(boot) +
              "\",\"min\":" + String((boot - lastAlive + 30) / 60) + ",\"cause\":\"" + resetName(resetReason) + "\"}";
  String ts; uint32_t first;
  appendEvents(ev, ts, first);
  Serial.println("Logged: " + ev);
}

// ======================================================================
//  Nodes
// ======================================================================
void cleanName(char* dst, const char* src) {
  int i = 0;
  for (; i < 11 && src[i]; i++) {
    char c = src[i];
    dst[i] = (isalnum((unsigned char)c) || c == ' ' || c == '-' || c == '_') ? c : '_';
  }
  dst[i] = 0;
  if (i == 0) strcpy(dst, "Node");
}

int findNode(const char* name, bool create) {
  int freeSlot = -1, oldest = -1;
  for (int i = 0; i < MAX_NODES; i++) {
    if (nodes[i].used && strcmp(nodes[i].name, name) == 0) return i;
    if (!nodes[i].used && freeSlot < 0) freeSlot = i;
    if (nodes[i].used && (oldest < 0 || nodes[i].lastEpoch < nodes[oldest].lastEpoch)) oldest = i;
  }
  if (!create) return -1;
  int slot = (freeSlot >= 0) ? freeSlot : oldest;
  memset(&nodes[slot], 0, sizeof(Node));
  nodes[slot].used = true;
  strncpy(nodes[slot].name, name, 11);
  return slot;
}

void updateNode(const char* name, const uint8_t* mac, float t, float h, float v, float soc,
                int8_t rssi, uint32_t seq, uint16_t interval, uint8_t flags) {
  Node& n = nodes[findNode(name, true)];
  memcpy(n.mac, mac, 6);
  n.tempC = t; n.rh = h; n.vbat = v; n.soc = soc;
  n.rssi = rssi; n.seq = seq; n.interval = interval ? interval : 60; n.flags = flags;
  n.lastMs = millis(); n.lastEpoch = timeValid ? (uint32_t)time(nullptr) : 0; n.count++; n.live = true;
}

bool nodeFresh(const Node& n) {
  uint32_t limit = max<uint32_t>(90, 3UL * n.interval) * 1000UL;
  return n.used && n.live && (millis() - n.lastMs) < limit;
}

uint32_t nodeAge(const Node& n) {
  if (n.live) return (millis() - n.lastMs) / 1000;
  uint32_t now = (uint32_t)time(nullptr);
  return (timeValid && n.lastEpoch && now > n.lastEpoch) ? now - n.lastEpoch : 0xFFFFFFFFUL;
}

#if ESP_IDF_VERSION_MAJOR >= 5
static void onEspNowRecv(const esp_now_recv_info_t* info, const uint8_t* data, int len) {
  const uint8_t* mac = info->src_addr;
  int8_t rssi = info->rx_ctrl ? info->rx_ctrl->rssi : 0;
#else
static void onEspNowRecv(const uint8_t* mac, const uint8_t* data, int len) {
  int8_t rssi = 0;
#endif
  if (len < 4 || len > 240) return;
  uint32_t magic; memcpy(&magic, data, 4);
  if (magic != HWO_MAGIC && magic != HWO_BFILL) return;
  portENTER_CRITICAL(&rxMux);
  uint8_t next = (rxHead + 1) & 7;
  if (next != rxTail) {
    memcpy(rxq[rxHead].d, data, len);
    rxq[rxHead].len = len;
    memcpy(rxq[rxHead].mac, mac, 6);
    rxq[rxHead].rssi = rssi;
    rxHead = next;
  }
  portEXIT_CRITICAL(&rxMux);
}

// Reply to a node. Goes out on whichever interface is on the channel the node used.
void espNowSend(const uint8_t* mac, const void* d, size_t n) {
  wifi_interface_t want = staUp ? WIFI_IF_STA : WIFI_IF_AP;
  esp_now_peer_info_t p = {};
  if (esp_now_get_peer(mac, &p) == ESP_OK) {
    if (p.ifidx != want) { p.ifidx = want; p.channel = 0; esp_now_mod_peer(&p); }
  } else {
    memcpy(p.peer_addr, mac, 6); p.channel = 0; p.ifidx = want; p.encrypt = false;
    if (esp_now_add_peer(&p) != ESP_OK) return;
  }
  esp_now_send(mac, (const uint8_t*)d, n);
}

String csvNum(float v, int dec) { return isnan(v) ? String("") : String(v, dec); }
fs::FS* envFS() { return sdOK ? (fs::FS*)&SD : (flashOK ? (fs::FS*)&LittleFS : nullptr); }
const char* ENV_HEAD = "utc,local,node,temp_c,rh_pct,vbat_v,soc_pct,rssi_dbm";

// Readings a node kept while the box was away. Only with an SD card: months of them won't fit in flash.
bool storeBackfill(const char* name, const BfPacket& bp) {
  if (!sdOK || !timeValid) return false;
  String openDay; File f;
  for (int i = 0; i < bp.n; i++) {
    const BfRec& r = bp.r[i];
    if (r.epoch < 1735689600UL || r.epoch > (uint32_t)time(nullptr) + 600) continue;   // not a sane time: dropped
    String day = localDate(r.epoch);
    if (day != openDay) {
      if (f) f.close();
      String path = "/env/" + day + ".csv";
      bool isNew = !SD.exists(path);
      f = SD.open(path, FILE_APPEND);
      if (!f) { sdOK = false; return false; }
      if (isNew) f.println(ENV_HEAD);
      openDay = day;
    }
    f.print(isoUtc(r.epoch)); f.print(','); f.print(isoLocal(r.epoch)); f.print(','); f.print(name); f.print(',');
    if (r.t100 != INT16_MIN) f.print(String(r.t100 / 100.0f, 2)); f.print(',');
    if (r.rh100 != 0xFFFF) f.print(String(r.rh100 / 100.0f, 1)); f.print(",,");
    if (r.soc != 255) f.print(r.soc); f.println(',');
  }
  if (f) f.close();
  return true;
}

void processRx() {
  while (true) {
    RxItem it;
    portENTER_CRITICAL(&rxMux);
    if (rxTail == rxHead) { portEXIT_CRITICAL(&rxMux); return; }
    memcpy(&it, &rxq[rxTail], sizeof(RxItem));
    rxTail = (rxTail + 1) & 7;
    portEXIT_CRITICAL(&rxMux);

    uint32_t magic; memcpy(&magic, it.d, 4);
    if (magic == HWO_MAGIC && (it.len == sizeof(EnvPacket) || it.len == sizeof(EnvPacketV2))) {
      EnvPacket p; memcpy(&p, it.d, sizeof(EnvPacket));
      char raw[12]; memcpy(raw, p.name, 11); raw[11] = 0;
      char nm[12]; cleanName(nm, raw);
      updateNode(nm, it.mac, p.tempC, p.rh, p.vbat, p.soc, it.rssi, p.seq, p.intervalS, p.flags);
      if (it.len == sizeof(EnvPacketV2)) {               // v2 node: give it the time; say if it can send in its buffer
        TimePacket tp = { HWO_TIME, timeValid ? (uint32_t)time(nullptr) : 0, (uint8_t)((timeValid ? 1 : 0) | (sdOK && timeValid ? 2 : 0)) };
        espNowSend(it.mac, &tp, sizeof tp);
      }
    } else if (magic == HWO_BFILL && it.len >= BF_HEAD && it.len <= sizeof(BfPacket)) {
      BfPacket bp = {}; memcpy(&bp, it.d, it.len);
      if (bp.n > BF_MAX || it.len != BF_HEAD + bp.n * sizeof(BfRec)) continue;
      char raw[12]; memcpy(raw, bp.name, 11); raw[11] = 0;
      char nm[12]; cleanName(nm, raw);
      Node& n = nodes[findNode(nm, true)];
      bool ok = true;
      if (!(n.bfAny && n.bfSeq == bp.bseq)) {             // a resent batch (our ack got lost) is not written twice
        ok = storeBackfill(nm, bp);
        if (ok) { n.bfSeq = bp.bseq; n.bfAny = true; n.bfRows += bp.n; }
      }
      BfAck a = { HWO_BACK, bp.bseq, (uint8_t)(ok ? 1 : 0) };
      espNowSend(it.mac, &a, sizeof a);
    }
  }
}

// At boot the box has heard nobody yet: show each node's last archived reading, with its age
uint32_t parseUtc(const String& s) {   // 2026-11-17T16:40:00Z
  if (s.length() < 20) return 0;
  int y = s.substring(0, 4).toInt(), mo = s.substring(5, 7).toInt(), d = s.substring(8, 10).toInt();
  int h = s.substring(11, 13).toInt(), mi = s.substring(14, 16).toInt(), se = s.substring(17, 19).toInt();
  y -= mo <= 2;
  int era = y / 400, yoe = y - era * 400, doy = (153 * (mo + (mo > 2 ? -3 : 9)) + 2) / 5 + d - 1;
  long days = era * 146097L + yoe * 365L + yoe / 4 - yoe / 100 + doy - 719468L;
  return (uint32_t)(days * 86400L + h * 3600L + mi * 60L + se);
}

void seedNodesFrom(fs::FS& fs) {
  File d = fs.open("/env");
  if (!d) return;
  String newest; File e = d.openNextFile();
  while (e) {
    String nm = e.name(); int sl = nm.lastIndexOf('/'); if (sl >= 0) nm = nm.substring(sl + 1);
    e.close();
    if (nm.endsWith(".csv") && nm.length() == 14 && nm > newest) newest = nm;
    e = d.openNextFile();
  }
  d.close();
  if (!newest.length()) return;
  File f = fs.open("/env/" + newest, FILE_READ);
  if (!f) return;
  if (f.size() > 6000) f.seek(f.size() - 6000);
  String line;
  while (f.available()) {
    char c = f.read();
    if (c != '\n') { if (line.length() < 120) line += c; continue; }
    // utc,local,node,temp_c,rh_pct,vbat_v,soc_pct,rssi
    String col[8]; int k = 0, st = 0;
    for (int i = 0; i <= (int)line.length() && k < 8; i++) if (i == (int)line.length() || line[i] == ',') { col[k++] = line.substring(st, i); st = i + 1; }
    line = "";
    if (k < 5 || !col[0].endsWith("Z")) continue;
    uint32_t ep = parseUtc(col[0]);
    char nm[12]; cleanName(nm, col[2].c_str());
    int idx = findNode(nm, true);
    Node& n = nodes[idx];
    if (n.live || ep < n.lastEpoch) continue;
    n.lastEpoch = ep;
    n.tempC = col[3].length() ? col[3].toFloat() : NAN;
    n.rh = col[4].length() ? col[4].toFloat() : NAN;
    n.vbat = (k > 5 && col[5].length()) ? col[5].toFloat() : NAN;
    n.soc = (k > 6 && col[6].length()) ? col[6].toFloat() : NAN;
    n.flags = 1; n.interval = 60;
  }
  f.close();
}
void seedNodes() { if (sdOK) seedNodesFrom(SD); else if (flashOK) seedNodesFrom(LittleFS); }

// ======================================================================
//  Climate log
// ======================================================================
// Flash only: keep ENV_FLASH_DAYS of climate files
void pruneFlashDir(const char* dir);
void pruneFlashEnv(const String& today) { pruneFlashDir("/env"); pruneFlashDir("/gps"); }
void pruneFlashDir(const char* dir) {
  if (!flashOK) return;
  time_t cut = time(nullptr) - (time_t)ENV_FLASH_DAYS * 86400;
  String cutDay = localDate(cut);
  File d = LittleFS.open(dir);
  if (!d) return;
  String doomed[40]; int nd = 0;
  File e = d.openNextFile();
  while (e) {
    String nm = e.name();
    int sl = nm.lastIndexOf('/');
    if (sl >= 0) nm = nm.substring(sl + 1);
    e.close();
    if (nm.endsWith(".csv") && nm.substring(0, 10) < cutDay && nd < 40) doomed[nd++] = String(dir) + "/" + nm;
    e = d.openNextFile();
  }
  d.close();
  for (int i = 0; i < nd; i++) LittleFS.remove(doomed[i]);
}

void envLogTick() {
  if (!timeValid) return;
  fs::FS* fs = envFS();
  if (!fs) return;
  time_t t = time(nullptr);
  uint32_t slot = (uint32_t)t / ENV_LOG_S;
  if (slot == lastEnvSlot) return;
  bool first = (lastEnvSlot == 0);
  lastEnvSlot = slot;
  if (first) return;                       // wait for the first clean boundary

  String day = localDate(t);
  if (day != lastPruneDay) { lastPruneDay = day; pruneFlashEnv(day); }

  String path = "/env/" + day + ".csv";
  bool isNew = !fs->exists(path);
  File f = fs->open(path, FILE_APPEND);
  if (!f) { if (fs == &SD) sdOK = false; return; }
  if (isNew) f.println(ENV_HEAD);
  String u = isoUtc(t), l = isoLocal(t);
  for (int i = 0; i < MAX_NODES; i++) {
    const Node& n = nodes[i];
    if (!nodeFresh(n)) continue;           // stale node = gap in the file, never filled
    f.print(u); f.print(','); f.print(l); f.print(','); f.print(n.name); f.print(',');
    f.print(csvNum(n.tempC, 2)); f.print(','); f.print(csvNum(n.rh, 1)); f.print(',');
    f.print(csvNum(n.vbat, 3)); f.print(','); f.print(csvNum(n.soc, 1)); f.print(',');
    f.println(n.rssi);
  }
  f.close();
}

void hubSensorTick() {
  if (!hubSht || millis() - lastHubRead < HUB_SENSOR_S * 1000UL) return;
  lastHubRead = millis();
  float t = NAN, h = NAN;
  bool ok = sht.readBoth(&t, &h);
  uint8_t mac[6]; WiFi.macAddress(mac);
  static uint32_t s = 0;
  updateNode("Hub", mac, t, h, NAN, NAN, 0, ++s, HUB_SENSOR_S, ok ? 1 : 0);
}

// ======================================================================
//  Battery (MAX17048 on newer Feathers, LC709203F on older ones)
// ======================================================================
uint8_t battType = 0;            // 0 none, 1 MAX17048, 2 LC709203F
float battV = NAN, battPct = NAN, battRate = NAN;
uint32_t lastBattMs = 0;

void battStart() {
  if (gaugeMax.begin()) battType = 1;
  else if (gaugeLc.begin()) { battType = 2; gaugeLc.setPackSize(LC709203F_APA_2000MAH); }
}
void battTick() {
  if (!battType || (lastBattMs && millis() - lastBattMs < BATT_READ_S * 1000UL)) return;
  lastBattMs = millis();
  if (battType == 1) { battV = gaugeMax.cellVoltage(); battPct = gaugeMax.cellPercent(); battRate = gaugeMax.chargeRate(); }
  else { battV = gaugeLc.cellVoltage(); battPct = gaugeLc.cellPercent(); battRate = NAN; }
  if (battPct > 100.0f) battPct = 100.0f;
}

// ======================================================================
//  GPS (Ultimate GPS FeatherWing on TX/RX) -> clock + track
// ======================================================================
struct GpsState {
  bool seen = false;             // NMEA arriving at all
  uint32_t lastNmeaMs = 0;
  bool fix = false;
  uint32_t lastFixMs = 0;
  double lat = 0, lon = 0;
  float alt = NAN, hdop = NAN, kn = NAN;
  uint8_t sats = 0, q = 0;
  uint32_t fixEpoch = 0;
  // track logging
  uint32_t ptEpoch = 0;
  double ptLat = 0, ptLon = 0;
  bool newSeg = true;
  uint32_t ptsToday = 0;
  String ptsDay;
  uint32_t lastClockSet = 0;
} gps;
char nmea[100];
uint8_t nmeaLen = 0;

bool gpsI2C = false;
uint32_t lastGpsPoll = 0, lastGpsProbe = 0;
void gpsSend(const char* body) {               // adds $ and the checksum
  uint8_t c = 0; for (const char* p = body; *p; p++) c ^= *p;
  char b[100]; snprintf(b, sizeof b, "$%s*%02X\r\n", body, c);
  Wire.beginTransmission(GPS_ADDR); Wire.write((const uint8_t*)b, strlen(b)); Wire.endTransmission();
}
void gpsStart() {
  Wire.beginTransmission(GPS_ADDR);
  gpsI2C = (Wire.endTransmission() == 0);
  lastGpsProbe = millis();
  if (!gpsI2C) return;
  delay(20);
  gpsSend("PMTK314,0,1,0,1,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0");   // RMC + GGA only
  gpsSend("PMTK220,1000");                                    // 1 Hz
}

double nmeaDeg(const char* v, const char* hemi) {   // ddmm.mmmm -> degrees
  if (!*v) return NAN;
  double x = atof(v); int deg = (int)(x / 100); double d = deg + (x - deg * 100) / 60.0;
  return (*hemi == 'S' || *hemi == 'W') ? -d : d;
}
uint32_t civilEpoch(int y, int mo, int d, int h, int mi, int se) {
  y -= mo <= 2;
  int era = y / 400, yoe = y - era * 400, doy = (153 * (mo + (mo > 2 ? -3 : 9)) + 2) / 5 + d - 1;
  long days = era * 146097L + yoe * 365L + yoe / 4 - yoe / 100 + doy - 719468L;
  return (uint32_t)(days * 86400L + h * 3600L + mi * 60L + se);
}
double distM(double a1, double o1, double a2, double o2) {
  const double R = 6371000.0, k = PI / 180.0;
  double dA = (a2 - a1) * k, dO = (o2 - o1) * k;
  double h = sin(dA / 2) * sin(dA / 2) + cos(a1 * k) * cos(a2 * k) * sin(dO / 2) * sin(dO / 2);
  return 2 * R * asin(sqrt(h));
}

fs::FS* gpsFS() { return sdOK ? (fs::FS*)&SD : (flashOK ? (fs::FS*)&LittleFS : nullptr); }

void gpsLogPoint(uint32_t ep) {
  fs::FS* fs = gpsFS();
  if (!fs) return;
  if (fs == &LittleFS && LittleFS.totalBytes() - LittleFS.usedBytes() < 200 * 1024) return;   // keep room for the log
  String day = trackDate(ep);
  if (!fs->exists("/gps")) fs->mkdir("/gps");
  String path = "/gps/" + day + ".csv";
  bool isNew = !fs->exists(path);
  File f = fs->open(path, FILE_APPEND);
  if (!f) { if (fs == &SD) sdOK = false; return; }
  if (isNew) f.println("utc,lat,lon,alt_m,kn,sats,hdop,seg");
  tripAddPoint(ep, gps.lat, gps.lon);
  f.print(isoUtc(ep)); f.print(','); f.print(String(gps.lat, 6)); f.print(','); f.print(String(gps.lon, 6)); f.print(',');
  f.print(csvNum(gps.alt, 1)); f.print(','); f.print(csvNum(gps.kn, 1)); f.print(','); f.print(gps.sats); f.print(',');
  f.print(csvNum(gps.hdop, 1)); f.print(','); f.println(gps.newSeg ? 1 : 0);
  f.close();
  if (day != gps.ptsDay) { gps.ptsDay = day; gps.ptsToday = 0; }
  gps.ptsToday++;
  gps.ptEpoch = ep; gps.ptLat = gps.lat; gps.ptLon = gps.lon; gps.newSeg = false;
}

void gpsSentence(char* line) {
  // checksum
  char* star = strrchr(line, '*');
  if (!star || line[0] != '$') return;
  uint8_t c = 0; for (char* p = line + 1; p < star; p++) c ^= *p;
  if (strtol(star + 1, nullptr, 16) != c) return;
  *star = 0;
  char* f[20]; int n = 0;
  for (char* p = line; n < 20; ) { f[n++] = p; char* q = strchr(p, ','); if (!q) break; *q = 0; p = q + 1; }
  gps.seen = true; gps.lastNmeaMs = millis();
  const char* id = f[0] + 3;
  if (!strcmp(id, "GGA") && n >= 10) {
    gps.q = atoi(f[6]); gps.sats = atoi(f[7]);
    gps.hdop = *f[8] ? atof(f[8]) : NAN;
    gps.alt = *f[9] ? atof(f[9]) : NAN;
  } else if (!strcmp(id, "RMC") && n >= 10) {
    bool ok = f[2][0] == 'A' && strlen(f[1]) >= 6 && strlen(f[9]) == 6;
    if (!ok) { gps.fix = false; return; }
    if (millis() - gps.lastFixMs > 10000) gps.newSeg = true;      // fix was lost: the next point starts a new line
    gps.fix = true; gps.lastFixMs = millis();
    gps.lat = nmeaDeg(f[3], f[4]); gps.lon = nmeaDeg(f[5], f[6]);
    gps.kn = *f[7] ? atof(f[7]) : NAN;
    const char *t = f[1], *d = f[9];
    uint32_t ep = civilEpoch(2000 + (d[4] - '0') * 10 + (d[5] - '0'), (d[2] - '0') * 10 + (d[3] - '0'), (d[0] - '0') * 10 + (d[1] - '0'),
                             (t[0] - '0') * 10 + (t[1] - '0'), (t[2] - '0') * 10 + (t[3] - '0'), (t[4] - '0') * 10 + (t[5] - '0'));
    gps.fixEpoch = ep;
    // clock: GPS wins over the RTC and the browser; NTP (house Wi-Fi) wins over GPS
    bool ntpFresh = !strcmp(timeSrc, "ntp") && lastNtpSync && millis() - lastNtpSync < 6UL * 3600000UL;
    if (ep > 1735689600UL && !ntpFresh) {
      long diff = (long)ep - (long)time(nullptr);
      if (!timeValid || ((diff > 1 || diff < -1) && (!gps.lastClockSet || millis() - gps.lastClockSet > 600000UL)) || strcmp(timeSrc, "gps")) {
        setSystemUtc((time_t)ep); writeRtc((time_t)ep);
        if (!timeValid || diff > 60 || diff < -60) lastEnvSlot = 0;
        timeValid = true; timeSrc = "gps"; gps.lastClockSet = millis();
      }
    }
    // track
    if (!timeValid || isnan(gps.lat) || isnan(gps.lon) || gps.q == 0) return;
    if (!isnan(gps.hdop) && gps.hdop > GPS_MAX_HDOP) return;
    uint32_t now = (uint32_t)time(nullptr);
    double m = gps.ptEpoch ? distM(gps.ptLat, gps.ptLon, gps.lat, gps.lon) : 1e9;
    if (gps.newSeg || !gps.ptEpoch || (now - gps.ptEpoch >= GPS_LOG_S && m >= GPS_MOVE_M) || now - gps.ptEpoch >= GPS_STILL_S) gpsLogPoint(now);
  }
}

// The PA1010D answers I2C reads with its NMEA stream, padded with 0x0A when it has nothing new
void gpsTick() {
  if (!gpsI2C) { if (millis() - lastGpsProbe > 10000) gpsStart(); return; }
  if (millis() - lastGpsPoll < 80) return;
  lastGpsPoll = millis();
  for (int chunk = 0; chunk < 8; chunk++) {
    int n = Wire.requestFrom(GPS_ADDR, (uint8_t)32);
    if (n <= 0) { if (millis() - gps.lastNmeaMs > 15000) { gpsI2C = false; lastGpsProbe = millis(); } return; }
    bool any = false;
    while (Wire.available()) {
      char c = Wire.read();
      if (c != 0x0A) any = true;
      if (c == '\n' || c == '\r') { if (nmeaLen) { nmea[nmeaLen] = 0; gpsSentence(nmea); nmeaLen = 0; } }
      else if ((uint8_t)c >= 0x20 && nmeaLen < sizeof(nmea) - 1) nmea[nmeaLen++] = c;
      else if ((uint8_t)c >= 0x20) nmeaLen = 0;
    }
    if (!any) break;
  }
  if (gps.fix && millis() - gps.lastFixMs > 5000) gps.fix = false;
}

// ======================================================================
//  Wi-Fi / ESP-NOW
// ======================================================================
void onWiFiEvent(WiFiEvent_t ev) {
  if (ev == ARDUINO_EVENT_WIFI_STA_DISCONNECTED && staUp) staLost = true;
}

void onStaConnected() {
  staUp = true;
  if (AP_ALWAYS) apUp = WiFi.softAP(cfg.apSsid.c_str(), cfg.apPass.c_str(), WiFi.channel());
  else { WiFi.softAPdisconnect(true); apUp = false; }
  sntp_set_time_sync_notification_cb(timeSyncCb);
  configTzTime(cfg.tz.c_str(), "pool.ntp.org", "time.nist.gov");
}

bool tryHomeWiFi(uint32_t ms) {
  WiFi.begin(cfg.homeSsid.c_str(), cfg.homePass.c_str());
  uint32_t t0 = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - t0 < ms) { delay(20); server.handleClient(); processRx(); screenTick(); }   // knob, button and alarms keep working
  if (WiFi.status() == WL_CONNECTED) return true;
  WiFi.disconnect(false);                  // stop retrying: retries hop channels and break the hotspot
  return false;
}

bool homeInScan(int n) {
  for (int i = 0; i < n; i++) if (WiFi.SSID(i) == cfg.homeSsid) return true;
  return false;
}

void startWiFi() {
  WiFi.persistent(false);
  WiFi.mode(WIFI_AP_STA);
  WiFi.setSleep(false);                    // modem sleep drops ESP-NOW packets
  WiFi.setAutoReconnect(false);            // leaving home must not make it hop channels forever
  WiFi.onEvent(onWiFiEvent);
  apUp = WiFi.softAP(cfg.apSsid.c_str(), cfg.apPass.c_str(), AP_CHANNEL);   // hotspot first: usable within seconds anywhere
  staUp = false;
  if (cfg.homeSsid.length()) {
    int n = WiFi.scanNetworks(false, false, false, 120);
    bool home = n > 0 && homeInScan(n);
    WiFi.scanDelete();
    if (home && tryHomeWiFi(15000)) onStaConnected();
  }
  lastScan = millis();
  MDNS.begin(MDNS_NAME);
  MDNS.addService("http", "tcp", 80);
}

void startEspNow() {
  if (esp_now_init() != ESP_OK) { Serial.println("ESP-NOW init failed"); return; }
  esp_now_register_recv_cb(onEspNowRecv);
}

// Away from home: look for the house Wi-Fi now and then (less often while a phone is on the hotspot,
// since a scan briefly takes the radio off the hotspot's channel). Back in range: join it.
void wifiTick() {
  if (staLost) {
    staLost = false; staUp = false;
    WiFi.disconnect(false);
    Serial.println("Left house Wi-Fi - hotspot only");
    lastScan = millis(); scanEvery = 45000;
  }
  if (cfgApplyAt && millis() > cfgApplyAt) {          // settings changed from the dashboard (after its reply went out)
    cfgApplyAt = 0;
    setenv("TZ", cfg.tz.c_str(), 1); tzset();
    if (cfgApplyAp) { cfgApplyAp = false; apUp = WiFi.softAP(cfg.apSsid.c_str(), cfg.apPass.c_str(), WiFi.channel()); }
    if (cfgApplySta) { cfgApplySta = false; if (staUp) { staUp = false; WiFi.disconnect(false); } lastScan = 0; scanEvery = 45000; }
  }
  if (staUp || !cfg.homeSsid.length()) return;
  if (scanning) {
    int n = WiFi.scanComplete();
    if (n == WIFI_SCAN_RUNNING) return;
    scanning = false;
    bool home = n > 0 && homeInScan(n);
    WiFi.scanDelete();
    lastScan = millis();
    if (home) {
      if (tryHomeWiFi(15000)) { onStaConnected(); scanEvery = 45000; Serial.println("Joined house Wi-Fi"); }
      else { apUp = WiFi.softAP(cfg.apSsid.c_str(), cfg.apPass.c_str(), WiFi.channel()); scanEvery = min<uint32_t>(scanEvery * 2, 1800000UL); }   // wrong password? back off
    }
    return;
  }
  uint32_t every = WiFi.softAPgetStationNum() > 0 ? max<uint32_t>(scanEvery, 180000UL) : scanEvery;
  if (millis() - lastScan < every) return;
  scanning = (WiFi.scanNetworks(true, false, false, 120) == WIFI_SCAN_RUNNING);
  if (!scanning) lastScan = millis();
}

// ======================================================================
//  Screen (1.5" 128x128 grayscale OLED, Adafruit 4741, SSD1327, STEMMA QT, I2C 0x3D) + knob + button
//  + buzzer (piezo 1739)
//  Screens: Logbook -> Checklist -> Today -> Notes -> Trip -> Join.
//  Knob (rotary encoder 5880, 24 clicks a turn, with a push switch):
//    turn  = move one row per click through the checklist and today's events
//    press = tick/untick the highlighted checklist item; on Notes, open the highlighted pinned note (turn to
//            scroll it) and press again for the list; on the other screens: next screen. Ticking an
//            item ticks what's nested under it, unticking unticks what it's nested under, as on the phone
//    hold 1.5 s = screen off
//  Button (lit): press = next screen. On the Logbook screen, hold it and turn the knob to set the
//    brightness (kept on the box). Its light is full while the screen is on and glows dim while asleep.
//  The screen goes to sleep after cfg.scrOffMin minutes with nothing touched (0 = never; set from the
//  dashboard). When it is asleep, the first turn or press only wakes it. Alarms wake it.
//  Without the encoder: button press = next screen, hold = tick (or open / close a note); the screen still
//  sleeps and wakes.
//  Alarms: kitchen timers (several at once, set from the dashboard), find-my-box, and the BNWAS move
//  reminder. While one is going, a press of the button or the knob silences it and does nothing else.
//  Every press, and every turn of the knob, also restarts the BNWAS period, like the reset button on a bridge.
// ======================================================================
class LbOled : public Adafruit_SSD1327 {           // exposes the frame buffer so only changed rows are sent
 public:
  LbOled() : Adafruit_SSD1327(128, 128, &Wire, -1, 400000, 400000) {}
  uint8_t* buf() { return buffer; }
  void win(int16_t y1, int16_t y2) { window_x1 = 0; window_x2 = WIDTH - 1; window_y1 = y1; window_y2 = y2; }
};
LbOled oled;
const uint8_t OLED_ADDR = 0x3D;
const uint8_t OLED_ROT  = 0;                      // 0-3: turns the picture to suit how the screen is mounted
const uint16_t C_ON = 15, C_DIM = 5, C_OFF = 0;    // grey levels (0-15)
const uint8_t BTN_EXT  = 11;                      // panel button (558): common to GND, NO to pin 11
const uint8_t BTN_LED  = 18;                      // A0: panel button LED ring +, LED - to GND (dimmed with PWM)
const uint8_t BTN_LED_CH = 4;                     // LEDC channel for the light (0 and 1 are the buzzer's)
const uint8_t LED_DIM  = 10;                      // light while the screen sleeps (0-255)
const uint8_t ENC_ADDRS[] = { 0x37, 0x38, 0x39 }; // encoder 5880: A0 closed = 0x37, A1 = 0x38, both = 0x39
                                                  // (0x36, all open, is the Feather's MAX17048 fuel gauge: only tried without one)
const int8_t  ENC_INT_PIN = -1;                   // its INT pad, if wired (e.g. 12); -1 = just read it every 20 ms
const uint8_t ENC_SW = 24;                        // the encoder's push switch, on the seesaw chip
const bool    ENC_REVERSE = false;                // scrolls the wrong way? set true
const uint16_t ENC_OFF_MS = 1500;                 // hold the knob this long: screen off
const uint8_t BUZ_A = 5, BUZ_B = 6;               // piezo (1739) wired between pins 5 and 6, driven in opposite phase
const uint16_t BUZ_HZ = 2650;                     // the piezo's loudest note
const uint16_t HOLD_MS = 600;
const int ROWS = 13;                              // text rows under the title bar

enum { S_STATUS, S_CL, S_TODAY, S_NOTES, S_TRIP, S_JOIN, S_WO, S_COUNT };
const char* SCR_KEY[] = { "status", "cl", "today", "notes", "map", "join", "wo" };   // names in the dashboard's list
uint8_t scrOrd[S_COUNT] = { S_STATUS, S_CL, S_TODAY, S_TRIP, S_JOIN };   // until the dashboard says otherwise
int scrN = 5;
std::vector<String> lbKeys, mapKeys;                     // the lines on LOGBOOK and MAP (empty: the usual ones)
String fSpd = "mph", fDist = "mi", fPos = "dd", fTemp = "F";
uint32_t mqT0 = 0; bool mqOn = false;                     // the sliding (marquee) highlighted line
bool oledOK = false, screenOn = false, bright = false;
Adafruit_seesaw enc;
bool encOK = false; uint8_t encAddr = 0;
struct EncSw { bool down, longDone, swallow; uint32_t t0; } encSw = {};
uint32_t lastInputMs = 0;   // last turn or press, for the sleep timer
uint8_t scr = S_STATUS;
uint32_t lastDrawMs = 0, msgUntil = 0, ledBlinkUntil = 0, lastEncMs = 0;
String msg;
int sel = 0;
int selMem[S_COUNT] = { 0 };
String clSelId;   // the checklist item last highlighted
bool redraw = true;

struct Btn { uint8_t pin; bool down, held, raw, swallow; uint32_t t0, chg; };
Btn btn = { BTN_EXT };

// ---- checklist rows (from the same file the dashboard edits) ----
struct ClRow { String id, text; uint8_t depth; bool done; };
std::vector<ClRow> clRows;
uint32_t clVer = 0xFFFFFFFF;
String clDay, clTitle;
int clTotal = 0, clDoneN = 0;   // the whole list, hidden items included
const char* CL_PATH = "/hwo/doc/checklist.json";
// ---- today's events and coming expiries (a summary the dashboard keeps on the box) ----
std::vector<String> todayRows;
uint32_t todayVer = 0xFFFFFFFF;
String todayFor, todayNote;
struct ExpRow { String date, name; };
std::vector<ExpRow> expRows;
// ---- pinned notes (in the same file, already wrapped to the screen's 21 characters) ----
struct BoxNote { String title; std::vector<String> lines; };
std::vector<BoxNote> boxNotes;
int noteOpen = -1, noteListSel = 0;   // the note being read (-1: the list), and where the list was
struct BoxWo { String no, title; std::vector<String> lines; };   // description (wrapped) then numbered steps
std::vector<BoxWo> boxWos;
int woOpen = -1, woListSel = 0;
// ---- trip (today) ----
String tripDay;
int spotsToday = 0;
bool boxH12 = false;                                      // header clock: 12 h if the dashboard says so
double tripDistM = 0;
uint32_t tripFixS = 0;

String ascii(const String& in) {            // the OLED font is plain ASCII
  String o; o.reserve(in.length());
  for (size_t i = 0; i < in.length(); i++) {
    uint8_t c = in[i];
    if (c < 0x80) { o += (c < 0x20) ? ' ' : (char)c; continue; }
    if ((c & 0xE0) == 0xC0) i += 1; else if ((c & 0xF0) == 0xE0) i += 2; else if ((c & 0xF8) == 0xF0) i += 3;
    o += '?';
  }
  return o;
}
String fit(const String& s, int n) { return (int)s.length() <= n ? s : s.substring(0, n - 1) + "~"; }
String shortDate(const String& d) {          // 2026-11-17 -> Nov 17
  static const char* M[] = {"Jan","Feb","Mar","Apr","May","Jun","Jul","Aug","Sep","Oct","Nov","Dec"};
  int m = d.substring(5, 7).toInt(); return String(m >= 1 && m <= 12 ? M[m - 1] : "?") + " " + String(d.substring(8, 10).toInt());
}
long dayNum(const String& d) {               // YYYY-MM-DD -> days since 1970 (civil calendar)
  int y = d.substring(0, 4).toInt(), m = d.substring(5, 7).toInt(), dd = d.substring(8, 10).toInt();
  y -= m <= 2; long era = (y >= 0 ? y : y - 399) / 400; unsigned yoe = (unsigned)(y - era * 400);
  unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + dd - 1; unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  return era * 146097 + (long)doe - 719468;
}

fs::FS* clFS() { return (flashOK && LittleFS.exists(CL_PATH)) ? (fs::FS*)&LittleFS : (sdOK && SD.exists(CL_PATH)) ? (fs::FS*)&SD : nullptr; }

bool clReadDoc(JsonDocument& doc) {
  fs::FS* fs = clFS(); if (!fs) return false;
  File f = fs->open(CL_PATH, FILE_READ); if (!f) return false;
  DeserializationError e = deserializeJson(doc, f); f.close();
  return !e && doc["items"].is<JsonArray>();
}
void clWalk(JsonArray items, const String& pid, uint8_t depth) {
  for (JsonObject it : items) {
    String par = it["parent"] | "";
    bool parOk = false;
    if (par.length()) for (JsonObject x : items) if (par == (const char*)(x["id"] | "")) { parOk = true; break; }
    if ((parOk ? par : String("")) != pid) continue;
    if (cfg.clHideDone && (bool)(it["done"] | false)) continue;   // hidden, with everything under it
    clRows.push_back({ String((const char*)(it["id"] | "")), ascii(it["text"] | ""), depth, (bool)(it["done"] | false) });
    if (depth < 6) clWalk(items, it["id"] | "", depth + 1);
  }
}
void clLoad() {
  if (clVer == docVer) return;
  clVer = docVer; clRows.clear(); clDay = ""; clTitle = ""; clTotal = 0; clDoneN = 0;
  JsonDocument doc;
  if (!clReadDoc(doc)) return;
  clDay = doc["day"] | ""; clTitle = ascii(doc["title"] | "");
  for (JsonObject x : doc["items"].as<JsonArray>()) { clTotal++; if ((bool)(x["done"] | false)) clDoneN++; }
  clWalk(doc["items"].as<JsonArray>(), "", 0);
  int k = -1;   // find the remembered item again (the list may have changed on the phone)
  for (size_t i = 0; clSelId.length() && i < clRows.size(); i++) if (clRows[i].id == clSelId) { k = i; break; }
  int n = clRows.size();
  selMem[S_CL] = k >= 0 ? k : constrain(selMem[S_CL], 0, max(0, n - 1));
  if (scr == S_CL) sel = selMem[S_CL];
}
// Same as ticking it on the phone: the item and everything nested under it change together; unticking
// also unticks the items it sits under. The file changes, and linked work orders get their steps / a note.
void clToggle(int row) {
  if (row < 0 || row >= (int)clRows.size()) return;
  JsonDocument doc;
  if (!clReadDoc(doc)) { msg = "Checklist not found"; msgUntil = millis() + 2000; return; }
  JsonArray items = doc["items"].as<JsonArray>();
  auto findItem = [&](const String& id) -> JsonObject { for (JsonObject x : items) if (id == (const char*)(x["id"] | "")) return x; return JsonObject(); };
  String id0 = clRows[row].id;
  JsonObject it = findItem(id0);
  if (it.isNull()) { clVer = 0xFFFFFFFF; return; }
  bool done = !(bool)(it["done"] | false);
  String today = timeValid ? localDate(time(nullptr)) : String(""), day = doc["day"] | today;
  String stamp = timeValid ? isoUtc(time(nullptr)) : String("");
  std::vector<String> ids = { id0 };
  for (size_t k = 0; k < ids.size() && ids.size() < 300; k++)
    for (JsonObject x : items) if (ids[k] == (const char*)(x["parent"] | "")) ids.push_back(x["id"] | "");
  if (!done) {
    String par = it["parent"] | "";
    for (int g = 0; par.length() && g < 10; g++) { JsonObject p = findItem(par); if (p.isNull()) break; if ((bool)(p["done"] | false)) ids.push_back(par); par = p["parent"] | ""; }
  }
  String evs;
  for (const String& id : ids) {
    JsonObject x = findItem(id); if (x.isNull()) continue;
    bool was = x["done"] | false;
    if (was == done && id != id0) continue;               // already right
    x["done"] = done;
    if (done && today.length()) x["doneAt"] = today; else x["doneAt"] = nullptr;
    if (stamp.length()) x["updated"] = stamp;
    String wo = x["wo"] | "", step = x["step"] | "";
    if (wo.length() && step.length())
      evs += (evs.length() ? "\n" : "") + String("{\"op\":\"wo.step\",\"id\":") + jStr(wo) + ",\"step\":" + jStr(step) + ",\"done\":" + jBool(done) + ",\"date\":" + jStr(day) + ",\"text\":" + jStr(x["text"] | "") + ",\"via\":\"cl\"}";
  }
  String text = it["text"] | "", wo0 = it["wo"] | "", step0 = it["step"] | "";
  if (done && !(wo0.length() && step0.length())) {        // a note on the work order the item belongs to
    String par = it["parent"] | ""; String lw = wo0;
    for (int guard = 0; !lw.length() && par.length() && guard < 10; guard++) { JsonObject p = findItem(par); if (p.isNull()) break; lw = p["wo"] | ""; par = p["parent"] | ""; }
    if (lw.length()) evs += (evs.length() ? "\n" : "") + String("{\"op\":\"wo.note\",\"id\":") + jStr(lw) + ",\"text\":" + jStr("Checklist \xE2\x9C\x93 " + text + " (" + shortDate(day) + ")") + "}";
  }
  String out; serializeJson(doc, out);
  bool ok = false;
  if (flashOK) { ok = writeDoc(LittleFS, CL_PATH, out); if (ok && sdOK) writeDoc(SD, CL_PATH, out); }
  else if (sdOK) ok = writeDoc(SD, CL_PATH, out);
  if (!ok) { msg = "Not saved"; msgUntil = millis() + 2000; return; }
  docVer++;
  if (evs.length() && timeValid && store) { String ts; uint32_t first; appendEvents(evs, ts, first); }
  ledBlinkUntil = millis() + 400;                         // the light blinks; the row itself shows the tick
  clSelId = id0;
  clLoad();
}

void todayLoad() {
  String d = timeValid ? localDate(time(nullptr)) : String("");
  if (todayVer == docVer && todayFor == d) return;
  todayVer = docVer; todayFor = d; todayRows.clear(); todayNote = ""; expRows.clear(); boxNotes.clear();
  const char* P = "/hwo/doc/oled.json";
  fs::FS* fs = (flashOK && LittleFS.exists(P)) ? (fs::FS*)&LittleFS : (sdOK && SD.exists(P)) ? (fs::FS*)&SD : nullptr;
  if (!fs) { todayNote = d.length() ? "Open the dashboard once" : "Clock not set yet"; return; }
  File f = fs->open(P, FILE_READ);
  JsonDocument doc; DeserializationError e = deserializeJson(doc, f); f.close();
  if (e) { todayNote = "Calendar file unreadable"; return; }
  boxH12 = doc["h12"] | false;
  if (doc["scr"].is<JsonArray>()) {
    int n = 0;
    for (const char* k : doc["scr"].as<JsonArray>()) for (int i = 0; i < S_COUNT && n < S_COUNT; i++) if (k && !strcmp(k, SCR_KEY[i])) scrOrd[n++] = i;
    if (n) scrN = n;
  }
  lbKeys.clear(); mapKeys.clear(); boxWos.clear();
  for (const char* k : doc["lb"].as<JsonArray>()) if (lbKeys.size() < 12) lbKeys.push_back(k ? k : "-");
  for (const char* k : doc["map"].as<JsonArray>()) if (mapKeys.size() < 13) mapKeys.push_back(k ? k : "-");
  fSpd = doc["fmt"]["spd"] | "mph"; fDist = doc["fmt"]["dist"] | "mi"; fPos = doc["fmt"]["pos"] | "dd"; fTemp = doc["fmt"]["temp"] | "F";
  for (JsonArray x : doc["wos"].as<JsonArray>()) {
    if (boxWos.size() >= 10) break;
    BoxWo w; w.no = ascii(x[0] | ""); w.title = ascii(x[1] | "");
    JsonArray xd = x[2], xs = x[3];
    for (JsonVariant l : xd) if (w.lines.size() < 60) w.lines.push_back(fit(ascii(l | ""), 21));
    int k = 0;
    for (JsonVariant l : xs) { if (w.lines.size() >= 100) break; if (!k) { if (w.lines.size()) w.lines.push_back(""); w.lines.push_back("STEPS"); } w.lines.push_back(String(++k) + ". " + ascii(l | "")); }
    boxWos.push_back(w);
  }
  if (woOpen >= (int)boxWos.size()) { woOpen = -1; if (scr == S_WO) sel = 0; }
  for (JsonArray x : doc["exp"].as<JsonArray>()) expRows.push_back({ String((const char*)(x[0] | "")), ascii(x[1] | "") });
  for (JsonArray x : doc["notes"].as<JsonArray>()) {
    if (boxNotes.size() >= 8) break;
    BoxNote b; b.title = fit(ascii(x[0] | ""), 40);
    JsonArray lines = x[1];
    for (JsonVariant l : lines) { if (b.lines.size() >= 80) break; b.lines.push_back(fit(ascii(l | ""), 21)); }
    boxNotes.push_back(b);
  }
  if (noteOpen >= (int)boxNotes.size()) { noteOpen = -1; if (scr == S_NOTES) sel = 0; }
  if (!d.length()) { todayNote = "Clock not set yet"; return; }
  String to = doc["to"] | "";
  if (to.length() && d > to) { todayNote = "Open the dashboard to load"; return; }
  JsonArray day = doc["days"][d].as<JsonArray>();
  for (JsonArray ev : day) {
    String t1 = ev[0] | "", t2 = ev[1] | "", ti = ascii(ev[2] | "");
    todayRows.push_back((t1.length() ? t1 : String("all  ")) + " " + ti);     // same width as 09:00
  }
  if (!todayRows.size()) todayNote = "Nothing on today";
}

// distance and time with a fix today, from today's track file (once), then kept up as points are logged
void tripLoad() {
  if (!timeValid) return;
  String d = trackDate(time(nullptr));
  if (d == tripDay) return;
  tripDay = d; tripDistM = 0; tripFixS = 0;
  String path = "/gps/" + d + ".csv";
  fs::FS* fs = (sdOK && SD.exists(path)) ? (fs::FS*)&SD : (flashOK && LittleFS.exists(path)) ? (fs::FS*)&LittleFS : nullptr;
  uint32_t n = 0;
  if (fs) {
    File f = fs->open(path, FILE_READ);
    double la0 = 0, lo0 = 0; uint32_t t0 = 0;
    while (f && f.available()) {
      String l = f.readStringUntil('\n');
      int c1 = l.indexOf(','), c2 = l.indexOf(',', c1 + 1), c3 = l.indexOf(',', c2 + 1);
      if (c1 < 0 || c3 < 0 || !l.startsWith("20")) continue;
      uint32_t t = parseUtc(l.substring(0, c1));
      double la = l.substring(c1 + 1, c2).toDouble(), lo = l.substring(c2 + 1, c3).toDouble();
      l.trim(); bool seg = l.endsWith(",1");
      if (t0 && !seg && t > t0 && t - t0 <= 400) { tripDistM += distM(la0, lo0, la, lo); tripFixS += t - t0; }
      la0 = la; lo0 = lo; t0 = t; n++;
    }
    if (f) f.close();
  }
  gps.ptsDay = d; gps.ptsToday = n;
}
void tripAddPoint(uint32_t ep, double la, double lo) {     // called just before a point is logged
  String d = trackDate(ep);
  if (d != tripDay) { tripDay = d; tripDistM = 0; tripFixS = 0; gps.ptsDay = d; gps.ptsToday = 0; spotsToday = 0; return; }   // midnight
  if (!gps.newSeg && gps.ptEpoch && ep > gps.ptEpoch && ep - gps.ptEpoch <= 400) {
    tripDistM += distM(gps.ptLat, gps.ptLon, la, lo); tripFixS += ep - gps.ptEpoch;
  }
}

// ======================================================================
//  Buzzer, timers, find-my-box, BNWAS
// ======================================================================
bool buzOK = false, buzOn = false;
uint32_t buzHz = 0;
void buzSet(uint32_t hz) {
  if (!buzOK) return;
  if (!hz) { if (buzOn) { ledcWrite(BUZ_A, 0); ledcWrite(BUZ_B, 256); buzOn = false; } return; }   // 256 inverted = both pins low
  if (hz != buzHz) { ledcChangeFrequency(BUZ_A, hz, 8); buzHz = hz; }
  if (!buzOn) { ledcWrite(BUZ_A, 128); ledcWrite(BUZ_B, 128); buzOn = true; }
}
void buzStart() {
  // channels 0 and 1 share one timer, so the two pins stay in step; B is inverted: twice the swing across the piezo
  buzOK = ledcAttachChannel(BUZ_A, BUZ_HZ, 8, 0) && ledcAttachChannel(BUZ_B, BUZ_HZ, 8, 1) && ledcOutputInvert(BUZ_B, true);
  buzHz = BUZ_HZ;
  if (buzOK) { ledcWrite(BUZ_A, 0); ledcWrite(BUZ_B, 256); }
}

struct KTimer {
  uint32_t id, secs, endMs, ringSince; String name; bool ringing;
  uint32_t left(uint32_t now) const { int32_t d = (int32_t)(endMs - now); return d <= 0 ? 0 : ((uint32_t)d + 999) / 1000; }
};
std::vector<KTimer> timers;
uint32_t timerNextId = 1;
const size_t MAX_TIMERS = 8;
const uint32_t TIMER_RING_MAX_MS = 10UL * 60UL * 1000UL;   // a ringing timer gives up after 10 minutes
bool findOn = false;
uint32_t findSince = 0;
const uint32_t FIND_MAX_MS = 2UL * 60UL * 1000UL;
uint8_t bnStage = 0;                 // 0 counting down, 1 visual warning, 2 buzzer
uint32_t bnT0 = 0;                   // start of the current stage (stage 0: of the period)
const uint32_t BN_VISUAL_MS = 30000;

bool timerRinging() { for (auto& t : timers) if (t.ringing) return true; return false; }
bool alarmActive() { return bnStage > 0 || findOn || timerRinging(); }
void bnwasRestart(uint32_t now) { bnStage = 0; bnT0 = now; }
// box = the button on the box. The dashboard can stop timers and find-my-box, but only the box resets BNWAS.
void silence(bool box, uint32_t now) {
  for (size_t i = 0; i < timers.size();) { if (timers[i].ringing) timers.erase(timers.begin() + i); else i++; }
  findOn = false;
  if (box && cfg.bnwasOn) bnwasRestart(now);
  buzSet(0); redraw = true;
}
uint32_t bnLeftS(uint32_t now) {
  if (!cfg.bnwasOn) return 0;
  if (bnStage == 0) { uint32_t per = (uint32_t)cfg.bnwasMin * 60000UL, el = now - bnT0; return el >= per ? 0 : (per - el + 999) / 1000; }
  if (bnStage == 1) { uint32_t el = now - bnT0; return el >= BN_VISUAL_MS ? 0 : (BN_VISUAL_MS - el + 999) / 1000; }
  return 0;
}

void alarmsTick(uint32_t now) {
  for (size_t i = 0; i < timers.size();) {
    KTimer& t = timers[i];
    if (!t.ringing && (int32_t)(now - t.endMs) >= 0) { t.ringing = true; t.ringSince = now; redraw = true; }
    if (t.ringing && now - t.ringSince > TIMER_RING_MAX_MS) { timers.erase(timers.begin() + i); redraw = true; continue; }
    i++;
  }
  if (findOn && now - findSince > FIND_MAX_MS) { findOn = false; redraw = true; }
  if (!cfg.bnwasOn) bnStage = 0;
  else if (bnStage == 0 && now - bnT0 >= (uint32_t)cfg.bnwasMin * 60000UL) { bnStage = 1; bnT0 = now; redraw = true; }
  else if (bnStage == 1 && now - bnT0 >= BN_VISUAL_MS) { bnStage = 2; bnT0 = now; redraw = true; }
  // sound: BNWAS buzzer first, then timers, then find-my-box
  uint32_t hz = 0;
  if (bnStage == 2) {
    uint32_t s = now - bnT0, per = s < 30000 ? 2000 : s < 60000 ? 1000 : 400, on = s < 60000 ? 300 : 250, ph = s % per;
    if (ph < on) hz = (s >= 60000 && (s / per) % 2) ? 3100 : BUZ_HZ;            // gets faster the longer it goes
  } else if (timerRinging()) {
    uint32_t ph = now % 1500;
    if (ph < 120 || (ph >= 240 && ph < 360) || (ph >= 480 && ph < 600)) hz = BUZ_HZ;
  } else if (findOn) {
    uint32_t ph = now % 1200;
    if (ph < 150) hz = BUZ_HZ; else if (ph < 300) hz = 3200;
  }
  buzSet(hz);
}

// ---- drawing ----
static uint8_t prevFrame[128 * 64];
bool frameStale = true;
void pushFrame() {                       // sends only the rows that changed (a full frame is 8 KB of I2C)
  uint8_t* b = oled.buf(); if (!b) return;
  int y1 = -1, y2 = -1;
  if (frameStale) { y1 = 0; y2 = 127; }
  else for (int y = 0; y < 128; y++) if (memcmp(b + y * 64, prevFrame + y * 64, 64)) { if (y1 < 0) y1 = y; y2 = y; }
  if (y1 < 0) return;
  oled.win(y1, y2); oled.display();
  memcpy(prevFrame, b, sizeof prevFrame); frameStale = false;
}
void head(const char* name) {
  oled.fillRect(0, 0, 128, 9, C_ON);
  oled.setTextColor(C_OFF);
  oled.setCursor(1, 1); oled.print(name);
  String tm5, r;
  if (timeValid) {
    todayLoad();                                         // cached: reads the file only when it changed
    struct tm tm; time_t t = time(nullptr); localtime_r(&t, &tm); char b[8];
    if (boxH12) snprintf(b, sizeof b, "%d:%02d%c", tm.tm_hour % 12 ? tm.tm_hour % 12 : 12, tm.tm_min, tm.tm_hour < 12 ? 'a' : 'p');
    else strftime(b, sizeof b, "%H:%M", &tm);
    tm5 = b;
  }
  int room = 21 - (int)strlen(name) - 1;                 // never let the clock run into the title
  r = tm5; if (battType && !isnan(battPct)) r += " " + String((int)battPct) + "%";
  if ((int)r.length() > room) r = tm5;
  if ((int)r.length() > room) r = "";
  oled.setCursor(127 - 6 * r.length(), 1); oled.print(r);
  oled.setTextColor(C_ON);
}
void row(int i, const String& s, bool inv = false, uint16_t col = C_ON) {
  int y = 11 + i * 9;
  if (inv) { oled.fillRect(0, y - 1, 128, 9, C_ON); oled.setTextColor(C_OFF); }
  else oled.setTextColor(col);
  oled.setCursor(1, y); oled.print(fit(s, 21));
  oled.setTextColor(C_ON);
}
void rowLR(int i, const String& l, const String& r, uint16_t col = C_ON) {     // left text, right-aligned value
  int room = 21 - (int)r.length() - 1;
  row(i, fit(l, max(room, 1)), false, col);
  oled.setTextColor(col); oled.setCursor(127 - 6 * r.length(), 11 + i * 9); oled.print(r); oled.setTextColor(C_ON);
}
// "In  68.4F 52%"; a reading the box hasn't heard again lately gets its age and is drawn grey
bool tempLine(const char* name, const char* lbl, String& out) {
  for (int i = 0; i < MAX_NODES; i++) {
    const Node& n = nodes[i];
    if (!n.used || strcmp(n.name, name) || isnan(n.tempC)) continue;
    out = String(lbl) + (fTemp == "C" ? String(n.tempC, 1) + "C" : String(n.tempC * 9.0f / 5.0f + 32.0f, 1) + "F");
    if (!isnan(n.rh)) out += " " + String((int)lroundf(n.rh)) + "%";
    if (nodeFresh(n)) return true;
    uint32_t ag = nodeAge(n);
    out += ag == 0xFFFFFFFFUL ? " old" : ag < 5400 ? " " + String(ag / 60) + "m" : ag < 172800 ? " " + String(ag / 3600) + "h" : " " + String(ag / 86400) + "d";
    return false;
  }
  out = String(lbl) + "--"; return true;
}
String clockLeft(uint32_t s) {                         // 4:32, or 1:05:00
  char b[24];
  if (s >= 3600) snprintf(b, sizeof b, "%lu:%02lu:%02lu", (unsigned long)(s / 3600), (unsigned long)((s / 60) % 60), (unsigned long)(s % 60));
  else snprintf(b, sizeof b, "%lu:%02lu", (unsigned long)(s / 60), (unsigned long)(s % 60));
  return b;
}
// a scrolling list: the highlight moves down the screen and the list only scrolls once it reaches the
// bottom (or top) row; each screen keeps its own scroll position
int listTop[S_COUNT] = { 0 };
int fixLen(const String& s, int mode) {   // 0: none, 1: through the first space (time, number), 2: indent + "[ ] "
  if (mode == 1) { int k = s.indexOf(' '); return k > 0 && k < 9 ? k + 1 : 0; }
  if (mode == 2) { int k = 0; while (k < (int)s.length() && s[k] == ' ') k++; return min(k + 4, 12); }
  return 0;
}
void rowSlide(int i, const String& s, int fix) {
  int y = 11 + i * 9, over = ((int)s.length() - 21) * 6, off = 0;
  if (over > 0) {
    mqOn = true;
    uint32_t t = millis() - mqT0, run = over * 30;           // a pause, 1 px per 30 ms, a pause, again
    if (t >= 1500 + run + 1200) mqT0 = millis(); else if (t > 1500) off = min((int)((t - 1500) / 30), over);
  }
  oled.fillRect(0, y - 1, 128, 9, C_ON); oled.setTextColor(C_OFF);
  oled.setCursor(1 + 6 * fix - off, y); oled.print(s.substring(fix));
  if (fix) { oled.fillRect(0, y - 1, 1 + 6 * fix, 9, C_ON); oled.setCursor(1, y); oled.print(s.substring(0, fix)); }
  oled.setTextColor(C_ON);
}
void drawList(const std::vector<String>& L, int selRow, bool showSel, int fixMode = 0) {
  int n = L.size(); int& top = listTop[scr];
  if (showSel) { if (selRow < top) top = selRow; if (selRow > top + ROWS - 1) top = selRow - ROWS + 1; }
  top = constrain(top, 0, max(0, n - ROWS));
  for (int i = 0; i < ROWS && top + i < n; i++) {
    if (showSel && top + i == selRow) rowSlide(i, L[top + i], fixLen(L[top + i], fixMode));
    else row(i, L[top + i]);
  }
}
String coord(double v, bool lat) {   // 38.12345 N  /  38 07.407'N  /  38 07'24.4"N
  char h = lat ? (v >= 0 ? 'N' : 'S') : (v >= 0 ? 'E' : 'W'); v = fabs(v);
  int d = (int)v; double m = (v - d) * 60;
  char b[24];
  if (fPos == "dm") snprintf(b, sizeof b, "%d%c %06.3f'%c", d, 248, m, h);
  else if (fPos == "dms") snprintf(b, sizeof b, "%d%c %02d'%04.1f\"%c", d, 248, (int)m, (m - (int)m) * 60, h);
  else snprintf(b, sizeof b, "%.5f %c", v, h);
  return b;
}
String distTxt(double m) {
  double k = fDist == "km" ? 1000.0 : fDist == "nm" ? 1852.0 : 1609.344, v = m / k;
  return String(v, v < 10 ? 2 : 1) + " " + fDist;
}
// one line of LOGBOOK; col goes grey for a stale reading
String lbLine(const String& k, uint16_t& col) {
  col = C_ON;
  struct tm tm; time_t t = time(nullptr); localtime_r(&t, &tm); char b[28];
  if (k == "date") { if (!timeValid) return "Clock not set"; strftime(b, sizeof b, "%a %b %d %Y", &tm); return b; }
  if (k == "time") { if (!timeValid) return "Clock not set"; if (boxH12) snprintf(b, sizeof b, "%d:%02d:%02d %cm", tm.tm_hour % 12 ? tm.tm_hour % 12 : 12, tm.tm_min, tm.tm_sec, tm.tm_hour < 12 ? 'a' : 'p'); else strftime(b, sizeof b, "%H:%M:%S", &tm); return b; }
  if (k == "net") return staUp ? "WiFi home " + String(WiFi.RSSI()) + "dBm" : "Hotspot " + String(WiFi.softAPgetStationNum()) + " connected";
  if (k == "ip") return staUp ? "IP " + WiFi.localIP().toString() : "IP " + WiFi.softAPIP().toString();
  if (k == "gps") { bool g = gpsI2C && gps.seen && millis() - gps.lastNmeaMs < 5000; return !g ? String("GPS not found") : gps.fix ? "GPS fix " + String(gps.sats) + " sat" : "GPS no fix (" + String(gps.sats) + " sat)"; }
  if (k == "sd") return (sdOK ? String("SD ok") : String("SD none")) + "  " + String(gps.ptsDay == tripDay ? gps.ptsToday : 0) + " pts";
  if (k == "batt") return battType && !isnan(battPct) ? "Battery " + String((int)battPct) + "% " + String(battV, 2) + "V" : String("Battery --");
  if (k == "volt") return !isnan(battV) ? "Battery " + String(battV, 2) + " V" : String("Battery --");
  if (k == "left") {
    if (isnan(battRate) || isnan(battPct)) return "Time left --";
    if (battRate > 0.5) return "Charging +" + String(battRate, 1) + "%/h";
    if (battRate > -0.05) return "Time left: steady";
    float h = battPct / -battRate; return "Time left ~" + (h < 48 ? String(h, h < 10 ? 1 : 0) + " h" : String(h / 24, 1) + " d");
  }
  if (k == "chip") { float c = temperatureRead(); return "Box " + (fTemp == "C" ? String(c, 1) + "C" : String(c * 9 / 5 + 32, 1) + "F"); }
  if (k == "up") { uint32_t u = millis() / 1000; snprintf(b, sizeof b, "Up %lud %02lu:%02lu", (unsigned long)(u / 86400), (unsigned long)(u / 3600 % 24), (unsigned long)(u / 60 % 60)); return b; }
  if (k.startsWith("n:")) { int p = k.indexOf('|'); String o; if (!tempLine(k.substring(2, p < 0 ? k.length() : p).c_str(), p < 0 ? "" : k.substring(p + 1).c_str(), o)) col = C_DIM; return o; }
  return "";
}
String mapLine(const String& k, uint16_t& col) {
  bool fresh = gps.fix && millis() - gps.lastFixMs < 5000;
  col = C_ON;
  if (k == "spd") { if (!fresh || isnan(gps.kn)) return "Speed --"; float v = fSpd == "kn" ? gps.kn : fSpd == "kmh" ? gps.kn * 1.852f : gps.kn * 1.15078f; return "Speed " + String(v, 1) + " " + (fSpd == "kmh" ? "km/h" : fSpd); }
  if (k == "dist") return "Dist  " + distTxt(tripDistM);
  if (k == "fix") return "Fix   " + String(tripFixS / 3600) + "h " + String((tripFixS / 60) % 60) + "m today";
  if (k == "pts") return "Points " + String(gps.ptsDay == tripDay ? gps.ptsToday : 0) + " today";
  if (k == "spots") { col = C_DIM; return spotsToday ? "Spots " + String(spotsToday) + " - press=save" : String("Press: save spot"); }
  if (k == "lat" || k == "lon" || k == "sats" || k == "alt") {
    if (!gps.lastFixMs) return k == "lat" ? "No fix yet" : "";
    if (!fresh) col = C_DIM;
    if (k == "lat") return coord(gps.lat, true);
    if (k == "lon") return coord(gps.lon, false);
    if (k == "alt") return isnan(gps.alt) ? String("Alt --") : "Alt " + (fDist == "mi" ? String(gps.alt * 3.28084f, 0) + " ft" : String(gps.alt, 0) + " m");
    return fresh ? "Sats " + String(gps.sats) : "last fix " + String((millis() - gps.lastFixMs) / 60000) + " min ago";
  }
  return "";
}
// big centred box over the screen for alarms
void alarmBox(const char* title, const String& l1, const String& l2, bool inv) {
  uint16_t bg = inv ? C_ON : C_OFF, fg = inv ? C_OFF : C_ON;
  oled.fillRect(0, 28, 128, 74, bg); oled.drawRect(0, 28, 128, 74, C_ON); oled.drawRect(2, 30, 124, 70, C_ON);
  oled.setTextColor(fg); oled.setTextSize(2);
  oled.setCursor(64 - 6 * strlen(title), 40); oled.print(title);
  oled.setTextSize(1);
  String a = fit(l1, 18), b = fit(l2, 18);
  oled.setCursor(64 - 3 * a.length(), 64); oled.print(a);
  oled.setCursor(64 - 3 * b.length(), 80); oled.print(b);
  oled.setTextColor(C_ON);
}

void drawScreen() {
  uint32_t now = millis();
  { int k = 0; while (k < scrN && scrOrd[k] != scr) k++; if (k == scrN) setScreen(scrOrd[0]); }
  mqOn = false;
  oled.clearDisplay();
  oled.setTextSize(1);
  if (scr == S_STATUS) {
    head("LOGBOOK");
    static const char* LB0[] = { "date", "net", "gps", "sd", "-", "n:Inside|In  ", "n:Outside|Out " };
    int nL = lbKeys.size() ? (int)lbKeys.size() : 7;
    for (int i = 0; i < nL; i++) { uint16_t c; String l = lbLine(lbKeys.size() ? lbKeys[i] : String(LB0[i]), c); row(i, l, false, c); }
    int aRow = nL + 1;
    // timers, BNWAS and coming expiries
    todayLoad();
    struct L { String l, r; uint16_t c; };
    std::vector<L> al;
    for (auto& k : timers) al.push_back({ k.name, k.ringing ? String("DONE") : clockLeft(k.left(now)), C_ON });
    if (cfg.bnwasOn) al.push_back({ "BNWAS", bnStage ? String("PRESS") : clockLeft(bnLeftS(now)), C_ON });
    long dn = timeValid ? dayNum(localDate(time(nullptr))) : 0;
    for (auto& x : expRows) {
      if (!dn || x.date.length() != 10) continue;
      long d = dayNum(x.date) - dn;
      al.push_back({ "Exp " + x.name, d < 0 ? String("EXPIRED") : d == 0 ? String("today") : String(d) + "d", d <= 7 ? C_ON : C_DIM });
    }
    size_t room = max(1, ROWS - aRow);
    if (al.size()) oled.drawFastHLine(0, 11 + (aRow - 1) * 9 + 3, 128, C_DIM);
    for (size_t i = 0; i < al.size() && i < room; i++) {
      if (i == room - 1 && al.size() > room) { row(aRow + i, "+" + String(al.size() - i) + " more", false, C_DIM); break; }
      rowLR(aRow + i, al[i].l, al[i].r, al[i].c);
    }
  } else if (scr == S_CL) {
    clLoad();
    String h = "CHECKLIST " + String(clDoneN) + "/" + String(clTotal);
    head(h.c_str());
    if (!clRows.size()) { if (clTotal) { row(1, "All ticked"); row(2, "(ticked items hidden)", false, C_DIM); } else { row(1, "No items"); row(2, "Add them on the phone"); } }
    else {
      std::vector<String> L;
      for (auto& r : clRows) { String ind; for (int k = 0; k < r.depth; k++) ind += " "; L.push_back(ind + (r.done ? "[x] " : "[ ] ") + r.text); }
      drawList(L, sel, true, 2);
    }
  } else if (scr == S_TODAY) {
    todayLoad();
    String h = "TODAY"; if (todayFor.length()) h += " " + shortDate(todayFor);
    head(h.c_str());
    if (!todayRows.size()) row(1, todayNote);
    else drawList(todayRows, sel, true, 1);
  } else if (scr == S_NOTES) {
    todayLoad();
    if (noteOpen >= (int)boxNotes.size()) noteOpen = -1;   // the file went away or changed
    if (noteOpen < 0) {
      head("NOTES");
      if (!boxNotes.size()) { row(1, "No notes here"); row(2, "Tick 'Show on the box'", false, C_DIM); row(3, "on a note", false, C_DIM); }
      else { std::vector<String> L; for (auto& b : boxNotes) L.push_back(b.title); drawList(L, sel, true); }
    } else {
      BoxNote& b = boxNotes[noteOpen];
      head(fit(b.title, 21).c_str());
      int n = b.lines.size(), top = constrain(sel, 0, max(0, n - ROWS));
      for (int i = 0; i < ROWS && top + i < n; i++) row(i, b.lines[top + i]);
      if (!n) row(1, "(no text)", false, C_DIM);
      if (n > ROWS) { int H = ROWS * 9, bh = max(6, H * ROWS / n), by = 10 + (H - bh) * top / (n - ROWS); oled.drawFastVLine(127, by, bh, C_DIM); }   // where you are in it
    }
  } else if (scr == S_TRIP) {
    head("MAP TODAY");
    tripLoad();
    static const char* MP0[] = { "spd", "dist", "fix", "spots", "lat", "lon", "sats" };
    int nM = mapKeys.size() ? (int)mapKeys.size() : 7;
    for (int i = 0; i < nM; i++) { uint16_t c; String l = mapLine(mapKeys.size() ? mapKeys[i] : String(MP0[i]), c); row(i, l, false, c); }
  } else if (scr == S_WO) {
    todayLoad();
    if (woOpen >= (int)boxWos.size()) woOpen = -1;
    if (woOpen < 0) {
      head(("WORK ORDERS " + String(boxWos.size())).c_str());
      if (!boxWos.size()) { row(1, "None here"); row(2, "Tick 'Show on the box'", false, C_DIM); row(3, "on a work order", false, C_DIM); }
      else { std::vector<String> L; for (auto& w : boxWos) L.push_back(w.no + " " + w.title); drawList(L, sel, true, 1); }
    } else {
      BoxWo& w = boxWos[woOpen];
      head(fit(w.no + " " + w.title, 21).c_str());
      if (!w.lines.size()) row(1, "(no description)", false, C_DIM);
      else drawList(w.lines, sel, true, 1);
    }
  } else {
    head("JOIN");
    row(0, "WiFi " + ascii(cfg.apSsid));
    row(1, "Pass " + ascii(cfg.apPass));
    row(3, "Open " + String(MDNS_NAME) + ".local");
    row(4, "  or " + WiFi.softAPIP().toString());
    row(6, staUp ? "Home: " + WiFi.localIP().toString() : String("Home WiFi: not here"));
  }
  // alarms on top of whatever screen is up
  bool blink = (now / 500) % 2;
  if (bnStage == 1) alarmBox("BNWAS", "Press the button", "alarm in " + String(bnLeftS(now)) + " s", blink);
  else if (bnStage == 2) alarmBox("BNWAS", "Press the button", "", blink);
  else if (timerRinging()) { String n; for (auto& k : timers) if (k.ringing) { n = k.name; break; } alarmBox("TIMER", n, "Press to stop", blink); }
  else if (findOn) alarmBox("HERE", "Find my box", "Press to stop", blink);
  else if (millis() < msgUntil) { oled.fillRect(14, 50, 100, 26, C_OFF); oled.drawRect(14, 50, 100, 26, C_ON); oled.setCursor(64 - 3 * msg.length(), 59); oled.print(msg); }
  pushFrame();
}

int listLen() {
  if (scr == S_CL) { clLoad(); return clRows.size(); }
  if (scr == S_TODAY) { todayLoad(); return todayRows.size(); }
  if (scr == S_NOTES) { todayLoad(); if (noteOpen >= (int)boxNotes.size()) noteOpen = -1; return noteOpen >= 0 ? max(1, (int)boxNotes[noteOpen].lines.size() - ROWS + 1) : (int)boxNotes.size(); }   // reading: one step per line
  if (scr == S_WO) { todayLoad(); if (woOpen >= (int)boxWos.size()) woOpen = -1; return woOpen >= 0 ? (int)boxWos[woOpen].lines.size() : (int)boxWos.size(); }
  return 0;
}
void setScreen(int s) {   // each screen comes back where you left it
  if (scr == S_NOTES && noteOpen >= 0) { noteOpen = -1; sel = noteListSel; }   // leaving a note goes back to the list
  if (scr == S_WO && woOpen >= 0) { woOpen = -1; sel = woListSel; }
  selMem[scr] = sel;
  scr = constrain(s, 0, S_COUNT - 1); mqT0 = millis();
  int n = listLen();
  sel = constrain(selMem[scr], 0, max(0, n - 1));
  redraw = true;
}
void nextScreen() {
  int k = 0; while (k < scrN && scrOrd[k] != scr) k++;
  setScreen(scrOrd[k < scrN ? (k + 1) % scrN : 0]);
}
void moveSel(int d) {   // one row per click of the knob, stopping at the ends
  int n = listLen(); if (n <= 0) { sel = 0; return; }
  int s2 = constrain(sel + d, 0, n - 1);
  if (s2 != sel) { sel = s2; redraw = true; mqT0 = millis(); }
  selMem[scr] = sel;
  if (scr == S_CL && sel < (int)clRows.size()) clSelId = clRows[sel].id;
}

void input() { redraw = true; }
void screenPower(bool on) {
  on = on && oledOK;
  if (on == screenOn) return;
  screenOn = on;
  if (oledOK) oled.oled_command(on ? SSD1327_DISPLAYON : SSD1327_DISPLAYOFF);
  if (on) { oled.setContrast(cfg.contrast); frameStale = true; redraw = true; }
}
// any turn or press: restarts the sleep timer and the BNWAS period; true if it should do nothing else
// (it stopped an alarm, or only woke the screen)
bool touched(uint32_t now) {
  lastInputMs = now; input();
  if (cfg.bnwasOn) bnwasRestart(now);
  if (alarmActive()) { silence(true, now); msg = "Stopped"; msgUntil = now + 1000; return true; }
  if (!screenOn && oledOK) { screenPower(true); return true; }
  return false;
}
void onShort() { if (screenOn) nextScreen(); }
void woPress() {   // WORK ORDERS: press opens one (description, then steps), press again goes back
  todayLoad();
  if (woOpen >= (int)boxWos.size()) woOpen = -1;
  if (woOpen < 0) { if (!boxWos.size()) { nextScreen(); return; } woListSel = sel; woOpen = constrain(sel, 0, (int)boxWos.size() - 1); sel = 0; listTop[S_WO] = 0; }
  else { woOpen = -1; sel = woListSel; }
  selMem[S_WO] = sel; mqT0 = millis(); redraw = true;
}
// MAP: a press saves where the box is now, as a spot on the dashboard's Map tab
void spotSave() {
  bool fresh = gps.fix && millis() - gps.lastFixMs < 5000;
  if (!fresh) { msg = "No GPS fix"; msgUntil = millis() + 1500; redraw = true; return; }
  if (!timeValid || !store) { msg = "Not saved"; msgUntil = millis() + 1500; redraw = true; return; }
  uint32_t t = time(nullptr);
  String ev = "{\"op\":\"spot.add\",\"id\":\"sp-" + String(t, HEX) + String(millis() % 1000) + "\",\"lat\":" + String(gps.lat, 6) + ",\"lon\":" + String(gps.lon, 6) +
              ",\"t\":" + jStr(isoLocal(t)) + ",\"src\":\"box\"}";
  String ts; uint32_t first;
  if (appendEvents(ev, ts, first) == 1) { spotsToday++; msg = "Spot saved"; ledBlinkUntil = millis() + 400; }
  else msg = "Not saved";
  msgUntil = millis() + 1500; redraw = true;
}
// NOTES: press opens the highlighted note, press again goes back to the list
void notesPress() {
  todayLoad();
  if (noteOpen >= (int)boxNotes.size()) noteOpen = -1;
  if (noteOpen < 0) { if (!boxNotes.size()) { nextScreen(); return; } noteListSel = sel; noteOpen = constrain(sel, 0, (int)boxNotes.size() - 1); sel = 0; }
  else { noteOpen = -1; sel = noteListSel; }
  selMem[S_NOTES] = sel; redraw = true;
}
void onHold() {   // without the encoder, hold the button to tick (checklist) or open / close a note
  if (!screenOn || encOK) return;
  if (scr == S_CL) clToggle(sel); else if (scr == S_NOTES) notesPress(); else if (scr == S_TRIP) spotSave(); else if (scr == S_WO) woPress();
}

void encStart() {
  for (uint8_t a : ENC_ADDRS) if (enc.begin(a)) { encOK = true; encAddr = a; break; }
  if (!encOK && battType != 1 && enc.begin(0x36)) { encOK = true; encAddr = 0x36; }   // address jumpers left open, no MAX17048 there
  if (!encOK) return;
  enc.pinMode(ENC_SW, INPUT_PULLUP);
  enc.getEncoderDelta();                                   // start counting from here
  if (ENC_INT_PIN >= 0) { pinMode(ENC_INT_PIN, INPUT_PULLUP); enc.setGPIOInterrupts((uint32_t)1 << ENC_SW, true); enc.enableEncoderInterrupt(); }
  Serial.printf("Encoder at 0x%02X (seesaw product %lu)\n", encAddr, (unsigned long)(enc.getVersion() >> 16));
}

void screenStart() {
  pinMode(BTN_EXT, INPUT_PULLUP);
  buzStart();                                              // claims LEDC channels 0 and 1 first
  ledcAttachChannel(BTN_LED, 5000, 8, BTN_LED_CH); ledcWrite(BTN_LED, 0);
  bnwasRestart(millis());
  oledOK = oled.begin(OLED_ADDR, true);
  encStart();
  lastInputMs = millis();
  if (!oledOK) return;
  oled.setRotation(OLED_ROT);
  oled.cp437(true);
  oled.clearDisplay(); oled.display();
  oled.oled_command(SSD1327_DISPLAYOFF);
  screenPower(true);                                       // on at power-up, then sleeps when left alone
}

uint32_t lastTripCheck = 0;
void encTick(uint32_t now) {
  if (!encOK || now - lastEncMs < 20) return;
  lastEncMs = now;
  if (ENC_INT_PIN >= 0 && digitalRead(ENC_INT_PIN) == HIGH && !encSw.down) return;   // nothing new (INT pulls low on a turn or press)
  // push switch
  bool d = !enc.digitalRead(ENC_SW);
  if (d != encSw.down) {
    encSw.down = d;
    if (d) { encSw.t0 = now; encSw.longDone = false; encSw.swallow = touched(now); }
    else {
      if (!encSw.swallow && !encSw.longDone && screenOn) {
        if (scr == S_CL) clToggle(sel); else if (scr == S_NOTES) notesPress(); else if (scr == S_TRIP) spotSave(); else if (scr == S_WO) woPress(); else nextScreen();
      }
      encSw.swallow = false;
    }
  }
  if (encSw.down && !encSw.swallow && !encSw.longDone && now - encSw.t0 >= ENC_OFF_MS) { encSw.longDone = true; screenPower(false); }
  // turning
  int32_t delta = enc.getEncoderDelta();
  if (!delta) return;
  if (ENC_REVERSE) delta = -delta;
  if (touched(now)) return;                                // that click woke the screen (or stopped an alarm)
  if (scr == S_STATUS && btn.down && !btn.swallow) {       // holding the button: the knob sets brightness
    bright = true; btn.held = true;
    cfg.contrast = constrain(cfg.contrast + (int)delta * 8, 1, 255); oled.setContrast(cfg.contrast);
    msg = "Bright " + String((int)lroundf(cfg.contrast * 100.0f / 255)) + "%"; msgUntil = now + 1500;
  } else moveSel((int)delta);
}

void screenTick() {
  uint32_t now = millis();
  if (timeValid && now - lastTripCheck > 5000) { lastTripCheck = now; tripLoad(); }   // today's distance from the file, once
  alarmsTick(now);
  // button
  bool raw = digitalRead(btn.pin) == LOW;
  if (raw != btn.raw) { btn.raw = raw; btn.chg = now; }
  if (now - btn.chg > 25 && raw != btn.down) {
    btn.down = raw;
    if (raw) { btn.t0 = now; btn.held = false; btn.swallow = touched(now); }
    else {
      if (btn.swallow) btn.swallow = false;
      else if (bright) { bright = false; cfgSave(); msg = "Saved"; msgUntil = now + 800; redraw = true; }
      else if (!btn.held) onShort();
    }
  }
  if (btn.down && !btn.held && !btn.swallow && now - btn.t0 >= HOLD_MS) { btn.held = true; onHold(); }
  encTick(now);
  // alarms wake the screen; otherwise it sleeps after cfg.scrOffMin minutes untouched
  if (alarmActive()) { if (!screenOn) screenPower(true); lastInputMs = now; }
  else if (screenOn && cfg.scrOffMin > 0 && now - lastInputMs > (uint32_t)cfg.scrOffMin * 60000UL) screenPower(false);
  // button light: full with the screen on, dim while it sleeps; blinks on a tick; flashes during an alarm
  uint8_t led = screenOn ? 255 : (oledOK ? LED_DIM : 0);
  if (alarmActive()) led = (now / (bnStage == 1 ? 500 : 150)) % 2 ? 255 : 0;
  else if (now < ledBlinkUntil) led = (now / 100) % 2 ? 255 : 0;
  ledcWrite(BTN_LED, led);
  if (!screenOn) return;
  if (redraw || now - lastDrawMs > (alarmActive() ? 250u : mqOn ? 30u : 1000u)) { drawScreen(); lastDrawMs = now; redraw = false; }
}

// ---- web: timers, find-my-box, BNWAS ----
String alarmsJson() {
  uint32_t now = millis();
  String s = "{\"buzzer\":" + jBool(buzOK) + ",\"timers\":[";
  for (size_t i = 0; i < timers.size(); i++) {
    const KTimer& t = timers[i];
    if (i) s += ",";
    s += "{\"id\":" + String(t.id) + ",\"name\":" + jStr(t.name) + ",\"secs\":" + String(t.secs) + ",\"left\":" + String(t.left(now)) + ",\"ring\":" + jBool(t.ringing) + "}";
  }
  s += "],\"find\":" + jBool(findOn) + ",\"findLeft\":" + String(findOn ? (FIND_MAX_MS - min(FIND_MAX_MS, now - findSince)) / 1000 : 0);
  s += ",\"bnwas\":{\"on\":" + jBool(cfg.bnwasOn) + ",\"min\":" + String(cfg.bnwasMin) + ",\"stage\":" + String(bnStage) + ",\"left\":" + String(bnLeftS(now)) + "}}";
  return s;
}
void handleTimersGet() { server.send(200, "application/json", alarmsJson()); }
void handleTimersPost() {
  JsonDocument d;
  if (deserializeJson(d, server.arg("plain"))) { sendErr(400, "Bad request"); return; }
  String op = d["op"] | "";
  uint32_t now = millis();
  if (op == "add") {
    uint32_t secs = d["secs"] | 0;
    if (secs < 1 || secs > 86400) { sendErr(400, "Timer must be 1 second to 24 hours"); return; }
    if (timers.size() >= MAX_TIMERS) { sendErr(400, "Up to 8 timers at once"); return; }
    String name = ascii(d["name"] | ""); name.trim(); if (!name.length()) name = "Timer"; name = name.substring(0, 24);
    timers.push_back({ timerNextId++, secs, now + secs * 1000UL, 0, name, false });
  } else if (op == "del") {
    uint32_t id = d["id"] | 0;
    for (size_t i = 0; i < timers.size(); i++) if (timers[i].id == id) { timers.erase(timers.begin() + i); break; }
  } else if (op == "stop") {
    silence(false, now);                                     // timers and find-my-box; BNWAS needs the box's button
  } else if (op == "find") {
    findOn = d["on"] | false; findSince = now;
  } else if (op == "bnwas") {
    bool on = d["on"] | cfg.bnwasOn; int mn = d["min"] | cfg.bnwasMin;
    if (mn < 1 || mn > 240) { sendErr(400, "Interval must be 1 to 240 minutes"); return; }
    bool chg = on != cfg.bnwasOn || mn != cfg.bnwasMin;
    cfg.bnwasOn = on; cfg.bnwasMin = mn;
    if (chg) { bnwasRestart(now); if (flashOK && !cfgSave()) { sendErr(503, "Could not save"); return; } }
  } else if (op == "screen") {                            // {"op":"screen","offMin":5} and/or {"on":true}
    if (d["offMin"].is<int>()) {
      int m = d["offMin"];
      if (m < 0 || m > 240) { sendErr(400, "Sleep time must be 0 (never) to 240 minutes"); return; }
      if (m != cfg.scrOffMin) { cfg.scrOffMin = m; if (flashOK && !cfgSave()) { sendErr(503, "Could not save"); return; } }
    }
    if (d["on"].is<bool>()) { lastInputMs = now; screenPower(d["on"].as<bool>()); }
    if (d["clHide"].is<bool>() && d["clHide"].as<bool>() != cfg.clHideDone) {
      cfg.clHideDone = d["clHide"].as<bool>(); clVer = 0xFFFFFFFF;            // re-read the list
      if (flashOK && !cfgSave()) { sendErr(503, "Could not save"); return; }
    }
  } else { sendErr(400, "Unknown op"); return; }
  redraw = true;
  alarmsTick(now);
  server.send(200, "application/json", alarmsJson());
}

// ======================================================================
//  Web
// ======================================================================
String jStr(const String& s) {
  String o = "\"";
  for (size_t i = 0; i < s.length(); i++) {
    char c = s[i];
    if (c == '"' || c == '\\') { o += '\\'; o += c; }
    else if ((uint8_t)c < 0x20) o += ' ';
    else o += c;
  }
  return o + "\"";
}
String jNum(float v, int dec) { return isnan(v) ? String("null") : String(v, dec); }
String jBool(bool b) { return b ? "true" : "false"; }
String macStr(const uint8_t* m) {
  char b[18]; snprintf(b, sizeof b, "%02X:%02X:%02X:%02X:%02X:%02X", m[0], m[1], m[2], m[3], m[4], m[5]);
  return String(b);
}

void sendErr(int code, const char* msg) {
  server.send(code, "application/json", String("{\"ok\":false,\"err\":") + jStr(msg) + "}");
}

const char UPDATE_HTML[] PROGMEM = R"HTML(<!doctype html><html><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1"><title>Logbook update</title>
<style>body{font:15px system-ui;margin:40px auto;max-width:480px;padding:0 16px;background:#14171c;color:#e6e8eb}
input,button{font:inherit;margin:6px 0;padding:8px;width:100%;box-sizing:border-box}
button{background:#3b82f6;color:#fff;border:0;border-radius:6px;cursor:pointer}#m{margin-top:12px}.mut{color:#8b95a3;font-size:13px}</style></head>
<body><h2>Logbook update</h2><p>Pick the <b>.lbk</b> update file (dashboard + firmware in one). If this page came up instead of the
dashboard, the box has no dashboard yet: the .lbk puts it back.</p>
<input id="pw" type="password" placeholder="Box password (none on a new box)"><input id="f" type="file" accept=".lbk,.bin">
<button onclick="go()">Upload</button><div id="m"></div><p class="mut">The box restarts when it's done (about 20 s).</p>
<script>function go(){const f=document.getElementById('f').files[0];if(!f)return;
const fd=new FormData();fd.append('firmware',f);const x=new XMLHttpRequest();
x.open('POST','/update?pw='+encodeURIComponent(document.getElementById('pw').value));
x.upload.onprogress=e=>{document.getElementById('m').textContent=Math.round(100*e.loaded/e.total)+'%'};
x.onload=()=>{document.getElementById('m').textContent=x.responseText;if(x.status==200)setTimeout(()=>location.href='/',20000)};
x.send(fd)}</script></body></html>)HTML";

const char LOGIN_HTML[] PROGMEM = R"HTML(<!doctype html><html><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1"><title>Logbook</title>
<style>body{font:15px system-ui,-apple-system,"Segoe UI",Roboto,sans-serif;margin:0;min-height:100vh;display:flex;align-items:center;justify-content:center;background:#14171c;color:#e6e8eb}
.b{width:100%;max-width:360px;margin:16px;padding:26px;background:#1d2128;border:1px solid #2c323c;border-radius:14px}
h1{font-size:22px;margin:0 0 4px}p{color:#8b95a3;font-size:13.5px;margin:0 0 16px}
input[type=password]{font:inherit;width:100%;box-sizing:border-box;padding:10px;border-radius:8px;border:1px solid #39404b;background:#14171c;color:inherit}
label{display:flex;gap:8px;align-items:center;margin:12px 0;font-size:14px}button{font:inherit;width:100%;padding:10px;border:0;border-radius:8px;background:#3b82f6;color:#fff;cursor:pointer}
#m{min-height:20px;margin-top:10px;font-size:13.5px;color:#f87171}.f{text-align:center;color:#6b7480;font-size:12px;margin-top:14px}
@media (prefers-color-scheme:light){body{background:#f2f4f7;color:#1c2128}.b{background:#fff;border-color:#dde2e8}input[type=password]{background:#fff;border-color:#c9d0d9}}</style></head>
<body><form class="b" onsubmit="go();return false"><h1>Logbook</h1><p>You're reaching the box over your home Wi-Fi, so it asks for the box password. On the box's own hotspot it doesn't.</p>
<input id="pw" type="password" placeholder="Box password" autocomplete="current-password" autofocus>
<label><input id="k" type="checkbox" checked> Remember this device</label><button>Open Logbook</button><div id="m"></div><div class="f">Logbook · Aspect Wave LLC</div></form>
<script>async function go(){const m=document.getElementById('m');m.textContent='';
try{const r=await fetch('/api/login',{method:'POST',headers:{'Content-Type':'text/plain'},body:JSON.stringify({pw:document.getElementById('pw').value,keep:document.getElementById('k').checked})});
if(r.ok){location.reload();return}const j=await r.json().catch(()=>({}));m.textContent=j.err||('Error '+r.status)}catch(e){m.textContent='The box did not answer'}}</script></body></html>)HTML";

const char* uiSource() {
  if (sdOK && SD.exists("/www/index.html")) return "sd";              // test override on the card
  if (flashOK && LittleFS.exists("/www/index.html.gz")) return "flash";
  if (flashOK && LittleFS.exists("/www/index.html")) return "flash";
  return "none";
}

void handleRoot() {
  if (!authed()) { server.sendHeader("Cache-Control", "no-store"); server.send_P(200, "text/html", LOGIN_HTML); return; }
  const char* src = uiSource();
  if (!strcmp(src, "none")) { server.send_P(200, "text/html", UPDATE_HTML); return; }   // first install: ask for the .lbk
  fs::FS& fs = !strcmp(src, "sd") ? (fs::FS&)SD : (fs::FS&)LittleFS;
  bool gz = strcmp(src, "flash") == 0 && LittleFS.exists("/www/index.html.gz");
  File f = fs.open(gz ? "/www/index.html.gz" : "/www/index.html", FILE_READ);
  server.sendHeader("Cache-Control", "no-cache");
  server.streamFile(f, "text/html");                                  // .gz -> Content-Encoding: gzip
  f.close();
}

void handleStatus() {
  time_t t = time(nullptr);
  String s; s.reserve(2800);
  s += "{\"fw\":\"" FW_VERSION "\"";
  s += ",\"epoch\":" + String((uint32_t)t);
  s += ",\"local\":" + jStr(isoLocal(t));
  s += ",\"tz\":" + jStr(cfg.tz) + ",\"setupDone\":" + jBool(cfg.setupDone);
  s += ",\"passSet\":" + jBool(passSet()) + ",\"login\":" + jBool(needLogin()) + ",\"remembered\":" + String(sessKept());
  s += ",\"timeValid\":" + jBool(timeValid);
  s += ",\"timeSrc\":" + jStr(timeSrc);
  s += ",\"rtcOK\":" + jBool(rtcOK);
  s += ",\"store\":" + jStr(storeName);
  s += ",\"storeOK\":" + jBool(store != nullptr);
  s += ",\"flashOK\":" + jBool(flashOK);
  s += ",\"flashKB\":" + String(flashOK ? LittleFS.usedBytes() / 1024 : 0);
  s += ",\"flashTotalKB\":" + String(flashOK ? LittleFS.totalBytes() / 1024 : 0);
  s += ",\"sdOK\":" + jBool(sdOK);
  s += ",\"cardMB\":" + String(sdOK ? (uint32_t)(SD.cardSize() / (1024ULL * 1024ULL)) : 0);
  s += ",\"eeOK\":" + jBool(eeOK);
  s += ",\"lastAlive\":" + jStr(lastAlive > 1735689600UL ? isoLocal(lastAlive) : String(""));
  s += ",\"resetReason\":" + jStr(resetName(resetReason));
  s += ",\"logSeq\":" + String(logNextSeq - 1);
  s += ",\"logBytes\":" + String(logBytes);
  s += ",\"envLogS\":" + String(ENV_LOG_S);
  s += ",\"envFlashDays\":" + String(ENV_FLASH_DAYS);
  s += ",\"uiSrc\":" + jStr(uiSource());
  s += ",\"docVer\":" + String(docVer);
  s += ",\"sta\":{\"ssid\":" + jStr(cfg.homeSsid) + ",\"up\":" + jBool(WiFi.status() == WL_CONNECTED);
  s += ",\"ip\":" + jStr(WiFi.localIP().toString()) + ",\"rssi\":" + String(WiFi.status() == WL_CONNECTED ? WiFi.RSSI() : 0) + "}";
  s += ",\"ap\":{\"up\":" + jBool(apUp);
  s += ",\"ssid\":" + jStr(cfg.apSsid) + ",\"ip\":" + jStr(WiFi.softAPIP().toString()) + ",\"clients\":" + String(WiFi.softAPgetStationNum()) + "}";
  s += ",\"channel\":" + String(WiFi.channel());
  s += ",\"mdns\":" + jStr(String(MDNS_NAME) + ".local");
  uint8_t m[6]; WiFi.macAddress(m);
  s += ",\"hubMac\":" + jStr(macStr(m));
  s += ",\"uptime\":" + String(millis() / 1000);
  s += ",\"heap\":" + String(ESP.getFreeHeap());
  s += ",\"batt\":{\"ok\":" + jBool(battType != 0) + ",\"pct\":" + jNum(battPct, 0) + ",\"v\":" + jNum(battV, 2) + ",\"rate\":" + jNum(battRate, 1) + "}";
  s += ",\"gps\":{\"seen\":" + jBool(gps.seen && millis() - gps.lastNmeaMs < 5000) + ",\"fix\":" + jBool(gps.fix) + ",\"sats\":" + String(gps.sats);
  s += ",\"kn\":" + jNum(gps.kn, 1);
  if (gps.lastFixMs) s += ",\"lat\":" + String(gps.lat, 6) + ",\"lon\":" + String(gps.lon, 6) + ",\"fixAge\":" + String((millis() - gps.lastFixMs) / 1000);
  s += ",\"ptsToday\":" + String(gps.ptsDay == trackDate(t) ? gps.ptsToday : 0) + ",\"store\":" + jStr(sdOK ? "sd" : "flash") + "}";
  s += ",\"alarm\":" + alarmsJson();
  s += ",\"screen\":{\"ok\":" + jBool(oledOK) + ",\"on\":" + jBool(screenOn) + ",\"enc\":" + jBool(encOK) + ",\"encAddr\":" + String(encAddr) + ",\"offMin\":" + String(cfg.scrOffMin) + ",\"clHide\":" + jBool(cfg.clHideDone) + "}";
  s += ",\"gpsBus\":" + jBool(gpsI2C);
  s += ",\"nodes\":[";
  bool first = true;
  for (int i = 0; i < MAX_NODES; i++) {
    const Node& n = nodes[i];
    if (!n.used) continue;
    if (!first) s += ",";
    first = false;
    s += "{\"name\":" + jStr(n.name) + ",\"mac\":" + jStr(macStr(n.mac));
    s += ",\"tempC\":" + jNum(n.tempC, 2) + ",\"rh\":" + jNum(n.rh, 1);
    s += ",\"vbat\":" + jNum(n.vbat, 3) + ",\"soc\":" + jNum(n.soc, 1);
    uint32_t ag = nodeAge(n);
    s += ",\"rssi\":" + String(n.rssi) + ",\"age\":" + (ag == 0xFFFFFFFFUL ? String("null") : String(ag)) + ",\"live\":" + jBool(n.live);
    s += ",\"backfilled\":" + String(n.bfRows);
    s += ",\"interval\":" + String(n.interval) + ",\"count\":" + String(n.count);
    s += ",\"seq\":" + String(n.seq) + ",\"sensorOK\":" + jBool(n.flags & 1);
    s += "}";
  }
  s += "]}";
  server.sendHeader("Cache-Control", "no-store");
  server.send(200, "application/json", s);
}

void handleLogGet() {
  server.sendHeader("Cache-Control", "no-store");
  if (!store) { sendErr(503, "No storage available"); return; }
  File f = store->open(LOG_PATH, FILE_READ);
  if (!f) { server.send(200, "application/x-ndjson", ""); return; }
  server.streamFile(f, "application/x-ndjson");
  f.close();
}

// Body: one JSON event per line. Each is stored as {"seq":N,"ts":"<local ISO>","e":<event>}
void handleLogPost() {
  if (!store)     { sendErr(503, "No storage available"); return; }
  if (!timeValid) { sendErr(409, "Hub clock not set"); return; }
  String body = server.arg("plain");
  if (body.length() == 0 || body.length() > 32768) { sendErr(400, "Empty or oversized body"); return; }
  String ts; uint32_t first;
  int n = appendEvents(body, ts, first);
  if (n == -2) { sendErr(507, "The box's storage is full - nothing was saved. Insert or check the SD card, or make room in Setup."); return; }
  if (n < 0)  { sendErr(503, "Write failed"); return; }
  if (n == 0) { sendErr(400, "No valid events"); return; }
  server.send(200, "application/json",
              "{\"ok\":true,\"first\":" + String(first) + ",\"last\":" + String(logNextSeq - 1) + ",\"ts\":" + jStr(ts) + "}");
}

void listEnvDays(fs::FS& fs, String& out, bool& first, const String& skipIn) {
  File d = fs.open("/env");
  if (!d) return;
  File e = d.openNextFile();
  while (e) {
    String nm = e.name();
    int sl = nm.lastIndexOf('/');
    if (sl >= 0) nm = nm.substring(sl + 1);
    if (!e.isDirectory() && nm.endsWith(".csv") && nm.length() == 14) {
      String day = nm.substring(0, 10);
      if (skipIn.indexOf(day) < 0) {
        if (!first) out += ",";
        first = false;
        out += "\"" + day + "\"";
      }
    }
    e.close();
    e = d.openNextFile();
  }
  d.close();
}

void handleEnvDays() {
  String s = "[";
  bool first = true;
  if (sdOK) listEnvDays(SD, s, first, "");
  if (flashOK) listEnvDays(LittleFS, s, first, s);   // flash days not already on the card
  s += "]";
  server.sendHeader("Cache-Control", "no-store");
  server.send(200, "application/json", s);
}

bool validDate(const String& d) {
  if (d.length() != 10 || d[4] != '-' || d[7] != '-') return false;
  for (int i = 0; i < 10; i++) if (i != 4 && i != 7 && !isdigit((unsigned char)d[i])) return false;
  return true;
}

void handleEnv() {
  String d = server.arg("date");
  if (!validDate(d)) { sendErr(400, "date=YYYY-MM-DD"); return; }
  String path = "/env/" + d + ".csv";
  fs::FS* fs = nullptr;
  if (sdOK && SD.exists(path)) fs = &SD;
  else if (flashOK && LittleFS.exists(path)) fs = &LittleFS;
  if (!fs) { sendErr(404, "No file for that date"); return; }
  File f = fs->open(path, FILE_READ);
  server.sendHeader("Cache-Control", "no-store");
  server.streamFile(f, "text/csv");
  f.close();
}

void handleDownload() {
  String p = server.arg("f");
  if (!(p.startsWith("/hwo/") || p.startsWith("/env/") || p.startsWith("/gps/")) || p.indexOf("..") >= 0 || p.startsWith("/hwo/config") || p.startsWith("/hwo/sessions")) { sendErr(400, "Not allowed"); return; }
  fs::FS* fs = nullptr;
  if (p.startsWith("/hwo/") && store && store->exists(p)) fs = store;
  else if (sdOK && SD.exists(p)) fs = &SD;
  else if (flashOK && LittleFS.exists(p)) fs = &LittleFS;
  if (!fs) { sendErr(404, "Not found"); return; }
  File f = fs->open(p, FILE_READ);
  String fn = p.substring(p.lastIndexOf('/') + 1);
  if (p.startsWith("/gps/")) fn = "track_" + fn;
  server.sendHeader("Content-Disposition", "attachment; filename=\"" + fn + "\"");
  server.streamFile(f, "application/octet-stream");
  f.close();
}

void handleTime() {
  uint32_t e = (uint32_t)server.arg("epoch").toInt();
  if (e < 1735689600UL) { sendErr(400, "epoch looks wrong"); return; }   // before 2025-01-01
  if (timeValid && (!strcmp(timeSrc, "gps") || !strcmp(timeSrc, "ntp")) && server.arg("force") != "1") {
    server.send(200, "application/json", "{\"ok\":false,\"kept\":" + jStr(timeSrc) + "}"); return;
  }
  setSystemUtc((time_t)e);
  writeRtc((time_t)e);
  timeValid = true;
  timeSrc = "laptop";
  lastEnvSlot = 0;
  server.send(200, "application/json", "{\"ok\":true,\"local\":" + jStr(isoLocal((time_t)e)) + "}");
}


// ---------- attachments ----------
String safeName(const String& in, size_t maxLen) {
  String o;
  for (size_t i = 0; i < in.length() && o.length() < maxLen; i++) {
    char c = in[i];
    o += (isalnum((unsigned char)c) || c == '.' || c == '-' || c == '_') ? c : '_';
  }
  return o.length() ? o : String("file");
}

const char* mimeFor(const String& p) {
  String l = p; l.toLowerCase();
  if (l.endsWith(".jpg") || l.endsWith(".jpeg")) return "image/jpeg";
  if (l.endsWith(".png"))  return "image/png";
  if (l.endsWith(".gif"))  return "image/gif";
  if (l.endsWith(".webp")) return "image/webp";
  if (l.endsWith(".pdf"))  return "application/pdf";
  if (l.endsWith(".txt") || l.endsWith(".csv")) return "text/plain";
  return "application/octet-stream";
}

void handleAttUpload() {
  HTTPUpload& up = server.upload();
  if (up.status == UPLOAD_FILE_START) {
    attOK = false; attBytes = 0; attErr = "";
    if (!authed()) { attErr = "Log in"; return; }
    String id = safeName(server.arg("id"), 12);
    String fn = safeName(up.filename, 40);
    attPath = "/att/" + id + "_" + fn;
    if (!sdOK) { attErr = "Insert the microSD card to add attachments"; return; }
    if (!SD.exists("/att")) SD.mkdir("/att");
    attFile = SD.open(attPath, FILE_WRITE);
    attWhere = "sd";
    attOK = (bool)attFile;
    if (!attOK) attErr = "Could not create the file on the SD card";
  } else if (up.status == UPLOAD_FILE_WRITE) {
    if (!attOK) return;
    if (attFile.write(up.buf, up.currentSize) != up.currentSize) { attFile.close(); SD.remove(attPath); attOK = false; attErr = "SD write failed (card full?)"; return; }
    attBytes += up.currentSize;
  } else if (up.status == UPLOAD_FILE_END) {
    if (attFile) attFile.close();
  } else if (up.status == UPLOAD_FILE_ABORTED) {
    if (attFile) attFile.close();
    if (attOK) SD.remove(attPath);
    attOK = false; attErr = "Upload aborted";
  }
}

// ---------- documents (daily checklist + archived days) ----------
bool validIcsName(const String& n) {
  if (!n.startsWith("ics/") || n.length() < 5 || n.length() > 28) return false;
  for (size_t i = 4; i < n.length(); i++) { char c = n[i]; if (!isalnum((unsigned char)c) && c != '-' && c != '_') return false; }
  return true;
}
bool validCxName(const String& n) {   // archive search index: cx/YYYY-MM or cx/YYYY-MM-<part>
  if (!n.startsWith("cx/") || n.length() < 10 || n.length() > 13) return false;
  for (size_t i = 3; i < n.length(); i++) if (!isdigit((unsigned char)n[i]) && n[i] != '-') return false;
  return true;
}
bool validNoteName(const String& n) {   // notes nt/0 …, archived notes na/1 …, note history nh/1 … (up to 4 digits)
  if (!(n.startsWith("nt/") || n.startsWith("na/") || n.startsWith("nh/")) || n.length() < 4 || n.length() > 7) return false;
  for (size_t i = 3; i < n.length(); i++) if (!isdigit((unsigned char)n[i])) return false;
  return true;
}
bool validDocName(const String& n) {
  if (n == "checklist" || n == "oled") return true;
  if (validIcsName(n) || validCxName(n) || validNoteName(n)) return true;
  if (!n.startsWith("cl/") || n.length() < 13 || n.length() > 17) return false;
  for (size_t i = 3; i < n.length(); i++) if (!isdigit((unsigned char)n[i]) && n[i] != '-') return false;
  return true;
}
String docPath(const String& n) { return n == "checklist" ? String("/hwo/doc/checklist.json") : n == "oled" ? String("/hwo/doc/oled.json") : "/hwo/" + n + ".json"; }  // cl/<day>, cx/<month>, ics/<id>, nt/<k>, na/<k>
bool flashFirst(const String& n) { return n == "checklist" || n == "oled" || n.startsWith("nt/"); }   // kept in flash, copied to the card

void handleDocGet() {
  String n = server.arg("n");
  if (!validDocName(n)) { sendErr(400, "Bad document name"); return; }
  String p = docPath(n);
  fs::FS* fs = nullptr;
  if (flashFirst(n)) { if (flashOK && LittleFS.exists(p)) fs = &LittleFS; else if (sdOK && SD.exists(p)) fs = &SD; }
  else { if (sdOK && SD.exists(p)) fs = &SD; else if (flashOK && LittleFS.exists(p)) fs = &LittleFS; }
  if (!fs) { sendErr(404, "No such document"); return; }
  File f = fs->open(p, FILE_READ);
  server.sendHeader("Cache-Control", "no-store");
  server.streamFile(f, "application/json");
  f.close();
}

bool writeDoc(fs::FS& fs, const String& p, const String& body) {
  String tmp = p + ".tmp";
  File f = fs.open(tmp, FILE_WRITE);
  if (!f) return false;
  size_t w = f.print(body);
  f.close();
  if (w != body.length()) { fs.remove(tmp); return false; }
  fs.remove(p);
  return fs.rename(tmp, p);
}

void handleDocPut() {
  String n = server.arg("n");
  if (!validDocName(n)) { sendErr(400, "Bad document name"); return; }
  String body = server.arg("plain");
  if (body.length() < 2 || body.length() > 32768 || body[0] != '{') { sendErr(400, "Body must be a JSON object under 32 kB"); return; }
  if (validIcsName(n)) { sendErr(400, "Calendars go to /api/doc-up"); return; }
  String p = docPath(n);
  bool ok = false;
  if (n == "checklist" || n == "oled") {
    if (flashOK) { ensureDirs(LittleFS); ok = writeDoc(LittleFS, p, body); if (ok && sdOK) { ensureDirs(SD); writeDoc(SD, p, body); } }
    else if (sdOK) { ensureDirs(SD); ok = writeDoc(SD, p, body); }
  } else if (n.startsWith("nt/")) {      // notes: flash plus a copy on the card; card only once flash is nearly full
    size_t freeB = flashOK ? LittleFS.totalBytes() - LittleFS.usedBytes() + fileSize(LittleFS, p.c_str()) : 0;
    if (flashOK && freeB >= 128 * 1024) { ensureDirs(LittleFS); ok = writeDoc(LittleFS, p, body); if (ok && sdOK) { ensureDirs(SD); writeDoc(SD, p, body); } }
    else if (sdOK) { ensureDirs(SD); ok = writeDoc(SD, p, body); if (ok && flashOK) LittleFS.remove(p); }
    else if (flashOK) { sendErr(507, "Flash is nearly full - insert the SD card to keep saving notes"); return; }
  } else if (sdOK) {
    ensureDirs(SD); ok = writeDoc(SD, p, body);
  } else if (flashOK) {
    if (LittleFS.totalBytes() - LittleFS.usedBytes() < 150 * 1024) { sendErr(507, "Flash is nearly full - insert the SD card to keep archiving"); return; }
    ensureDirs(LittleFS); ok = writeDoc(LittleFS, p, body);
  }
  if (!ok) { sendErr(503, "Could not save the document"); return; }
  docVer++;
  server.send(200, "application/json", "{\"ok\":true,\"ver\":" + String(docVer) + "}");
}

void listDocs(fs::FS& fs, String& out, bool& first, const char* dir) {
  File d = fs.open(String("/hwo/") + dir);
  if (!d) return;
  File e = d.openNextFile();
  while (e) {
    String nm = e.name(); int sl = nm.lastIndexOf('/'); if (sl >= 0) nm = nm.substring(sl + 1);
    e.close();
    if (nm.endsWith(".json")) {
      String id = String(dir) + "/" + nm.substring(0, nm.length() - 5);
      if (out.indexOf("\"" + id + "\"") < 0) { if (!first) out += ","; first = false; out += "\"" + id + "\""; }
    }
    e = d.openNextFile();
  }
  d.close();
}

void handleDocList() {
  String s = "[";
  bool first = true;
  String d = server.arg("d");
  const char* dir = d == "ics" ? "ics" : d == "cx" ? "cx" : d == "nt" ? "nt" : d == "na" ? "na" : d == "nh" ? "nh" : "cl";
  if (sdOK) listDocs(SD, s, first, dir);
  if (flashOK) listDocs(LittleFS, s, first, dir);
  s += "]";
  server.sendHeader("Cache-Control", "no-store");
  server.send(200, "application/json", s);
}

// Imported calendars can be hundreds of kB: streamed to the card (or flash if there is room)
File upFile; String upPath, upErr; fs::FS* upFs = nullptr; bool upOK = false; size_t upBytes = 0;
void handleDocUpload() {
  HTTPUpload& up = server.upload();
  if (up.status == UPLOAD_FILE_START) {
    upOK = false; upBytes = 0; upErr = ""; upFs = nullptr;
    if (!authed()) { upErr = "Log in"; return; }
    String n = server.arg("n");
    if (!validIcsName(n)) { upErr = "Bad calendar name"; return; }
    upFs = sdOK ? (fs::FS*)&SD : (flashOK ? (fs::FS*)&LittleFS : nullptr);
    if (!upFs) { upErr = "No storage"; return; }
    ensureDirs(*upFs);
    upPath = docPath(n);
    upFile = upFs->open(upPath + ".tmp", FILE_WRITE);
    upOK = (bool)upFile;
    if (!upOK) upErr = "Could not create the file";
  } else if (up.status == UPLOAD_FILE_WRITE) {
    if (!upOK) return;
    if (upFs == &LittleFS && LittleFS.totalBytes() - LittleFS.usedBytes() < 250 * 1024) { upFile.close(); LittleFS.remove(upPath + ".tmp"); upOK = false; upErr = "Not enough room in flash - insert the SD card"; return; }
    if (upFile.write(up.buf, up.currentSize) != up.currentSize) { upFile.close(); upFs->remove(upPath + ".tmp"); upOK = false; upErr = "Write failed (card full?)"; return; }
    upBytes += up.currentSize;
  } else if (up.status == UPLOAD_FILE_END) {
    if (upFile) upFile.close();
    if (upOK) { upFs->remove(upPath); upOK = upFs->rename(upPath + ".tmp", upPath); if (!upOK) upErr = "Rename failed"; }
  } else if (up.status == UPLOAD_FILE_ABORTED) {
    if (upFile) upFile.close();
    if (upFs) upFs->remove(upPath + ".tmp");
    upOK = false; upErr = "Upload aborted";
  }
}

void handleDocDel() {
  String n = server.arg("n");
  if (!validIcsName(n) && !validCxName(n) && !validNoteName(n) && !(n.startsWith("cl/") && validDocName(n))) { sendErr(400, "Only imported calendars, archived checklist days, their index and note files can be removed"); return; }
  String p = docPath(n);
  bool a = sdOK && SD.exists(p) && SD.remove(p);
  bool b = flashOK && LittleFS.exists(p) && LittleFS.remove(p);
  docVer++;
  server.send(200, "application/json", String("{\"ok\":true,\"removed\":") + jBool(a || b) + "}");
}

// ---------- GPS track files ----------
void listGpsDays(fs::FS& fs, String& out, bool& first) {
  File d = fs.open("/gps");
  if (!d) return;
  File e = d.openNextFile();
  while (e) {
    String nm = e.name(); int sl = nm.lastIndexOf('/'); if (sl >= 0) nm = nm.substring(sl + 1);
    size_t sz = e.size();
    e.close();
    if (nm.endsWith(".csv") && nm.length() == 14 && out.indexOf(nm.substring(0, 10)) < 0) {
      if (!first) out += ",";
      first = false;
      out += "[\"" + nm.substring(0, 10) + "\"," + String(sz) + "]";
    }
    e = d.openNextFile();
  }
  d.close();
}
void handleGpsDays() {
  String s = "["; bool first = true;
  if (sdOK) listGpsDays(SD, s, first);
  if (flashOK) listGpsDays(LittleFS, s, first);
  s += "]";
  server.sendHeader("Cache-Control", "no-store");
  server.send(200, "application/json", s);
}
void handleGps() {
  String d = server.arg("date");
  if (!validDate(d)) { sendErr(400, "date=YYYY-MM-DD"); return; }
  String path = "/gps/" + d + ".csv";
  fs::FS* fs = (sdOK && SD.exists(path)) ? (fs::FS*)&SD : (flashOK && LittleFS.exists(path)) ? (fs::FS*)&LittleFS : nullptr;
  if (!fs) { sendErr(404, "No track for that date"); return; }
  File f = fs->open(path, FILE_READ);
  server.sendHeader("Cache-Control", "no-store");
  server.streamFile(f, "text/csv");
  f.close();
}

// ---------- restore a backup: replaces the work-order log (the old one is kept beside it) ----------
File rsFile; bool rsAuth = false, rsOK = false; size_t rsBytes = 0; String rsErr;
void handleRestoreUpload() {
  HTTPUpload& up = server.upload();
  if (up.status == UPLOAD_FILE_START) {
    rsAuth = pwCheck(server.arg("pw")); rsOK = false; rsBytes = 0; rsErr = "";
    if (!rsAuth) { rsErr = "Wrong password"; return; }
    if (!store) { rsErr = "No storage"; return; }
    rsFile = store->open("/hwo/log.new", FILE_WRITE);
    rsOK = (bool)rsFile;
    if (!rsOK) rsErr = "Could not write";
  } else if (up.status == UPLOAD_FILE_WRITE) {
    if (!rsOK) return;
    if (rsFile.write(up.buf, up.currentSize) != up.currentSize) { rsFile.close(); store->remove("/hwo/log.new"); rsOK = false; rsErr = "Write failed (full?)"; return; }
    rsBytes += up.currentSize;
  } else if (up.status == UPLOAD_FILE_END) {
    if (rsFile) rsFile.close();
  } else if (up.status == UPLOAD_FILE_ABORTED) {
    if (rsFile) rsFile.close();
    if (store) store->remove("/hwo/log.new");
    rsOK = false; rsErr = "Upload aborted";
  }
}
void handleRestoreDone() {
  if (!rsOK) { sendErr(rsAuth ? 400 : 403, rsErr.length() ? rsErr.c_str() : "Restore failed"); return; }
  File f = store->open("/hwo/log.new", FILE_READ);
  int c = f ? f.peek() : -1;
  if (f) f.close();
  if (rsBytes < 10 || c != '{') { store->remove("/hwo/log.new"); sendErr(400, "That is not a log file"); return; }
  store->remove("/hwo/log.before-restore.jsonl");
  if (store->exists(LOG_PATH)) store->rename(LOG_PATH, "/hwo/log.before-restore.jsonl");
  if (!store->rename("/hwo/log.new", LOG_PATH)) { sendErr(503, "Rename failed - old log kept as log.before-restore.jsonl"); return; }
  if (store == &LittleFS && sdOK) {
    if (SD.exists(LOG_PATH)) { SD.remove("/hwo/log.before-restore.jsonl"); SD.rename(LOG_PATH, "/hwo/log.before-restore.jsonl"); }
    copyFile(LittleFS, SD, LOG_PATH);
  }
  scanLog();
  server.send(200, "application/json", "{\"ok\":true,\"events\":" + String(logNextSeq - 1) + "}");
}

// ---------- settings ----------
void handleConfigGet() {
  server.sendHeader("Cache-Control", "no-store");
  server.send(200, "application/json", "{\"apSsid\":" + jStr(cfg.apSsid) + ",\"homeSsid\":" + jStr(cfg.homeSsid) + ",\"homeSet\":" + jBool(cfg.homePass.length() > 0) +
              ",\"tz\":" + jStr(cfg.tz) + ",\"setupDone\":" + jBool(cfg.setupDone) + ",\"passSet\":" + jBool(passSet()) + ",\"apDefault\":" + jBool(cfg.apPass == HUB_AP_PASS) + "}");
}
// Body: JSON with any of apSsid, apPass, homeSsid, homePass, tz, setupDone, newPass. Passwords left out stay as
// they are. Until a box password exists none is needed; after that ?pw= must be the box password.
void handleConfigPost() {
  if (!flashOK) { sendErr(503, "Flash not mounted"); return; }
  if (!pwCheck(server.arg("pw"))) { sendErr(pwLocked() ? 429 : 403, pwLocked() ? "Too many tries - wait 30 s" : "Wrong box password"); return; }
  JsonDocument d;
  if (deserializeJson(d, server.arg("plain"))) { sendErr(400, "Bad request"); return; }
  Cfg n = cfg;
  if (d["apSsid"].is<const char*>()) n.apSsid = (const char*)d["apSsid"];
  if (d["apPass"].is<const char*>() && strlen(d["apPass"])) n.apPass = (const char*)d["apPass"];
  if (d["homeSsid"].is<const char*>()) n.homeSsid = (const char*)d["homeSsid"];
  if (d["homePass"].is<const char*>()) n.homePass = (const char*)d["homePass"];
  if (d["tz"].is<const char*>() && strlen(d["tz"])) n.tz = (const char*)d["tz"];
  if (d["setupDone"].is<bool>()) n.setupDone = d["setupDone"];
  String np = d["newPass"] | "";
  if (np.length() && (np.length() < 8 || np.length() > 64)) { sendErr(400, "Box password: 8 to 64 characters"); return; }
  if (n.setupDone && !passSet() && !np.length()) { sendErr(400, "Choose a box password"); return; }
  if (n.setupDone && !cfg.setupDone && n.apPass == HUB_AP_PASS) { sendErr(400, "Choose your own hotspot password (the starting one is public)"); return; }
  n.apSsid.trim(); n.homeSsid.trim();
  if (!n.apSsid.length() || n.apSsid.length() > 32) { sendErr(400, "Hotspot name: 1 to 32 characters"); return; }
  if (n.apPass.length() < 8 || n.apPass.length() > 63) { sendErr(400, "Hotspot password: 8 to 63 characters"); return; }
  if (n.homeSsid.length() > 32 || n.homePass.length() > 63) { sendErr(400, "Home Wi-Fi name or password too long"); return; }
  bool ap = n.apSsid != cfg.apSsid || n.apPass != cfg.apPass, sta = n.homeSsid != cfg.homeSsid || n.homePass != cfg.homePass;
  Cfg old = cfg; cfg = n;
  if (np.length()) passSetTo(np);
  if (!cfgSave()) { cfg = old; sendErr(503, "Could not save"); return; }
  if (np.length()) { sess.clear(); sessSave(); if (!viaHotspot()) setCookie(sessNew(false), false); }   // a new password signs every device out
  cfgApplyAp = ap; cfgApplySta = sta; cfgApplyAt = millis() + 1500;   // after this reply has gone out
  server.send(200, "application/json", String("{\"ok\":true,\"apChanged\":") + jBool(ap) + ",\"homeChanged\":" + jBool(sta) + "}");
}
void handleScan() {
  if (scanning) { WiFi.scanDelete(); scanning = false; }
  int n = WiFi.scanNetworks(false, false, false, 120);
  String s = "["; int k = 0;
  for (int i = 0; i < n && k < 30; i++) {
    String id = WiFi.SSID(i); if (!id.length()) continue;
    bool dup = false; for (int j = 0; j < i; j++) if (WiFi.SSID(j) == id) { dup = true; break; }
    if (dup) continue;
    if (k++) s += ",";
    s += "{\"ssid\":" + jStr(id) + ",\"rssi\":" + String(WiFi.RSSI(i)) + "}";
  }
  WiFi.scanDelete(); lastScan = millis();
  server.send(200, "application/json", s + "]");
}


// ---------- login / logout / box password ----------
void handleLogin() {
  JsonDocument d; deserializeJson(d, server.arg("plain"));
  if (!needLogin()) { server.send(200, "application/json", "{\"ok\":true}"); return; }
  if (pwLocked()) { sendErr(429, "Too many tries - wait 30 s"); return; }
  if (!pwCheck(d["pw"] | "")) { sendErr(pwLocked() ? 429 : 403, pwLocked() ? "Too many tries - wait 30 s" : "Wrong password"); return; }
  bool keep = d["keep"] | false;
  setCookie(sessNew(keep), keep);
  server.send(200, "application/json", "{\"ok\":true}");
}
void handleLogout() {
  int i = sessFind();
  if (i >= 0) { bool k = sess[i].keep; sess.erase(sess.begin() + i); if (k) sessSave(); }
  server.sendHeader("Set-Cookie", "lb_s=; Path=/; Max-Age=0; SameSite=Strict");
  server.send(200, "application/json", "{\"ok\":true}");
}
void handlePassword() {   // {old, new}: change it; signs every device out (this one gets a fresh login)
  JsonDocument d; if (deserializeJson(d, server.arg("plain"))) { sendErr(400, "Bad request"); return; }
  String np = d["new"] | "";
  if (np.length() < 8 || np.length() > 64) { sendErr(400, "Box password: 8 to 64 characters"); return; }
  if (!pwCheck(d["old"] | "")) { sendErr(pwLocked() ? 429 : 403, pwLocked() ? "Too many tries - wait 30 s" : "The current password is wrong"); return; }
  String os = cfg.passSalt, oh = cfg.passHash;
  passSetTo(np);
  if (!cfgSave()) { cfg.passSalt = os; cfg.passHash = oh; sendErr(503, "Could not save"); return; }
  sess.clear(); sessSave();
  if (!viaHotspot()) setCookie(sessNew(false), false);
  server.send(200, "application/json", "{\"ok\":true}");
}
void handleForget() {     // forget every remembered device (this one stays logged in until the box restarts)
  bool here = !viaHotspot() && sessFind() >= 0;
  sess.clear(); sessSave();
  if (here) setCookie(sessNew(false), false);
  server.send(200, "application/json", "{\"ok\":true}");
}

bool serveAttachment(const String& uri) {
  if (!uri.startsWith("/att/") || uri.indexOf("..") >= 0) return false;
  if (!sdOK) { sendErr(503, "SD card not in — attachments live on the card"); return true; }
  if (!SD.exists(uri)) { sendErr(404, "Attachment not found on this SD card"); return true; }
  File f = SD.open(uri, FILE_READ);
  server.sendHeader("Cache-Control", "max-age=31536000, immutable");
  server.streamFile(f, mimeFor(uri));
  f.close();
  return true;
}

void setupWeb() {
  // no CORS: the dashboard comes from the box itself, so other web pages open on a phone can't read the box
  server.on("/", HTTP_GET, handleRoot);
  server.on("/api/status", HTTP_GET, G(handleStatus));
  server.on("/api/log", HTTP_GET, G(handleLogGet));
  server.on("/api/log", HTTP_POST, G(handleLogPost));
  server.on("/api/envdays", HTTP_GET, G(handleEnvDays));
  server.on("/api/env", HTTP_GET, G(handleEnv));
  server.on("/api/time", HTTP_POST, G(handleTime));
  server.on("/dl", HTTP_GET, G(handleDownload));
  server.on("/update", HTTP_GET, []() { server.send_P(200, "text/html", UPDATE_HTML); });
  server.on("/update", HTTP_POST,
    []() {
      bool ok = otaAuthorized && (upKind == 1 || upKind == 4) && !Update.hasError() && Update.isFinished();   // a cut-off file never counts
      if (otaAuthorized && !ok && !otaErr.length() && upKind != 9) otaErr = "Upload incomplete - nothing changed";
      if (ok && upKind == 4) {                       // bundle: the new dashboard goes live with the new firmware
        LittleFS.remove("/www/index.html.gz"); LittleFS.remove("/www/index.html");
        ok = LittleFS.rename("/www/index.tmp", "/www/index.html.gz");
        if (!ok) otaErr = "Firmware written but the dashboard could not be saved";
      } else LittleFS.remove("/www/index.tmp");
      server.sendHeader("Connection", "close");
      String msg = ok ? "Update OK - restarting" : (!otaAuthorized ? "Wrong password" : otaErr.length() ? otaErr : String("Update failed: ") + Update.errorString());
      server.send(ok ? 200 : (otaAuthorized ? 500 : 403), "text/plain", msg);
      if (ok) { delay(500); ESP.restart(); }
    },
    []() {
      HTTPUpload& up = server.upload();
      if (up.status == UPLOAD_FILE_START) {
        otaAuthorized = pwCheck(server.arg("pw"));
        upKind = 0; upHead = ""; otaErr = ""; upUiLeft = 0; upBinSize = 0;
      } else if (up.status == UPLOAD_FILE_WRITE) {
        if (!otaAuthorized || upKind == 9) return;
        const uint8_t* d = up.buf; size_t n = up.currentSize;
        while (n) {
          if (upKind == 0) {
            if (d[0] == 0xE9) { upKind = 1; Update.begin(UPDATE_SIZE_UNKNOWN); }
            else if (d[0] == 'L') upKind = 2;
            else { upKind = 9; otaErr = "Not a Logbook update file (.lbk) or firmware (.bin)"; return; }
          }
          if (upKind == 2) {
            while (n && upHead.length() < 300) { char c = *d++; n--; if (c == '\n') { upKind = 3; break; } upHead += c; }
            if (upKind == 2) { if (upHead.length() >= 300) { upKind = 9; otaErr = "Bad update file header"; } return; }
            if (!upHead.startsWith("LOGBOOK1")) { upKind = 9; otaErr = "Bad update file header"; return; }
            int u = upHead.indexOf("\"ui\":"), bi = upHead.indexOf("\"bin\":");
            if (u < 0 || bi < 0 || !flashOK) { upKind = 9; otaErr = flashOK ? "Bad update file header" : "Flash not mounted"; return; }
            upUiLeft = upHead.substring(u + 5).toInt(); upBinSize = upHead.substring(bi + 6).toInt();
            if (!LittleFS.exists("/www")) LittleFS.mkdir("/www");
            upUi = LittleFS.open("/www/index.tmp", FILE_WRITE);
            if (!upUi || upUiLeft < 1000 || upBinSize < 100000) { upKind = 9; otaErr = "Bad update file"; return; }
            continue;
          }
          if (upKind == 3) {
            size_t k = n < upUiLeft ? n : upUiLeft;
            if (upUi.write(d, k) != k) { upUi.close(); upKind = 9; otaErr = "Flash full - dashboard not saved"; return; }
            d += k; n -= k; upUiLeft -= k;
            if (!upUiLeft) { upUi.close(); upKind = 4; if (!Update.begin(upBinSize)) { upKind = 9; otaErr = String("Firmware: ") + Update.errorString(); return; } }
            continue;
          }
          if (upKind == 1 || upKind == 4) { Update.write((uint8_t*)d, n); n = 0; }
        }
      } else if (up.status == UPLOAD_FILE_END) {
        if (otaAuthorized && (upKind == 1 || upKind == 4)) Update.end(true);
        if (upUi) upUi.close();
      } else if (up.status == UPLOAD_FILE_ABORTED) {
        Update.abort(); if (upUi) upUi.close(); LittleFS.remove("/www/index.tmp");
      }
    });
  // dashboard .html upload -> flash
  server.on("/update-ui", HTTP_POST,
    []() {
      if (!flashOK)      { server.send(503, "text/plain", "Flash storage not mounted"); return; }
      if (!uiAuthorized) { server.send(uiBytes ? 507 : 403, "text/plain", uiBytes ? "Flash full - dashboard not changed" : "Wrong password"); return; }
      if (uiBytes < 2000) { LittleFS.remove("/www/index.tmp"); server.send(400, "text/plain", "File too small to be the dashboard"); return; }
      LittleFS.remove("/www/index.html"); LittleFS.remove("/www/index.html.gz");
      LittleFS.rename("/www/index.tmp", "/www/index.html");
      server.send(200, "text/plain", "Dashboard updated (" + String(uiBytes / 1024) + " kB)");
    },
    []() {
      HTTPUpload& up = server.upload();
      if (up.status == UPLOAD_FILE_START) {
        uiAuthorized = flashOK && pwCheck(server.arg("pw"));
        uiBytes = 0;
        if (uiAuthorized) {
          if (!LittleFS.exists("/www")) LittleFS.mkdir("/www");
          uiFile = LittleFS.open("/www/index.tmp", FILE_WRITE);
        }
      } else if (up.status == UPLOAD_FILE_WRITE) {
        if (uiAuthorized && uiFile) { if (uiFile.write(up.buf, up.currentSize) != up.currentSize) { uiFile.close(); LittleFS.remove("/www/index.tmp"); uiAuthorized = false; } else uiBytes += up.currentSize; }
      } else if (up.status == UPLOAD_FILE_END) {
        if (uiFile) uiFile.close();
      } else if (up.status == UPLOAD_FILE_ABORTED) {
        if (uiFile) uiFile.close();
        LittleFS.remove("/www/index.tmp");
        uiBytes = 0;
      }
    });
  server.on("/api/att", HTTP_POST,
    []() {
      if (!gate()) return;
      if (!attOK) { sendErr(507, attErr.length() ? attErr.c_str() : "Upload failed"); return; }
      server.send(200, "application/json", "{\"ok\":true,\"path\":" + jStr(attPath) + ",\"size\":" + String(attBytes) +
                  ",\"where\":" + jStr(attWhere) + "}");
    }, handleAttUpload);
  server.on("/api/doc", HTTP_GET, G(handleDocGet));
  server.on("/api/doc", HTTP_POST, G(handleDocPut));
  server.on("/api/docs", HTTP_GET, G(handleDocList));
  server.on("/api/doc-up", HTTP_POST, []() {
    if (!gate()) return;
    if (!upOK) { sendErr(507, upErr.length() ? upErr.c_str() : "Upload failed"); return; }
    docVer++;
    server.send(200, "application/json", "{\"ok\":true,\"size\":" + String(upBytes) + ",\"ver\":" + String(docVer) + "}");
  }, handleDocUpload);
  server.on("/api/doc-del", HTTP_POST, G(handleDocDel));
  server.on("/api/timers", HTTP_GET, G(handleTimersGet));
  server.on("/api/timers", HTTP_POST, G(handleTimersPost));
  server.on("/api/config", HTTP_GET, G(handleConfigGet));
  server.on("/api/config", HTTP_POST, G(handleConfigPost));
  server.on("/api/scan", HTTP_GET, G(handleScan));
  server.on("/api/gpsdays", HTTP_GET, G(handleGpsDays));
  server.on("/api/gps", HTTP_GET, G(handleGps));
  server.on("/api/restore", HTTP_POST, handleRestoreDone, handleRestoreUpload);
  server.on("/api/att-del", HTTP_POST, []() {
    if (!gate()) return;
    String p = server.arg("f");
    if (!p.startsWith("/att/") || p.indexOf("..") >= 0) { sendErr(400, "Not allowed"); return; }
    bool a = sdOK && SD.exists(p) && SD.remove(p);
    server.send(200, "application/json", String("{\"ok\":true,\"removed\":") + jBool(a) + "}");
  });
  server.on("/api/login", HTTP_POST, handleLogin);
  server.on("/api/logout", HTTP_POST, handleLogout);
  server.on("/api/password", HTTP_POST, G(handlePassword));
  server.on("/api/forget-devices", HTTP_POST, G(handleForget));
  server.onNotFound([]() { if (!gate()) return; if (!serveAttachment(server.uri())) sendErr(404, "Not found"); });
  const char* hk[] = { "Cookie" }; server.collectHeaders(hk, 1);   // the login cookie
  server.begin();
}

// ======================================================================
//  Setup / loop
// ======================================================================
void i2cPowerOn() {
#if defined(PIN_I2C_POWER)
  // ESP32-S2 Feather revisions differ in polarity: drive the opposite of the pin's rest state
  pinMode(PIN_I2C_POWER, INPUT);
  delay(1);
  bool rest = digitalRead(PIN_I2C_POWER);
  pinMode(PIN_I2C_POWER, OUTPUT);
  digitalWrite(PIN_I2C_POWER, !rest);
  delay(10);
#endif
}

void setup() {
  Serial.begin(115200);
  delay(300);
  resetReason = esp_reset_reason();
  Serial.printf("Logbook v" FW_VERSION "   reset: %s\n", resetName(resetReason));

  setenv("TZ", TZ_POSIX, 1);       // replaced by the saved time zone once flash is mounted
  tzset();

  i2cPowerOn();
  Wire.begin();
  rtcOK = rtc.begin();
  if (rtcOK) {
    if (rtc.initialized() && !rtc.lostPower()) {
      DateTime d = rtc.now();
      if (d.year() >= 2025) { setSystemUtc((time_t)d.unixtime()); timeValid = true; timeSrc = "rtc"; }
    }
    rtc.start();
  }
  hubSht = sht.begin(0x44);
  eeStart();
  Wire.setClock(400000);
  battStart();
  gpsStart();
  screenStart();

  flashOK = startFlash();
  if (flashOK) { cfgLoad(); sessLoad(); } else cfgDefaults();
  setenv("TZ", cfg.tz.c_str(), 1); tzset();
  sdOK = startSD();
  chooseStore();
  syncSdMirror();
  seedNodes();

  Serial.printf("Screen %s  GPS %s\n", oledOK ? "ok" : "none", gpsI2C ? "ok" : "none");
  Serial.printf("Flash %s  SD %s  RTC %s  EEPROM %s  SHT30 %s  battery gauge %s   log: %s, %lu events\n",
                flashOK ? "ok" : "FAILED (partition scheme?)", sdOK ? "ok" : "none", rtcOK ? "ok" : "none",
                eeOK ? "ok" : "none", hubSht ? "yes" : "no", battType == 1 ? "MAX17048" : battType == 2 ? "LC709203F" : "none",
                storeName, (unsigned long)(logNextSeq - 1));
  if (eeOK && lastAlive) Serial.printf("Last heartbeat: %lu (epoch)\n", (unsigned long)lastAlive);

  startWiFi();
  startEspNow();
  setupWeb();
  Serial.printf("House Wi-Fi %s  %s   hotspot %s  %s   ch %d   http://%s.local\n",
                staUp ? "up" : "down", WiFi.localIP().toString().c_str(), apUp ? "up" : "off",
                WiFi.softAPIP().toString().c_str(), (int)WiFi.channel(), MDNS_NAME);
  Serial.printf("MAC for the node sketches (HUB_MAC): %s\n", WiFi.macAddress().c_str());
}

void loop() {
  server.handleClient();
  processRx();

  if (ntpSynced) {
    ntpSynced = false;
    writeRtc(time(nullptr));
    timeValid = true;
    timeSrc = "ntp";
    lastNtpSync = millis();
  }
  gpsTick();
  battTick();
  screenTick();

  outageCheck();
  heartbeatTick();
  hubSensorTick();
  envLogTick();
  wifiTick();

  if (!sdOK && millis() - lastSdRetry > 30000) {   // card can be added later, no reboot needed
    lastSdRetry = millis();
    SD.end();
    sdOK = startSD();
    if (sdOK) {
      if (!store) chooseStore();
      syncSdMirror();
      Serial.println("SD card mounted");
    }
  }
  delay(2);
}
