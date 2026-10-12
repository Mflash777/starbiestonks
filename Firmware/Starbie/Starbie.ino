/*
  Starbie  -  talking car + gyro steering wheel + live stocks
  Hack Club Half-Life, Week 1

  Hardware (matches the PCB):
    XIAO ESP32-C3
    SSD1306 128x64 OLED  (I2C 0x3C)   SDA = GPIO6 (D4), SCL = GPIO7 (D5)
    MPU6050 gyro/accel   (I2C 0x68)   same I2C bus
    DHT11                              GPIO3 (D1)
    BTN1                               GPIO4 (D2)  -> to GND
    BTN2                               GPIO5 (D3)  -> to GND

  Views (exactly four, so the menu stays at four items):
    CAR     talking car + steering wheel that turns with the board
    DRIVE   tilt-to-steer driving game
    STOCKS  live prices + sparkline chart
    STATS   temperature, humidity, tilt, wheel angle

  Buttons:
    BTN1 short  = next view
    BTN1 long   = re-centre the steering wheel (hold ~0.7 s)
    BTN2 short  = action in the view (poke car / restart game / next ticker / re-centre)

  Libraries (Library Manager):
    Adafruit GFX Library, Adafruit SSD1306, Adafruit MPU6050,
    Adafruit Unified Sensor, DHT sensor library, ArduinoJson (version 7)
  Board: "XIAO_ESP32C3"   (Tools > USB CDC On Boot > Enabled if you want Serial output)

  Stock data comes from Yahoo's public chart endpoint. It is unofficial, can change
  or rate-limit, and is for fun only - NOT financial advice.
*/

#include <Arduino.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <Adafruit_MPU6050.h>
#include <Adafruit_Sensor.h>
#include <DHT.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <Preferences.h>

// ============================ SETTINGS (edit these) ============================
const char* WIFI_SSID = "YOUR_WIFI_NAME";       // keep real passwords OUT of GitHub!
const char* WIFI_PASS = "YOUR_WIFI_PASSWORD";

const char* TICKERS[] = { "TSLA", "RACE", "NVDA", "AAPL" };   // up to 8, 5 chars max each
const int   NUM_TICKERS = 4;

// How the board is held while steering:
//   1 = upright, like a real steering wheel (screen facing you, you rotate it)
//   0 = flat on the desk, tilt left/right
#define STEER_UPRIGHT 1

// If the wheel turns the WRONG way on screen, flip this between 1 and -1.
#define STEER_SIGN  -1
// If the wheel jitters / fights itself (gyro and accelerometer disagree), flip this.
#define GYRO_SIGN    1
// ===============================================================================

#define SDA_PIN  6
#define SCL_PIN  7
#define DHT_PIN  3
#define BTN1_PIN 4
#define BTN2_PIN 5

#define SCREEN_W 128
#define SCREEN_H 64
#define POINTS   48                 // chart points kept per ticker

Adafruit_SSD1306 display(SCREEN_W, SCREEN_H, &Wire, -1);
Adafruit_MPU6050 mpu;
DHT dht(DHT_PIN, DHT11);
Preferences prefs;

// ------------------------------- types ---------------------------------------
enum View { V_CAR = 0, V_DRIVE, V_STOCKS, V_STATS, V_COUNT };
enum NetState { NET_IDLE = 0, NET_CONNECTING, NET_FETCHING, NET_OK, NET_ERR };
enum BtnEvent { EV_NONE = 0, EV_SHORT, EV_LONG };

struct StockData {
  bool     valid;
  float    price;
  float    prevClose;
  uint8_t  n;
  float    pts[POINTS];
  uint32_t updatedAt;
};

struct Button {
  uint8_t  pin;
  bool     down;
  bool     longFired;
  uint32_t changedAt;
  uint32_t pressedAt;
};

struct Obstacle {
  float   x, y;
  uint8_t w, h;
  bool    on;
};

// ------------------------------- state ---------------------------------------
View view = V_CAR;
bool mpuOk = false;

// steering
float   biasRate   = 0;      // gyro bias (deg/s)
float   angleRaw   = 0;      // filtered board angle (deg)
float   angleZero  = 0;      // re-centre offset
float   wheelDeg   = 0;      // angle drawn on screen, clockwise positive
float   rollDeg = 0, pitchDeg = 0;
uint32_t lastImu = 0;

// environment
float tempC = NAN, hum = NAN;
uint32_t lastDht = 0;

// stocks (written by background task, read by UI)
StockData stocks[8];
portMUX_TYPE stockMux = portMUX_INITIALIZER_UNLOCKED;
volatile uint8_t netState = NET_IDLE;
int tickerIdx = 0;

// toast
char     toastText[16] = "";
uint32_t toastUntil = 0;

// talking
char     msgLines[3][21];
int      msgTotal = 0;
uint32_t msgStart = 0;
uint32_t nextAutoSay = 0;
uint8_t  sayCycle = 0;

// buttons
Button b1 = { BTN1_PIN, false, false, 0, 0 };
Button b2 = { BTN2_PIN, false, false, 0, 0 };

// game
Obstacle obs[4];
float    gameSpeed = 40, gameDist = 0, roadScroll = 0, spawnTimer = 0;
bool     gameOver = false;
int      bestScore = 0;
const int ROAD_L = 32, ROAD_R = 100, ROAD_C = 66;
const int PLAYER_Y = 46;

// ------------------------------- helpers -------------------------------------
float wrap180(float a) {
  while (a > 180.0f)  a -= 360.0f;
  while (a < -180.0f) a += 360.0f;
  return a;
}

float clampf(float v, float lo, float hi) {
  if (v < lo) return lo;
  if (v > hi) return hi;
  return v;
}

void showToast(const char* t) {
  strncpy(toastText, t, sizeof(toastText) - 1);
  toastText[sizeof(toastText) - 1] = 0;
  toastUntil = millis() + 800;
}

// ------------------------------- stocks --------------------------------------
void getStock(int i, StockData& out) {
  portENTER_CRITICAL(&stockMux);
  out = stocks[i];
  portEXIT_CRITICAL(&stockMux);
}

bool fetchStock(const char* sym, StockData& out) {
  WiFiClientSecure client;
  client.setInsecure();                       // fine for a hobby display
  HTTPClient http;
  http.setTimeout(8000);
  http.useHTTP10(true);                       // avoids chunked encoding so the JSON stream parses
  http.setUserAgent("Mozilla/5.0");

  char url[160];
  snprintf(url, sizeof(url),
           "https://query1.finance.yahoo.com/v8/finance/chart/%s?interval=15m&range=1d", sym);
  if (!http.begin(client, url)) return false;

  int code = http.GET();
  if (code != 200) { http.end(); return false; }

  JsonDocument filter;
  filter["chart"]["result"][0]["meta"]["regularMarketPrice"] = true;
  filter["chart"]["result"][0]["meta"]["chartPreviousClose"] = true;
  filter["chart"]["result"][0]["indicators"]["quote"][0]["close"] = true;

  JsonDocument doc;
  DeserializationError err =
      deserializeJson(doc, http.getStream(), DeserializationOption::Filter(filter));
  http.end();
  if (err) return false;

  JsonObject r = doc["chart"]["result"][0];
  if (r.isNull()) return false;

  float price = r["meta"]["regularMarketPrice"] | 0.0f;
  float prev  = r["meta"]["chartPreviousClose"] | 0.0f;
  if (price <= 0) return false;
  if (prev <= 0) prev = price;

  // collect valid closes (Yahoo puts null for empty intervals)
  static float tmp[120];
  int cnt = 0;
  JsonArray closes = r["indicators"]["quote"][0]["close"];
  for (JsonVariant v : closes) {
    if (v.isNull()) continue;
    if (cnt < 120) tmp[cnt++] = v.as<float>();
  }

  out.valid     = true;
  out.price     = price;
  out.prevClose = prev;
  out.updatedAt = millis();
  if (cnt <= POINTS) {
    for (int i = 0; i < cnt; i++) out.pts[i] = tmp[i];
    out.n = cnt;
  } else {                                     // down-sample to POINTS
    for (int i = 0; i < POINTS; i++) out.pts[i] = tmp[(i * (cnt - 1)) / (POINTS - 1)];
    out.n = POINTS;
  }
  if (out.n > 0) out.pts[out.n - 1] = price;   // last point = live price
  return true;
}

bool connectWifi() {
  if (WiFi.status() == WL_CONNECTED) return true;
  netState = NET_CONNECTING;
  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  uint32_t t0 = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - t0 < 15000) vTaskDelay(200 / portTICK_PERIOD_MS);
  return WiFi.status() == WL_CONNECTED;
}

// runs on its own FreeRTOS task so the screen never freezes while the network is slow
void stockTask(void*) {
  for (;;) {
    if (!connectWifi()) {
      netState = NET_ERR;
      vTaskDelay(15000 / portTICK_PERIOD_MS);
      continue;
    }
    bool anyFail = false;
    for (int i = 0; i < NUM_TICKERS; i++) {
      netState = NET_FETCHING;
      StockData tmp;
      memset(&tmp, 0, sizeof(tmp));
      if (fetchStock(TICKERS[i], tmp)) {
        portENTER_CRITICAL(&stockMux);
        stocks[i] = tmp;
        portEXIT_CRITICAL(&stockMux);
      } else {
        anyFail = true;
      }
      vTaskDelay(400 / portTICK_PERIOD_MS);
    }
    netState = anyFail ? NET_ERR : NET_OK;
    vTaskDelay((anyFail ? 15000 : 60000) / portTICK_PERIOD_MS);   // retry fast on failure
  }
}

// ------------------------------- sensors -------------------------------------
void calibrateGyro() {
  if (!mpuOk) return;
  float sum = 0;
  const int N = 150;
  for (int i = 0; i < N; i++) {
    sensors_event_t a, g, t;
    mpu.getEvent(&a, &g, &t);
#if STEER_UPRIGHT
    sum += g.gyro.z;
#else
    sum += g.gyro.x;
#endif
    delay(4);
  }
  biasRate = (sum / N) * RAD_TO_DEG;
}

void updateImu() {
  uint32_t now = micros();
  float dt = (now - lastImu) / 1000000.0f;
  lastImu = now;
  if (dt <= 0 || dt > 0.2f) dt = 0.03f;

  if (!mpuOk) {                                // no sensor: wiggle so the demo still looks alive
    wheelDeg = 35.0f * sinf(millis() / 900.0f);
    rollDeg = wheelDeg; pitchDeg = 0;
    return;
  }

  sensors_event_t a, g, t;
  mpu.getEvent(&a, &g, &t);

  float rate, accAng, inPlane;
#if STEER_UPRIGHT
  rate    = g.gyro.z * RAD_TO_DEG - biasRate;
  accAng  = atan2f(a.acceleration.x, a.acceleration.y) * RAD_TO_DEG;
  inPlane = hypotf(a.acceleration.x, a.acceleration.y);
#else
  rate    = g.gyro.x * RAD_TO_DEG - biasRate;
  accAng  = atan2f(a.acceleration.y, a.acceleration.z) * RAD_TO_DEG;
  inPlane = hypotf(a.acceleration.y, a.acceleration.z);
#endif
  float mag = sqrtf(a.acceleration.x * a.acceleration.x + a.acceleration.y * a.acceleration.y +
                    a.acceleration.z * a.acceleration.z);

  // complementary filter: trust the gyro for fast moves, the accelerometer to stop drift
  angleRaw = wrap180(angleRaw + GYRO_SIGN * rate * dt);
  bool accOk = (inPlane > 4.0f) && (fabsf(mag - 9.81f) < 2.5f);
  if (accOk) angleRaw = wrap180(angleRaw + 0.04f * wrap180(accAng - angleRaw));

  wheelDeg = wrap180(STEER_SIGN * (angleRaw - angleZero));

  rollDeg  = atan2f(a.acceleration.y, a.acceleration.z) * RAD_TO_DEG;
  pitchDeg = atan2f(-a.acceleration.x, hypotf(a.acceleration.y, a.acceleration.z)) * RAD_TO_DEG;
}

void recenter() {
  angleZero = angleRaw;
  showToast("CENTERED");
}

void updateDht() {
  if (millis() - lastDht < 2500) return;
  lastDht = millis();
  float t = dht.readTemperature();
  float h = dht.readHumidity();
  if (!isnan(t)) tempC = t;
  if (!isnan(h)) hum = h;
}

// ------------------------------- buttons -------------------------------------
BtnEvent readButton(Button& b) {
  bool raw = (digitalRead(b.pin) == LOW);
  uint32_t now = millis();
  BtnEvent ev = EV_NONE;
  if (raw != b.down && now - b.changedAt > 25) {          // debounce
    b.changedAt = now;
    b.down = raw;
    if (raw) { b.pressedAt = now; b.longFired = false; }
    else if (!b.longFired) ev = EV_SHORT;
  }
  if (b.down && !b.longFired && now - b.pressedAt > 700) {
    b.longFired = true;
    ev = EV_LONG;
  }
  return ev;
}

// ------------------------------- talking -------------------------------------
void say(const char* s) {
  memset(msgLines, 0, sizeof(msgLines));
  int line = 0, col = 0;
  const char* p = s;
  while (*p && line < 3) {
    const char* e = p;
    while (*e && *e != ' ') e++;
    int wl = e - p;
    if (col > 0 && col + 1 + wl > 20) { line++; col = 0; if (line >= 3) break; }
    if (col > 0) msgLines[line][col++] = ' ';
    for (int i = 0; i < wl && col < 20; i++) msgLines[line][col++] = p[i];
    msgLines[line][col] = 0;
    p = e;
    while (*p == ' ') p++;
  }
  msgTotal = 0;
  for (int i = 0; i < 3; i++) msgTotal += strlen(msgLines[i]);
  msgStart = millis();
  nextAutoSay = millis() + 3500 + msgTotal * 35 + 2500;
}

bool isTalking() {
  return (int)((millis() - msgStart) / 35) < msgTotal;
}

const char* QUIPS[] = {
  "Vroom! Ready when you are.",
  "Tilt me and I steer!",
  "Mind the gap. And the market.",
  "Buy the dip? Ask a human.",
  "Poke me with BTN2!",
  "I run on gyros, not gas.",
  "Hold BTN1 to re-centre me.",
  "Try DRIVE mode. I dare you."
};
const int NUM_QUIPS = 8;

void stockQuip() {
  char buf[64];
  StockData s;
  getStock(tickerIdx, s);
  if (!s.valid) {
    if (netState == NET_ERR) say("No market data. Check my WiFi?");
    else say("Fetching the market...");
    return;
  }
  float pct = (s.price - s.prevClose) / s.prevClose * 100.0f;
  if (pct > 1.0f)       snprintf(buf, sizeof(buf), "%s is up %.1f%%! Floor it!", TICKERS[tickerIdx], pct);
  else if (pct < -1.0f) snprintf(buf, sizeof(buf), "%s down %.1f%%. Brake!", TICKERS[tickerIdx], -pct);
  else                  snprintf(buf, sizeof(buf), "%s flat at %.2f. Cruising.", TICKERS[tickerIdx], s.price);
  say(buf);
}

void autoSay() {
  char buf[64];
  switch (sayCycle++ % 4) {
    case 0: stockQuip(); break;
    case 1:
      if (!isnan(tempC)) { snprintf(buf, sizeof(buf), "It is %.0f C in here. %.0f%% humid.", tempC, hum); say(buf); }
      else say(QUIPS[random(NUM_QUIPS)]);
      break;
    case 2:
      if (fabsf(wheelDeg) > 25) say(wheelDeg > 0 ? "Whoa, turning right!" : "Whoa, turning left!");
      else say(QUIPS[random(NUM_QUIPS)]);
      break;
    default: say(QUIPS[random(NUM_QUIPS)]); break;
  }
}

// ------------------------------- drawing -------------------------------------
void drawWheel(int cx, int cy, int r, float deg) {
  float a = deg * DEG_TO_RAD;                         // positive = clockwise on screen
  display.drawCircle(cx, cy, r, SSD1306_WHITE);
  if (r > 8)  display.drawCircle(cx, cy, r - 1, SSD1306_WHITE);
  if (r > 16) display.drawCircle(cx, cy, r - 2, SSD1306_WHITE);
  const float spokes[3] = { 0.0f, 1.5708f, 3.1416f };  // right, bottom, left  (T-shape at 0 deg)
  for (int i = 0; i < 3; i++) {
    int x = cx + (int)roundf(cosf(a + spokes[i]) * (r - 2));
    int y = cy + (int)roundf(sinf(a + spokes[i]) * (r - 2));
    display.drawLine(cx, cy, x, y, SSD1306_WHITE);
  }
  int hub = r / 4; if (hub < 2) hub = 2;
  display.fillCircle(cx, cy, hub, SSD1306_WHITE);
  // 12 o'clock marker so you can see the rotation
  int mx = cx + (int)roundf(cosf(a - 1.5708f) * r);
  int my = cy + (int)roundf(sinf(a - 1.5708f) * r);
  display.fillCircle(mx, my, r > 12 ? 2 : 1, SSD1306_WHITE);
}

void drawCarFace() {
  // cabin + body
  display.drawRoundRect(18, 33, 48, 13, 4, SSD1306_WHITE);
  display.drawRoundRect(6, 44, 72, 18, 5, SSD1306_WHITE);
  // wheels
  display.fillRoundRect(10, 60, 14, 4, 1, SSD1306_WHITE);
  display.fillRoundRect(60, 60, 14, 4, 1, SSD1306_WHITE);

  // headlight eyes, pupils follow the steering
  int px = (int)clampf(wheelDeg / 15.0f, -3, 3);
  bool blink = (millis() % 3600) < 130;
  const int ex[2] = { 24, 60 };
  for (int i = 0; i < 2; i++) {
    display.drawCircle(ex[i], 51, 6, SSD1306_WHITE);
    if (blink) display.drawFastHLine(ex[i] - 4, 51, 9, SSD1306_WHITE);
    else       display.fillCircle(ex[i] + px, 51, 2, SSD1306_WHITE);
  }

  // mouth: flaps while talking, small smile when idle
  if (isTalking()) {
    int open = 1 + (millis() / 90) % 4;
    display.fillRoundRect(34, 54, 16, open + 1, 1, SSD1306_WHITE);
  } else {
    display.drawFastHLine(36, 56, 12, SSD1306_WHITE);
    display.drawPixel(35, 55, SSD1306_WHITE);
    display.drawPixel(48, 55, SSD1306_WHITE);
  }
}

void drawBubble() {
  display.drawRoundRect(0, 0, 128, 31, 4, SSD1306_WHITE);
  display.fillTriangle(44, 30, 56, 30, 48, 36, SSD1306_BLACK);      // tail
  display.drawLine(44, 30, 48, 36, SSD1306_WHITE);
  display.drawLine(56, 30, 48, 36, SSD1306_WHITE);

  int remaining = (int)((millis() - msgStart) / 35);
  display.setTextSize(1);
  for (int i = 0; i < 3; i++) {
    int len = strlen(msgLines[i]);
    if (len == 0) continue;
    int n = remaining < len ? remaining : len;
    if (n > 0) {
      char tmp[21];
      memcpy(tmp, msgLines[i], n);
      tmp[n] = 0;
      display.setCursor(4, 3 + i * 9);
      display.print(tmp);
    }
    remaining -= len;
    if (remaining <= 0) break;
  }
}

void drawCarView() {
  drawBubble();
  drawCarFace();
  drawWheel(106, 48, 14, wheelDeg);
}

// ---- game ----
void gameReset() {
  for (int i = 0; i < 4; i++) obs[i].on = false;
  gameSpeed = 45; gameDist = 0; roadScroll = 0; spawnTimer = 0.6f;
  gameOver = false;
}

void gameUpdate(float dt) {
  if (gameOver) return;
  gameSpeed = 45.0f + gameDist * 0.12f;
  if (gameSpeed > 120) gameSpeed = 120;
  gameDist   += gameSpeed * dt * 0.2f;
  roadScroll += gameSpeed * dt;

  spawnTimer -= dt;
  if (spawnTimer <= 0) {
    for (int i = 0; i < 4; i++) {
      if (!obs[i].on) {
        obs[i].w = 10 + random(8);
        obs[i].h = 12;
        obs[i].x = ROAD_L + 3 + random(ROAD_R - ROAD_L - 6 - obs[i].w);
        obs[i].y = -12;
        obs[i].on = true;
        break;
      }
    }
    spawnTimer = 0.45f + random(40) / 100.0f - gameSpeed / 400.0f;
    if (spawnTimer < 0.25f) spawnTimer = 0.25f;
  }

  float px = ROAD_C + clampf(wheelDeg, -45, 45) / 45.0f * 28.0f;
  float pl = px - 4, pr = px + 4;
  for (int i = 0; i < 4; i++) {
    if (!obs[i].on) continue;
    obs[i].y += gameSpeed * dt;
    if (obs[i].y > SCREEN_H) { obs[i].on = false; continue; }
    float ol = obs[i].x, orr = obs[i].x + obs[i].w;
    if (pr > ol && pl < orr && PLAYER_Y + 13 > obs[i].y && PLAYER_Y < obs[i].y + obs[i].h) {
      gameOver = true;
      if ((int)gameDist > bestScore) {
        bestScore = (int)gameDist;
        prefs.putInt("best", bestScore);
      }
    }
  }
}

void drawDriveView() {
  // road edges + scrolling centre dashes
  display.drawFastVLine(ROAD_L, 0, SCREEN_H, SSD1306_WHITE);
  display.drawFastVLine(ROAD_R, 0, SCREEN_H, SSD1306_WHITE);
  int off = (int)roadScroll % 16;
  for (int y = -16 + off; y < SCREEN_H; y += 16) display.drawFastVLine(ROAD_C, y, 8, SSD1306_WHITE);

  for (int i = 0; i < 4; i++) {
    if (!obs[i].on) continue;
    int x = (int)obs[i].x, y = (int)obs[i].y;
    display.drawRect(x, y, obs[i].w, obs[i].h, SSD1306_WHITE);
    display.fillRect(x + 2, y + obs[i].h - 5, obs[i].w - 4, 2, SSD1306_WHITE);
  }

  int px = ROAD_C + (int)(clampf(wheelDeg, -45, 45) / 45.0f * 28.0f);
  display.fillRoundRect(px - 4, PLAYER_Y, 9, 14, 2, SSD1306_WHITE);
  display.fillRect(px - 2, PLAYER_Y + 3, 5, 3, SSD1306_BLACK);

  // HUD
  display.setTextSize(1);
  display.setCursor(0, 0);  display.print("SCORE");
  display.setCursor(0, 9);  display.print((int)gameDist);
  display.setCursor(0, 24); display.print("BEST");
  display.setCursor(0, 33); display.print(bestScore);
  drawWheel(116, 52, 9, wheelDeg);

  if (gameOver) {
    display.fillRect(28, 18, 72, 28, SSD1306_BLACK);
    display.drawRect(28, 18, 72, 28, SSD1306_WHITE);
    display.setCursor(48, 23); display.print("CRASH!");
    display.setCursor(33, 35); display.print("BTN2: retry");
  }
}

// ---- stocks ----
void drawArrow(int x, int y, bool up) {
  if (up) display.fillTriangle(x, y + 12, x + 12, y + 12, x + 6, y, SSD1306_WHITE);
  else    display.fillTriangle(x, y, x + 12, y, x + 6, y + 12, SSD1306_WHITE);
}

void drawSparkline(const StockData& s, int x0, int y0, int w, int h) {
  if (s.n < 2) return;
  float mn = s.prevClose, mx = s.prevClose;
  for (int i = 0; i < s.n; i++) {
    if (s.pts[i] < mn) mn = s.pts[i];
    if (s.pts[i] > mx) mx = s.pts[i];
  }
  float minRange = s.prevClose * 0.006f;               // keep flat stocks looking calm
  if (mx - mn < minRange) {
    float mid = (mx + mn) / 2;
    mx = mid + minRange / 2; mn = mid - minRange / 2;
  }
  auto ypos = [&](float v) { return y0 + h - 1 - (int)((v - mn) / (mx - mn) * (h - 1)); };

  int yp = ypos(s.prevClose);                           // dotted previous-close line
  for (int x = x0; x < x0 + w; x += 4) display.drawPixel(x, yp, SSD1306_WHITE);

  int lx = x0, ly = ypos(s.pts[0]);
  for (int i = 1; i < s.n; i++) {
    int x = x0 + (i * (w - 1)) / (s.n - 1);
    int y = ypos(s.pts[i]);
    display.drawLine(lx, ly, x, y, SSD1306_WHITE);
    lx = x; ly = y;
  }
  display.fillCircle(lx, ly, 1, SSD1306_WHITE);
}

void drawStocksView() {
  StockData s;
  getStock(tickerIdx, s);

  display.setTextSize(1);
  display.setCursor(0, 0);
  display.print(TICKERS[tickerIdx]);
  display.print(" ");
  display.print(tickerIdx + 1);
  display.print("/");
  display.print(NUM_TICKERS);

  // status, top right
  char st[16];
  if (s.valid && netState != NET_CONNECTING) {
    uint32_t age = (millis() - s.updatedAt) / 1000;
    if (age < 100) snprintf(st, sizeof(st), "%s%us", netState == NET_FETCHING ? "~" : "", (unsigned)age);
    else           snprintf(st, sizeof(st), "%um", (unsigned)(age / 60));
  } else {
    strcpy(st, netState == NET_ERR ? "NO NET" : (netState == NET_CONNECTING ? "WIFI.." : "LOAD.."));
  }
  display.setCursor(128 - strlen(st) * 6, 0);
  display.print(st);

  if (!s.valid) {
    display.setCursor(10, 28);
    display.print(netState == NET_ERR ? "Check WiFi / SSID" : "Getting prices...");
    return;
  }

  float chg = s.price - s.prevClose;
  float pct = chg / s.prevClose * 100.0f;
  bool up = chg >= 0;

  char buf[20];
  if (s.price >= 1000) snprintf(buf, sizeof(buf), "%.1f", s.price);
  else                 snprintf(buf, sizeof(buf), "%.2f", s.price);
  display.setTextSize(2);
  display.setCursor(0, 10);
  display.print(buf);

  drawArrow(108, 11, up);

  display.setTextSize(1);
  snprintf(buf, sizeof(buf), "%+.2f  %+.2f%%", chg, pct);
  display.setCursor(0, 28);
  display.print(buf);

  drawSparkline(s, 0, 40, 128, 24);
}

// ---- stats ----
void drawStatsView() {
  char buf[24];
  display.setTextSize(1);
  display.setCursor(0, 0);  display.print("STATS");

  display.setCursor(0, 12);
  if (isnan(tempC)) display.print("Temp: --");
  else { snprintf(buf, sizeof(buf), "Temp: %.1f C", tempC); display.print(buf); }
  display.setCursor(0, 22);
  if (isnan(hum)) display.print("Hum:  --");
  else { snprintf(buf, sizeof(buf), "Hum:  %.0f %%", hum); display.print(buf); }

  snprintf(buf, sizeof(buf), "R%+.0f P%+.0f", rollDeg, pitchDeg);
  display.setCursor(0, 32); display.print(buf);
  snprintf(buf, sizeof(buf), "Wheel: %+.0f deg", wheelDeg);
  display.setCursor(0, 42); display.print(buf);

  // steering bar
  display.drawRect(0, 54, 128, 9, SSD1306_WHITE);
  display.drawFastVLine(64, 54, 9, SSD1306_WHITE);
  int bx = 64 + (int)(clampf(wheelDeg, -90, 90) / 90.0f * 60.0f);
  display.fillRect(bx - 2, 56, 5, 5, SSD1306_WHITE);

  drawWheel(108, 22, 14, wheelDeg);
}

void drawToast() {
  if (millis() > toastUntil) return;
  int w = strlen(toastText) * 6 + 8;
  int x = (128 - w) / 2;
  display.fillRect(x, 24, w, 14, SSD1306_WHITE);
  display.setTextColor(SSD1306_BLACK);
  display.setCursor(x + 4, 27);
  display.print(toastText);
  display.setTextColor(SSD1306_WHITE);
}

// ------------------------------- views / input --------------------------------
void setView(View v) {
  view = v;
  static const char* names[] = { "CAR", "DRIVE", "STOCKS", "STATS" };
  showToast(names[v]);
  if (v == V_DRIVE) gameReset();
  if (v == V_CAR) say("Hi! I'm Starbie.");
}

void handleButtons() {
  BtnEvent e1 = readButton(b1);
  BtnEvent e2 = readButton(b2);

  if (e1 == EV_SHORT) setView((View)((view + 1) % V_COUNT));
  if (e1 == EV_LONG)  recenter();

  if (e2 == EV_SHORT) {
    switch (view) {
      case V_CAR:
        if (random(2)) stockQuip(); else say(QUIPS[random(NUM_QUIPS)]);
        break;
      case V_DRIVE:
        if (gameOver) gameReset();
        break;
      case V_STOCKS:
        tickerIdx = (tickerIdx + 1) % NUM_TICKERS;
        break;
      case V_STATS:
        recenter();
        break;
      default: break;
    }
  }
  if (e2 == EV_LONG && view == V_STOCKS) {
    tickerIdx = (tickerIdx + NUM_TICKERS - 1) % NUM_TICKERS;
  }
}

// ------------------------------- setup / loop ---------------------------------
void setup() {
  Serial.begin(115200);
  pinMode(BTN1_PIN, INPUT_PULLUP);
  pinMode(BTN2_PIN, INPUT_PULLUP);

  Wire.begin(SDA_PIN, SCL_PIN);
  Wire.setClock(400000);                               // fast I2C = smoother animation

  if (!display.begin(SSD1306_SWITCHCAPVCC, 0x3C)) {
    Serial.println("OLED not found - check SDA/SCL and address");
    for (;;) delay(1000);
  }
  display.setTextWrap(false);
  display.setTextColor(SSD1306_WHITE);
  display.clearDisplay();
  display.setTextSize(1);
  display.setCursor(18, 20); display.print("Starbie waking up");
  display.setCursor(18, 34); display.print("Keep me still...");
  display.display();

  mpuOk = mpu.begin(0x68, &Wire);
  if (mpuOk) {
    mpu.setAccelerometerRange(MPU6050_RANGE_4_G);
    mpu.setGyroRange(MPU6050_RANGE_500_DEG);
    mpu.setFilterBandwidth(MPU6050_BAND_44_HZ);
    calibrateGyro();
  } else {
    Serial.println("MPU6050 not found - steering demo mode");
  }

  dht.begin();
  prefs.begin("starbie", false);
  bestScore = prefs.getInt("best", 0);

  randomSeed(micros());
  lastImu = micros();

  xTaskCreate(stockTask, "stocks", 12288, nullptr, 1, nullptr);

  gameReset();
  say("Hi! I'm Starbie.");
  showToast("CAR");
}

void loop() {
  static uint32_t lastFrame = 0;
  uint32_t now = millis();
  if (now - lastFrame < 28) return;                    // ~30 fps cap
  float dt = (now - lastFrame) / 1000.0f;
  lastFrame = now;
  if (dt > 0.2f) dt = 0.03f;

  updateImu();
  updateDht();
  handleButtons();

  if (view == V_CAR && now > nextAutoSay && !isTalking()) autoSay();
  if (view == V_DRIVE) gameUpdate(dt);

  display.clearDisplay();
  switch (view) {
    case V_CAR:    drawCarView();    break;
    case V_DRIVE:  drawDriveView();  break;
    case V_STOCKS: drawStocksView(); break;
    case V_STATS:  drawStatsView();  break;
    default: break;
  }
  drawToast();
  display.display();
}
