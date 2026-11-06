// Freenove FNK0104B: BKK departures for one stop, both directions. Stops and routes come from .env.
// Touch: tap = wake / switch view, hold = cycle screen timeout. BOOT button = refresh now.
#include <ArduinoJson.h>
#include <HTTPClient.h>
#include <Preferences.h>
#include <TFT_eSPI.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <Wire.h>
#include "ca.h"
#include "logic.h"
#include "config.h"

// Tuning knobs
const uint32_t POLL_MS = 30000;                // poll when a bus is due within NEAR_S
const uint32_t POLL_FAR_MS = 90000;            // poll when the next bus is further away
const long NEAR_S = 20 * 60;
const uint32_t RETRY_MS = 60000;               // retry interval after a failed request
const uint32_t MIN_GAP_MS = 10000;             // min time between any two requests (wake, BOOT)
const uint8_t TIMEOUTS_MIN[] = {5, 15, 30, 0};  // hold cycles these, 0 = always on
const uint8_t DEFAULT_TIMEOUT_IDX = 0;         // 5 min
const uint8_t BL_DAY = 255, BL_NIGHT = 40;     // backlight PWM 0-255
const int DIM_FROM_H = 22, DIM_TO_H = 6;       // night dimming hours
const uint32_t HOLD_MS = 1500;
const char* TZ_BUDAPEST = "CET-1CEST,M3.5.0,M10.5.0/3";

// Board pins (Freenove FNK0104B)
const int KEY_PIN = 0, TP_SDA = 16, TP_SCL = 15, TP_RST = 18;
const uint8_t TP_ADDR = 0x38;  // FT6336U

const char* URL =
    "https://futar.bkk.hu/api/query/v1/ws/otp/api/where/arrivals-and-departures-for-stop.json"
    "?stopId=" CFG_STOP_A "&stopId=" CFG_STOP_B "&minutesBefore=0&minutesAfter=90&onlyDepartures=true"
    "&limit=30&includeReferences=trips,alerts&key=" CFG_BKK_KEY;
const char* STOPS[2] = {CFG_STOP_A, CFG_STOP_B};
const char* TITLES[2] = {CFG_STOP_A_TITLE, CFG_STOP_B_TITLE};
struct Route { const char* id; const char* name; };
const Route ROUTES[] = CFG_ROUTES;

const int DATE_X = 238, DATE_W = 110;  // header date: right edge, max width
const int MAXD = 8, MAX_ROWS = 6, ROW_H = 26, FOOT_Y = 219;
const uint16_t BKK_BLUE = 0x04FC;  // #009EE3

struct Dep { const char* route; char head[24]; time_t t; bool live, canceled; int delay; };
struct Data { Dep deps[2][MAXD]; int n[2]; char alert[640]; time_t lastOk; int err; };  // err: 0 ok, HTTP code, or -100 JSON

Data shared;  // written by fetch task, guarded by mtx
Data cur;     // render copy, loop task only
SemaphoreHandle_t mtx;
TaskHandle_t fetchTaskH;
volatile bool screenOn = true;

TFT_eSPI tft;
TFT_eSprite foot(&tft);
Preferences prefs;
char rowCache[MAX_ROWS][64];
char footCache[96];
char tickText[640];
int tickX, tickW;
int view;  // 0 = both directions, 1/2 = one direction full height
uint8_t timeoutIdx;
uint32_t lastActivity, msgUntil;
char msg[48];

// --- fetch (own FreeRTOS task, so drawing never stalls) ---

void logFail(WiFiClientSecure& client, const char* what, int code) {
  char tls[80] = "";
  client.lastError(tls, sizeof tls);
  Serial.printf("FAIL %s %d | tls: %s | rssi %d | heap %u, max block %u | up %lus, reset reason %d\n", what, code, tls,
                (int)WiFi.RSSI(), ESP.getFreeHeap(), ESP.getMaxAllocHeap(), millis() / 1000, (int)esp_reset_reason());
}

// Returns 0 on success, else the HTTP code (negative = connection error) or -100 for bad JSON.
int fetch(Data& out) {
  WiFiClientSecure client;
  client.setCACert(BKK_ROOT_CA);
  HTTPClient http;
  http.useHTTP10(true);  // no chunked encoding, stream straight into the parser
  http.setTimeout(10000);
  http.begin(client, URL);
  int code = http.GET();
  if (code != 200) {
    logFail(client, "HTTP", code);
    http.end();
    return code;
  }
  JsonDocument filter;
  JsonObject st = filter["data"]["entry"]["stopTimes"].add<JsonObject>();
  for (const char* k : {"stopId", "tripId", "stopHeadsign", "departureTime", "predictedDepartureTime", "canceled", "alertIds"})
    st[k] = true;
  filter["data"]["references"]["trips"] = true;
  filter["data"]["references"]["alerts"] = true;
  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, http.getStream(), DeserializationOption::Filter(filter));
  http.end();
  if (err) {
    logFail(client, err.c_str(), -100);
    return -100;
  }

  JsonObject refs = doc["data"]["references"];
  time_t now = time(nullptr);
  out.n[0] = out.n[1] = 0;
  out.alert[0] = 0;
  const char* seen[4] = {};
  int nSeen = 0;
  for (JsonObject s : doc["data"]["entry"]["stopTimes"].as<JsonArray>()) {
    const char* sid = s["stopId"] | "";
    int dir = !strcmp(sid, STOPS[0]) ? 0 : !strcmp(sid, STOPS[1]) ? 1 : -1;
    if (dir < 0 || out.n[dir] >= MAXD) continue;
    const char* rid = refs["trips"][s["tripId"].as<const char*>()]["routeId"] | "";
    const char* name = nullptr;
    for (const Route& r : ROUTES) if (!strcmp(rid, r.id)) name = r.name;
    if (!name) continue;
    Dep& d = out.deps[dir][out.n[dir]++];
    d.route = name;
    asciify(s["stopHeadsign"] | "", d.head, sizeof d.head);
    time_t sched = s["departureTime"] | 0L;
    time_t pred = s["predictedDepartureTime"] | 0L;
    d.live = pred != 0;
    d.t = d.live ? pred : sched;
    d.delay = d.live ? (int)((pred - sched + 30) / 60) : 0;
    d.canceled = s["canceled"] | false;

    // Alerts: "<route>: header - description", each alert once
    for (const char* aid : s["alertIds"].as<JsonArray>()) {
      bool dup = false;
      for (int i = 0; i < nSeen; i++) dup |= !strcmp(seen[i], aid);
      JsonObject a = refs["alerts"][aid];
      if (dup || nSeen >= 4 || (a["start"] | 0L) > now || now > (a["end"] | (long)now)) continue;
      seen[nSeen++] = aid;
      static char html[700], text[640], head[48];
      asciify(a["header"]["translations"]["hu"] | "", head, sizeof head);
      stripHtml(a["description"]["translations"]["hu"] | "", html, sizeof html);
      asciify(html, text, sizeof text);
      size_t o = strlen(out.alert);
      snprintf(out.alert + o, sizeof out.alert - o, "%s%s: %s - %s", o ? "   |   " : "", name, head, text);
    }
  }
  out.lastOk = now;
  Serial.printf("ok %d+%d deps, alert %u chars, heap %u, max block %u, rssi %d\n", out.n[0], out.n[1],
                strlen(out.alert), ESP.getFreeHeap(), ESP.getMaxAllocHeap(), (int)WiFi.RSSI());
  return 0;
}

uint32_t nextInterval(const Data& d) {
  time_t now = time(nullptr);
  for (int dir = 0; dir < 2; dir++)
    for (int i = 0; i < d.n[dir]; i++)
      if (d.deps[dir][i].t >= now && d.deps[dir][i].t - now <= NEAR_S) return POLL_MS;
  return POLL_FAR_MS;
}

void fetchTask(void*) {
  static Data tmp;
  uint32_t wait = 0, lastTry = 0 - MIN_GAP_MS;  // first attempt passes the gap check
  for (;;) {
    ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(wait));  // notify = fetch now
    if (!screenOn || time(nullptr) < 1700000000 || WiFi.status() != WL_CONNECTED) { wait = 1000; continue; }
    uint32_t since = millis() - lastTry;
    if (since < MIN_GAP_MS) { wait = MIN_GAP_MS - since; continue; }
    lastTry = millis();
    int err = fetch(tmp);
    xSemaphoreTake(mtx, portMAX_DELAY);
    if (!err) shared = tmp;
    shared.err = err;
    xSemaphoreGive(mtx);
    wait = err ? RETRY_MS : nextInterval(tmp);
  }
}

// --- drawing (loop task) ---

void drawRow(int idx, int y, const Dep* d, time_t now) {
  char key[64] = "-", lbl[8] = "", dl[8] = "";
  uint16_t col = TFT_WHITE;
  if (d) {
    if (d->canceled) { strcpy(lbl, "elmarad"); col = TFT_RED; }
    else {
      minutesLabel(d->t, now, lbl, sizeof lbl);
      if (d->live) col = d->delay >= 2 ? TFT_ORANGE : TFT_GREEN;
      if (d->live && d->delay >= 2) snprintf(dl, sizeof dl, "+%d", d->delay);
    }
    snprintf(key, sizeof key, "%s|%s|%s|%s|%u", d->route, d->head, lbl, dl, col);
  }
  if (!strcmp(key, rowCache[idx])) return;  // redraw only on change
  strcpy(rowCache[idx], key);
  tft.fillRect(0, y, 320, ROW_H, TFT_BLACK);
  if (!d) return;
  int my = y + ROW_H / 2;
  tft.fillRoundRect(4, y + 2, 46, ROW_H - 4, 4, BKK_BLUE);
  tft.setTextColor(TFT_WHITE, BKK_BLUE);
  tft.setTextDatum(MC_DATUM);
  tft.drawString(d->route, 27, my, 2);

  tft.setTextDatum(MR_DATUM);
  tft.setTextColor(col, TFT_BLACK);
  int x = 316 - tft.drawString(lbl, 316, my, d->canceled ? 2 : 4) - 6;
  if (dl[0]) {
    tft.setTextColor(TFT_ORANGE, TFT_BLACK);
    x -= tft.drawString(dl, x, my, 2) + 6;
  }
  char head[24];
  strcpy(head, d->head);
  for (size_t n = strlen(head); n && tft.textWidth(head, 2) > x - 58; n--) head[n - 1] = 0;
  tft.setTextDatum(ML_DATUM);
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.drawString(head, 58, my, 2);
}

void drawLayout() {
  tft.fillRect(0, 22, 320, FOOT_Y - 22, TFT_BLACK);
  memset(rowCache, 0, sizeof rowCache);
  tft.setTextDatum(ML_DATUM);
  tft.setTextColor(TFT_LIGHTGREY, TFT_BLACK);
  char t[48];
  for (int i = 0; i < 2; i++) {
    if (view && view - 1 != i) continue;
    asciify(TITLES[i], t, sizeof t);
    tft.drawString(t, 4, view ? 32 : 32 + i * 98, 2);
  }
}

void render(time_t now) {
  xSemaphoreTake(mtx, portMAX_DELAY);
  cur = shared;
  xSemaphoreGive(mtx);

  struct tm lt;
  localtime_r(&now, &lt);
  char clk[9];
  strftime(clk, sizeof clk, "%H:%M:%S", &lt);
  tft.setTextDatum(MR_DATUM);
  tft.setTextColor(TFT_WHITE, BKK_BLUE);
  tft.setTextPadding(70);
  tft.drawString(lt.tm_year > 120 ? clk : "--:--:--", 316, 11, 2);
  tft.setTextPadding(0);

  static const char* DAYS[] = {"Vasarnap", "Hetfo", "Kedd", "Szerda", "Csutortok", "Pentek", "Szombat"};
  static int lastDay = -1;
  if (lt.tm_year > 120 && lt.tm_yday != lastDay) {  // date changes once a day
    lastDay = lt.tm_yday;
    char date[24];
    snprintf(date, sizeof date, "%02d.%02d. %s", lt.tm_mon + 1, lt.tm_mday, DAYS[lt.tm_wday]);
    tft.setTextPadding(DATE_W);
    tft.drawString(date, DATE_X, 11, 2);
    tft.setTextPadding(0);
  }

  int rows = view == 0 ? 3 : MAX_ROWS;
  for (int k = 0; k < (view == 0 ? 2 : 1); k++) {
    int dir = view == 0 ? k : view - 1, y0 = 42 + k * 98, r = 0;
    for (int i = 0; i < cur.n[dir] && r < rows; i++) {
      if (cur.deps[dir][i].t < now - 30) continue;  // departed since last poll
      drawRow(k * rows + r, y0 + r * ROW_H, &cur.deps[dir][i], now);
      r++;
    }
    for (; r < rows; r++) drawRow(k * rows + r, y0 + r * ROW_H, nullptr, now);
  }
}

// Footer: status line, or the alert text scrolling. Called every ~30 ms.
void drawFooter(time_t now) {
  char line[96];
  uint16_t col = TFT_DARKGREY;
  long age = cur.lastOk ? (long)(now - cur.lastOk) : -1;
  bool ticker = false;
  if ((int32_t)(millis() - msgUntil) < 0) { snprintf(line, sizeof line, "%s", msg); col = TFT_CYAN; }
  else if (WiFi.status() != WL_CONNECTED) { snprintf(line, sizeof line, "WiFi kapcsolodas..."); col = TFT_RED; }
  else if (age < 0) snprintf(line, sizeof line, "Adatok betoltese...");
  else if (age > 180) {
    int o = snprintf(line, sizeof line, "Regi adat: %ld perce", age / 60);
    if (cur.err) snprintf(line + o, sizeof line - o, " (hiba %d)", cur.err);
    col = TFT_RED;
  }
  else if (cur.alert[0]) ticker = true;
  else snprintf(line, sizeof line, "WiFi %d dBm", (int)WiFi.RSSI());

  if (!ticker && !strcmp(line, footCache)) return;
  foot.fillSprite(TFT_BLACK);
  foot.setTextDatum(ML_DATUM);
  if (!ticker) {
    strcpy(footCache, line);
    foot.setTextColor(col, TFT_BLACK);
    foot.drawString(line, 4, 10, 2);
    foot.pushSprite(0, FOOT_Y);
    return;
  }
  footCache[0] = 0;
  if (strcmp(tickText, cur.alert)) {
    strcpy(tickText, cur.alert);
    tickW = foot.textWidth(tickText, 2);
    tickX = 4;
  }
  foot.setTextColor(TFT_YELLOW, TFT_BLACK);
  foot.drawString(tickText, tickX, 10, 2);
  foot.pushSprite(0, FOOT_Y);
  if (tickW > 312 && (tickX -= 2) < -tickW) tickX = 320;
}

// --- input ---

int touchCount() {
  Wire.beginTransmission(TP_ADDR);
  Wire.write(0x02);  // TD_STATUS
  if (Wire.endTransmission(false) != 0 || Wire.requestFrom(TP_ADDR, (uint8_t)1) != 1) return 0;
  int n = Wire.read() & 0x0F;
  return n <= 2 ? n : 0;
}

void showMsg(const char* m) {
  snprintf(msg, sizeof msg, "%s", m);
  msgUntil = millis() + 3000;
}

void cycleTimeout() {
  timeoutIdx = (timeoutIdx + 1) % sizeof TIMEOUTS_MIN;
  prefs.putUChar("tout", timeoutIdx);
  char m[48];
  if (TIMEOUTS_MIN[timeoutIdx]) snprintf(m, sizeof m, "Kepernyo kikapcsol: %u perc", TIMEOUTS_MIN[timeoutIdx]);
  else snprintf(m, sizeof m, "Kepernyo: mindig bekapcsolva");
  showMsg(m);
}

void setup() {
  Serial.begin(115200);
  Serial.printf("boot, reset reason %d\n", (int)esp_reset_reason());
  pinMode(KEY_PIN, INPUT_PULLUP);
  pinMode(TP_RST, OUTPUT);
  digitalWrite(TP_RST, LOW);
  delay(10);
  digitalWrite(TP_RST, HIGH);
  Wire.begin(TP_SDA, TP_SCL);
  prefs.begin("cfg");
  timeoutIdx = prefs.getUChar("tout", DEFAULT_TIMEOUT_IDX) % sizeof TIMEOUTS_MIN;

  tft.init();
  tft.setRotation(1);
  ledcSetup(0, 5000, 8);
  ledcAttachPin(TFT_BL, 0);
  ledcWrite(0, BL_DAY);
  tft.fillScreen(TFT_BLACK);
  tft.fillRect(0, 0, 320, 22, BKK_BLUE);
  tft.setTextColor(TFT_WHITE, BKK_BLUE);
  tft.setTextDatum(ML_DATUM);
  char title[40];
  asciify(CFG_STOP_NAME, title, sizeof title);
  for (size_t n = strlen(title); n && tft.textWidth(title, 2) > DATE_X - DATE_W - 12; n--) title[n - 1] = 0;
  tft.drawString(title, 4, 11, 2);
  drawLayout();
  foot.createSprite(320, 20);

  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(true);
  WiFi.begin(CFG_WIFI_SSID, CFG_WIFI_PASS);
  configTzTime(TZ_BUDAPEST, "pool.ntp.org", "time.google.com");

  mtx = xSemaphoreCreateMutex();
  xTaskCreatePinnedToCore(fetchTask, "fetch", 16384, nullptr, 1, &fetchTaskH, 0);
  lastActivity = millis();
}

void loop() {
  static uint32_t lastTouchPoll, lastTick, lastFoot, touchStart;
  static bool touching, held, touchWoke, keyWas = HIGH;
  static uint8_t bl = BL_DAY;
  uint32_t ms = millis();
  time_t now = time(nullptr);

  if (ms - lastTouchPoll >= 30) {
    lastTouchPoll = ms;
    bool t = touchCount() > 0;
    if (t && !touching) { touching = true; held = false; touchStart = ms; touchWoke = !screenOn; lastActivity = ms; }
    else if (t && !held && ms - touchStart >= HOLD_MS) { held = true; cycleTimeout(); }
    else if (!t && touching) {
      touching = false;
      if (!held && !touchWoke) { view = (view + 1) % 3; drawLayout(); }
    }
  }
  bool key = digitalRead(KEY_PIN);
  if (keyWas == HIGH && key == LOW) { lastActivity = ms; xTaskNotifyGive(fetchTaskH); showMsg("Frissites..."); }
  keyWas = key;

  uint32_t tout = TIMEOUTS_MIN[timeoutIdx] * 60000UL;
  bool on = !tout || ms - lastActivity < tout;
  if (on && !screenOn && now - cur.lastOk > 15) xTaskNotifyGive(fetchTaskH);  // woke with old data
  screenOn = on;

  struct tm lt;
  localtime_r(&now, &lt);
  uint8_t want = !on ? 0 : (lt.tm_year > 120 && (lt.tm_hour >= DIM_FROM_H || lt.tm_hour < DIM_TO_H)) ? BL_NIGHT : BL_DAY;
  if (want != bl) ledcWrite(0, bl = want);

  if (on && ms - lastTick >= 1000) { lastTick = ms; render(now); }
  if (on && ms - lastFoot >= 30) { lastFoot = ms; drawFooter(now); }
  delay(5);
}
