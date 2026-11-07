#include <Arduino.h>
#include <FastLED.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <time.h>
#include "secrets.h"
#include "path_schedule.h"

// ---------------------------------------------------------------------------
// PATH Train Clock
// ---------------------------------------------------------------------------
// Ring: 60x WS2812B, 60 LEDs = 60 positions (clock face)
//   LED 0 at top = 12 o'clock.
//   - Real-time clock hands (hour / minute / second), NTP synced.
//   - Train hand points to the MINUTE-OF-HOUR the next train arrives
//     (e.g. train at 8:20 -> LED 20), colored by the train's line color.
//   DIN -> D5 / GPIO 5, 5V -> external supply, shared GND with ESP32.
// ---------------------------------------------------------------------------

#define LED_PIN     5
#define NUM_LEDS    60
#define BRIGHTNESS  64

// --- What we're tracking ---------------------------------------------------
#define STATION_CODE  "GRV"     // Grove St
#define DIRECTION     "ToNY"    // To New York

// --- Features --------------------------------------------------------------
#define SHOW_CLOCK_HANDS  true  // real-time hour/minute/second hands
#define TRAIN_TRAIL       0     // extra LEDs trailing each train hand (0 = single LED)
#define MAX_TRAINS        6     // how many upcoming trains to display
#define USE_SCHEDULE      true  // fill in trains beyond the 2 from the live API
#define FORCE_ALERT       false // TEST: force the service-alert display on

// --- Timing ----------------------------------------------------------------
#define FETCH_INTERVAL_MS  30000UL        // poll the live API every 30s
#define ALERTS_INTERVAL_MS (2UL*60UL*1000UL)      // check alerts every 2 min
#define SCHEDULE_INTERVAL_MS (6UL*3600UL*1000UL)  // refresh schedule every 6h
#define WIFI_RETRY_MS      10000UL

// --- Time zone (New York) --------------------------------------------------
#define TZ_STRING  "EST5EDT,M3.2.0,M11.1.0"

const char* API_URL =
    "https://www.panynj.gov/bin/portauthority/ridepath.json";

const char* ALERTS_URL =
    "https://www.panynj.gov/bin/portauthority/alerts.json";

// Remote schedule (updated by GitHub Action). Falls back to the baked-in
// path_schedule.h if this fetch fails.
const char* SCHEDULE_URL =
    "https://raw.githubusercontent.com/bsung1/path-clock/main/schedule.json";

CRGB leds[NUM_LEDS];

// Parsed train data
struct Train {
  time_t arrivalEpoch;   // absolute arrival time
  CRGB   color;          // line color
  bool   valid;
  bool   live;           // true = from real-time API, false = from schedule
  bool   viaHoboken;     // true = ATW route (alternates orange/blue)
  int    delayMin;       // minutes late vs schedule (0 = on time, <0 = early)
};

// Service alert state (from alerts.json)
bool alertActive = false;      // a delay alert is currently open
char alertText[64] = {0};      // short description for logging

Train trains[MAX_TRAINS];     // upcoming trains
int   trainCount = 0;         // how many are valid
unsigned long lastWifiTry = 0;
bool wifiOk = false;
bool timeOk = false;

// Mutex protects trains[]/trainCount between the fetch task and the render loop.
SemaphoreHandle_t trainsMutex = nullptr;

// --- Runtime schedule (downloaded from GitHub, falls back to baked-in) ------
#define MAX_SCHED 256
struct RtSched { uint16_t minute; uint8_t route; };
RtSched rtWeekday[MAX_SCHED];  uint16_t rtWeekdayCount = 0;
RtSched rtSaturday[MAX_SCHED]; uint16_t rtSaturdayCount = 0;
RtSched rtSunday[MAX_SCHED];   uint16_t rtSundayCount = 0;
bool rtScheduleLoaded = false; // true once a remote schedule is fetched

// Line colors (from PATH branding)
static const CRGB COLOR_33RD = CRGB(0xFF, 0x99, 0x00);  // orange - 33rd St
static const CRGB COLOR_WTC  = CRGB(0xD9, 0x3A, 0x30);  // red - WTC
static const CRGB COLOR_BLUE = CRGB(0x4D, 0x92, 0xFB);  // blue - via Hoboken hint

// Live vs scheduled shades (live = darker/saturated, scheduled = lighter amber)
static const CRGB COLOR_LIVE = CRGB(0x80, 0x38, 0x00);  // dark orange - live
static const CRGB COLOR_SCHED = CRGB(0xFF, 0xB0, 0x00); // light amber - scheduled (no blue)

// Forward declarations
void fillFromSchedule(Train* buf, int& count);
void computeDelays(Train* buf, int count);
void fetchTask(void* param);
bool fetchSchedule();
bool fetchAlerts();

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

// Parse a "RRGGBB" hex string into CRGB.
CRGB hexToColor(const char* hex) {
  if (!hex || strlen(hex) < 6) return CRGB::White;
  long v = strtol(hex, nullptr, 16);
  return CRGB((v >> 16) & 0xFF, (v >> 8) & 0xFF, v & 0xFF);
}

// Set one LED to a color scaled by brightness (max-combine so brighter wins).
void setPixel(int idx, CRGB color, uint8_t brightness) {
  idx = ((idx % NUM_LEDS) + NUM_LEDS) % NUM_LEDS;
  CRGB c = color;
  c.nscale8_video(brightness);
  leds[idx] = c;
}

// Draw a "hand" at a position with an optional trailing glow.
void drawHand(int pos, CRGB color, uint8_t brightness, int trail = TRAIN_TRAIL) {
  setPixel(pos, color, brightness);
  for (int i = 1; i <= trail; i++) {
    setPixel(pos - i, color, brightness / (3 * i));
  }
}

// Show a status color on the whole ring (used for boot / error states).
void showStatus(CRGB color) {
  fill_solid(leds, NUM_LEDS, color);
  FastLED.show();
}

// ---------------------------------------------------------------------------
// WiFi
// ---------------------------------------------------------------------------
void connectWifi() {
  Serial.printf("Connecting to WiFi \"%s\"", WIFI_SSID);
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

  unsigned long start = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - start < 15000) {
    delay(500);
    Serial.print(".");
  }
  Serial.println();

  if (WiFi.status() == WL_CONNECTED) {
    wifiOk = true;
    Serial.print("WiFi connected. IP: ");
    Serial.println(WiFi.localIP());
  } else {
    wifiOk = false;
    Serial.println("WiFi connection FAILED");
  }
}

// ---------------------------------------------------------------------------
// NTP time sync
// ---------------------------------------------------------------------------
void syncTime() {
  configTzTime(TZ_STRING, "pool.ntp.org", "time.nist.gov");
  Serial.print("Syncing time");
  struct tm t;
  unsigned long start = millis();
  while (!getLocalTime(&t, 1000) && millis() - start < 15000) {
    Serial.print(".");
  }
  Serial.println();
  if (getLocalTime(&t, 100)) {
    timeOk = true;
    Serial.printf("Time synced: %02d:%02d:%02d\n", t.tm_hour, t.tm_min, t.tm_sec);
  } else {
    timeOk = false;
    Serial.println("Time sync FAILED");
  }
}

// ---------------------------------------------------------------------------
// Fetch + parse PATH data
// ---------------------------------------------------------------------------
// Builds into a local buffer, then briefly locks the mutex to publish it.
// This keeps the render loop running smoothly (no multi-second stalls).
bool fetchTrains() {
  if (WiFi.status() != WL_CONNECTED) {
    wifiOk = false;
    return false;
  }
  wifiOk = true;

  HTTPClient http;
  http.begin(API_URL);
  http.setTimeout(10000);
  int code = http.GET();
  if (code != 200) {
    Serial.printf("HTTP GET failed: %d\n", code);
    http.end();
    return false;
  }

  String payload = http.getString();
  http.end();
  Serial.printf("HTTP 200, %u bytes\n", payload.length());

  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, payload);
  if (err) {
    Serial.printf("JSON parse failed: %s\n", err.c_str());
    return false;
  }

  // Build into a local buffer (no lock held during network/parse).
  Train local[MAX_TRAINS];
  for (auto& t : local) t.valid = false;
  int localCount = 0;

  time_t nowEpoch = time(nullptr);
  for (JsonObject st : doc["results"].as<JsonArray>()) {
    if (st["consideredStation"] != STATION_CODE) continue;

    for (JsonObject dest : st["destinations"].as<JsonArray>()) {
      if (dest["label"] != DIRECTION) continue;

      for (JsonObject msg : dest["messages"].as<JsonArray>()) {
        if (localCount >= MAX_TRAINS) break;
        // Only 33rd Street trains (skip World Trade Center).
        const char* head = msg["headSign"] | "";
        if (strstr(head, "33rd") == nullptr) continue;

        int secs = atoi(msg["secondsToArrival"] | "0");
        local[localCount].arrivalEpoch = nowEpoch + secs;
        local[localCount].color        = COLOR_LIVE;   // darker shade = live
        local[localCount].valid        = true;
        local[localCount].live         = true;
        local[localCount].viaHoboken   = false;
        local[localCount].delayMin     = 0;            // set below
        localCount++;
      }
    }
  }

  // Fill remaining slots from the static schedule.
  if (USE_SCHEDULE) fillFromSchedule(local, localCount);

  // --- Compute delay for each live train vs its nearest schedule slot ---
  // (fillFromSchedule already claimed slots, so we re-scan the timetable.)
  computeDelays(local, localCount);

  // Publish atomically.
  if (xSemaphoreTake(trainsMutex, portMAX_DELAY) == pdTRUE) {
    memcpy(trains, local, sizeof(trains));
    trainCount = localCount;
    xSemaphoreGive(trainsMutex);
  }

  int liveCount = 0;
  for (int i = 0; i < localCount; i++) if (local[i].live) liveCount++;
  Serial.printf("Fetched %d live + %d scheduled = %d total\n",
                liveCount, localCount - liveCount, localCount);
  for (int i = 0; i < localCount; i++) {
    struct tm t;
    localtime_r(&local[i].arrivalEpoch, &t);
    Serial.printf("  %s %d: %02d:%02d\n", local[i].live ? "LIVE" : "SCHED",
                  i + 1, t.tm_hour, t.tm_min);
  }
  return localCount > 0;
}

// ---------------------------------------------------------------------------
// Delay computation
// ---------------------------------------------------------------------------
// For each live train, find the nearest scheduled departure and record the
// difference in minutes. Positive = late, negative = early.
void computeDelays(Train* buf, int count) {
  struct tm nowTm;
  time_t nowEpoch = time(nullptr);
  localtime_r(&nowEpoch, &nowTm);

  const SchedEntry* sched;
  uint16_t schedCount;
  if (rtScheduleLoaded) {
    if (nowTm.tm_wday == 0)      { sched = (const SchedEntry*)rtSunday;   schedCount = rtSundayCount; }
    else if (nowTm.tm_wday == 6) { sched = (const SchedEntry*)rtSaturday; schedCount = rtSaturdayCount; }
    else                         { sched = (const SchedEntry*)rtWeekday;  schedCount = rtWeekdayCount; }
  } else if (nowTm.tm_wday == 0) { sched = SCHED_SUNDAY;   schedCount = SCHED_SUNDAY_COUNT; }
  else if (nowTm.tm_wday == 6)   { sched = SCHED_SATURDAY; schedCount = SCHED_SATURDAY_COUNT; }
  else                           { sched = SCHED_WEEKDAY;  schedCount = SCHED_WEEKDAY_COUNT; }

  for (int j = 0; j < count; j++) {
    if (!buf[j].live) continue;
    long bestDiff = 30 * 60;   // only match within 30 min
    for (uint16_t i = 0; i < schedCount; i++) {
      struct tm depTm = nowTm;
      depTm.tm_hour = sched[i].minute / 60;
      depTm.tm_min  = sched[i].minute % 60;
      depTm.tm_sec  = 0;
      long diff = labs((long)(mktime(&depTm) - buf[j].arrivalEpoch));
      if (diff < bestDiff) bestDiff = diff;
    }
    if (bestDiff < 30 * 60) {
      // Recompute signed difference against the best match.
      long bestSigned = 0; long bestAbs = 30 * 60;
      for (uint16_t i = 0; i < schedCount; i++) {
        struct tm depTm = nowTm;
        depTm.tm_hour = sched[i].minute / 60;
        depTm.tm_min  = sched[i].minute % 60;
        depTm.tm_sec  = 0;
        long signedDiff = (long)(buf[j].arrivalEpoch - mktime(&depTm));
        if (labs(signedDiff) < bestAbs) { bestAbs = labs(signedDiff); bestSigned = signedDiff; }
      }
      buf[j].delayMin = (int)(bestSigned / 60);
    } else {
      buf[j].delayMin = 0;
    }
  }
}

// ---------------------------------------------------------------------------
// Service alerts fetch (PATH alerts.json)
// ---------------------------------------------------------------------------
// Sets alertActive if there's an OPEN delay alert affecting our line.
bool fetchAlerts() {
  if (WiFi.status() != WL_CONNECTED) return false;

  HTTPClient http;
  http.begin(ALERTS_URL);
  http.setTimeout(10000);
  int code = http.GET();
  if (code != 200) { http.end(); return false; }
  String payload = http.getString();
  http.end();

  JsonDocument doc;
  if (deserializeJson(doc, payload)) return false;

  bool active = false;
  for (JsonObject a : doc.as<JsonArray>()) {
    const char* state = a["state"] | "";
    const char* tmpl  = a["TemplateName"] | "";
    const char* subj  = a["Subject"] | "";
    const char* msg   = a["SentMessage"] | "";

    if (strcmp(state, "Open") != 0) continue;
    // Only delay-type alerts.
    if (strstr(tmpl, "Delay") == nullptr) continue;

    // Only alerts affecting our line (JSQ-33 / 33rd Street).
    // PATH names lines like "JSQ-33", "NWK-WTC", "HOB-33".
    bool ourLine = strstr(subj, "JSQ-33") || strstr(msg, "JSQ-33") ||
                   strstr(subj, "33rd")   || strstr(msg, "33rd")   ||
                   strstr(subj, "JSQ")    || strstr(msg, "JSQ");
    if (!ourLine) continue;

    active = true;
    strncpy(alertText, subj, sizeof(alertText) - 1);
    break;
  }
  alertActive = active;
  Serial.printf("Alerts: %s\n", active ? alertText : "none");
  return true;
}

// ---------------------------------------------------------------------------
// Remote schedule fetch (from GitHub, updated by a GitHub Action)
// ---------------------------------------------------------------------------
// Parses {"weekday":[[min,route],...],"saturday":[...],"sunday":[...]}.
// On success, sets rtScheduleLoaded so fillFromSchedule uses it.
bool fetchSchedule() {
  if (WiFi.status() != WL_CONNECTED) return false;

  HTTPClient http;
  http.begin(SCHEDULE_URL);
  http.setTimeout(10000);
  int code = http.GET();
  if (code != 200) {
    Serial.printf("Schedule fetch failed: %d\n", code);
    http.end();
    return false;
  }
  String payload = http.getString();
  http.end();

  JsonDocument doc;
  if (deserializeJson(doc, payload)) {
    Serial.println("Schedule JSON parse failed");
    return false;
  }

  auto load = [](JsonArray arr, RtSched* out, uint16_t& cnt) {
    cnt = 0;
    for (JsonArray pair : arr) {
      if (cnt >= MAX_SCHED) break;
      out[cnt].minute = pair[0] | 0;
      out[cnt].route  = pair[1] | 0;
      cnt++;
    }
  };
  load(doc["weekday"].as<JsonArray>(),  rtWeekday,  rtWeekdayCount);
  load(doc["saturday"].as<JsonArray>(), rtSaturday, rtSaturdayCount);
  load(doc["sunday"].as<JsonArray>(),   rtSunday,   rtSundayCount);

  rtScheduleLoaded = (rtWeekdayCount > 0);
  Serial.printf("Remote schedule loaded: %u/%u/%u\n",
                rtWeekdayCount, rtSaturdayCount, rtSundayCount);
  return rtScheduleLoaded;
}

// ---------------------------------------------------------------------------
// Static schedule fill
// ---------------------------------------------------------------------------
// Adds upcoming 33rd Street departures from the embedded GTFS timetable into
// `buf`, skipping any already covered by a live train. Operates on a local
// buffer so no lock is needed here.
void fillFromSchedule(Train* buf, int& count) {
  struct tm nowTm;
  time_t nowEpoch = time(nullptr);
  localtime_r(&nowEpoch, &nowTm);

  // Prefer the downloaded schedule; fall back to the baked-in one.
  const SchedEntry* sched;
  uint16_t schedCount;
  if (rtScheduleLoaded) {
    // Remote schedule uses the same {minute, route} layout.
    if (nowTm.tm_wday == 0) {
      sched = (const SchedEntry*)rtSunday;   schedCount = rtSundayCount;
    } else if (nowTm.tm_wday == 6) {
      sched = (const SchedEntry*)rtSaturday; schedCount = rtSaturdayCount;
    } else {
      sched = (const SchedEntry*)rtWeekday;  schedCount = rtWeekdayCount;
    }
  } else if (nowTm.tm_wday == 0) {          // Sunday
    sched = SCHED_SUNDAY;   schedCount = SCHED_SUNDAY_COUNT;
  } else if (nowTm.tm_wday == 6) {   // Saturday
    sched = SCHED_SATURDAY; schedCount = SCHED_SATURDAY_COUNT;
  } else {                           // Mon-Fri
    sched = SCHED_WEEKDAY;  schedCount = SCHED_WEEKDAY_COUNT;
  }

  int nowMin = nowTm.tm_hour * 60 + nowTm.tm_min;

  // --- Dedup: each live train "claims" its nearest schedule slot ---
  // This handles delays: a live train 5 min late still suppresses its
  // scheduled counterpart instead of showing both.
  const long MATCH_WINDOW = 20 * 60;   // max seconds apart to be the same train
  bool claimed[256] = {false};         // one flag per schedule entry

  for (int j = 0; j < count; j++) {
    if (!buf[j].live) continue;
    long bestDiff = MATCH_WINDOW;
    int  bestIdx  = -1;
    for (uint16_t i = 0; i < schedCount; i++) {
      if (claimed[i]) continue;
      if (sched[i].minute <= nowMin) continue;
      struct tm depTm = nowTm;
      depTm.tm_hour = sched[i].minute / 60;
      depTm.tm_min  = sched[i].minute % 60;
      depTm.tm_sec  = 0;
      long diff = labs((long)(mktime(&depTm) - buf[j].arrivalEpoch));
      if (diff < bestDiff) { bestDiff = diff; bestIdx = i; }
    }
    if (bestIdx >= 0) claimed[bestIdx] = true;
  }

  // --- Add unclaimed schedule entries ---
  for (uint16_t i = 0; i < schedCount && count < MAX_TRAINS; i++) {
    if (claimed[i]) continue;
    int depMin = sched[i].minute;
    if (depMin <= nowMin) continue;          // already departed

    struct tm depTm = nowTm;
    depTm.tm_hour = depMin / 60;
    depTm.tm_min  = depMin % 60;
    depTm.tm_sec  = 0;

    buf[count].arrivalEpoch = mktime(&depTm);
    buf[count].color        = COLOR_SCHED;  // lighter shade = scheduled
    buf[count].valid        = true;
    buf[count].live         = false;
    buf[count].viaHoboken   = (sched[i].route == 1);
    buf[count].delayMin     = 0;
    count++;
  }

  // Sort by arrival time (simple insertion sort - tiny array).
  for (int i = 1; i < count; i++) {
    Train key = buf[i];
    int j = i - 1;
    while (j >= 0 && buf[j].arrivalEpoch > key.arrivalEpoch) {
      buf[j + 1] = buf[j];
      j--;
    }
    buf[j + 1] = key;
  }
}

// ---------------------------------------------------------------------------
// Setup / Loop
// ---------------------------------------------------------------------------
void setup() {
  Serial.begin(115200);
  delay(500);
  Serial.println("\nPATH Train Clock starting...");

  FastLED.addLeds<WS2812B, LED_PIN, GRB>(leds, NUM_LEDS);
  FastLED.setBrightness(BRIGHTNESS);
  FastLED.clear(true);

  showStatus(CRGB(0, 0, 40));   // dim blue = booting
  connectWifi();
  if (!wifiOk) showStatus(CRGB(40, 0, 0));  // red = no wifi

  // Create the mutex BEFORE any fetch (fetchTrains locks it).
  trainsMutex = xSemaphoreCreateMutex();

  if (wifiOk) {
    syncTime();
    fetchTrains();   // fetch immediately so we don't wait a full interval
  }

  // Background fetch task on core 0 (render loop runs on core 1).
  xTaskCreatePinnedToCore(fetchTask, "fetch", 8192, nullptr, 1, nullptr, 0);
}

// Runs on core 0: fetches on an interval without blocking rendering.
void fetchTask(void* param) {
  unsigned long lastSched = 0;
  unsigned long lastAlerts = 0;
  bool firstRun = true;
  for (;;) {
    // Refresh the remote schedule periodically (and immediately on first run).
    if (firstRun || millis() - lastSched > SCHEDULE_INTERVAL_MS) {
      lastSched = millis();
      fetchSchedule();
    }
    // Alerts change slowly - check every 2 minutes.
    if (firstRun || millis() - lastAlerts > ALERTS_INTERVAL_MS) {
      lastAlerts = millis();
      fetchAlerts();
    }
    firstRun = false;
    fetchTrains();
    vTaskDelay(pdMS_TO_TICKS(FETCH_INTERVAL_MS));
  }
}

void loop() {
  unsigned long now = millis();

  // Reconnect WiFi if dropped
  if (WiFi.status() != WL_CONNECTED && now - lastWifiTry > WIFI_RETRY_MS) {
    lastWifiTry = now;
    connectWifi();
  }

  // Snapshot the train list under the mutex (fetch task may be updating it).
  Train snapshot[MAX_TRAINS];
  int   snapCount = 0;
  if (xSemaphoreTake(trainsMutex, pdMS_TO_TICKS(10)) == pdTRUE) {
    memcpy(snapshot, trains, sizeof(snapshot));
    snapCount = trainCount;
    xSemaphoreGive(trainsMutex);
  }

  // Render
  FastLED.clear();

  if (!wifiOk) {
    // No WiFi: slow red pulse
    uint8_t b = beatsin8(30, 10, 60);
    fill_solid(leds, NUM_LEDS, CRGB(b, 0, 0));
  } else {
    // --- Real-time clock hands (drawn first, dimmer, single LED) ---
    if (SHOW_CLOCK_HANDS && timeOk) {
      struct tm t;
      time_t nowEpoch = time(nullptr);
      localtime_r(&nowEpoch, &t);

      // Hour hand: 12-hour position, moves smoothly with minutes.
      int hourPos = ((t.tm_hour % 12) * 5 + t.tm_min / 12) % NUM_LEDS;
      // Minute hand: minute-of-hour.
      int minPos = t.tm_min;
      // Second hand: second-of-minute.
      int secPos = t.tm_sec;

      drawHand(hourPos, CRGB(80, 0, 80), 120, 0);   // purple hour hand
      drawHand(minPos,  CRGB(0, 80, 0), 120, 0);    // green minute hand
      setPixel(secPos,  CRGB(80, 80, 80), 160);     // white second tick
    }

    // --- Train hands (drawn on top, brighter) ---
    // Only show trains within the next hour (the ring is a 60-minute face).
    time_t nowEpoch = time(nullptr);
    int shown = 0;
    for (int i = 0; i < snapCount; i++) {
      long secsUntil = (long)(snapshot[i].arrivalEpoch - nowEpoch);
      if (secsUntil < 0) continue;              // train already left
      int minsUntil = (secsUntil + 59) / 60;    // round up
      if (minsUntil > 60) continue;             // beyond the hour face

      struct tm t;
      localtime_r(&snapshot[i].arrivalEpoch, &t);

      // ATW (via Hoboken) trains alternate orange <-> blue so you can tell
      // them apart from the plain orange YEL trains.
      CRGB baseColor = snapshot[i].color;
      if (snapshot[i].viaHoboken) {
        bool bluePhase = (millis() / 500) % 2;   // swap every 500ms
        baseColor = bluePhase ? COLOR_BLUE : snapshot[i].color;
      }

      if (shown == 0) {
        // --- NEXT TRAIN: pulse (breathe), speeding up as it approaches ---
        if (minsUntil <= 4) {
          // Imminent: solid red, full brightness.
          drawHand(t.tm_min, CRGB(255, 0, 0), 255);
        } else {
          // Pulse rate rises as the train nears: ~0.25Hz far -> ~1.5Hz near.
          // Map minsUntil 4..60 -> bpm 15..90.
          int span = 60 - 4;
          int d = minsUntil - 4;
          if (d > span) d = span;
          uint8_t bpm = 15 + (uint8_t)((span - d) * 75 / span);
          uint8_t b = beatsin8(bpm, 40, 255);   // breathe 40..255
          drawHand(t.tm_min, baseColor, b);
        }
      } else {
        // --- Other trains: steady, fading with order ---
        uint8_t b = (shown == 1) ? 150 : (shown == 2) ? 100 : 60;
        drawHand(t.tm_min, baseColor, b);
      }
      shown++;
    }

    if (shown == 0 && !SHOW_CLOCK_HANDS) {
      // No upcoming trains and no clock: dim white ring as a fallback.
      fill_solid(leds, NUM_LEDS, CRGB(20, 20, 20));
    }

    // --- Service alert: whole ring breathes red (something's wrong) ---
    if (alertActive || FORCE_ALERT) {
      uint8_t b = beatsin8(10, 20, 120);   // slow red breath
      for (int i = 0; i < NUM_LEDS; i++) {
        leds[i] += CRGB(b, 0, 0);
      }
    }
  }

  FastLED.show();
  delay(50);
}