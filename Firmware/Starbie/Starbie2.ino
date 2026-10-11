/*
  Starbie Stocks: Starbie, but it watches the market for you. (File: Starbie.ino)

  This is the original Starbie firmware (pet, radial menu, stats, shake) with
  one new screen: a live stock ticker with price, daily change and a
  sparkline of today's price action. Prices come over WiFi from Yahoo Finance.

  Install these libraries with Sketch > Include Library > Manage Libraries:
    - Adafruit GFX Library
    - Adafruit SSD1306
    - Adafruit MPU6050
    - DHT sensor library
    - ArduinoJson (by Benoit Blanchon, version 7.x)   <-- new
  Board: esp32 by Espressif Systems > XIAO_ESP32C3

  Controls
    Button 2: cycle screens  STOCKS -> PET -> STATS -> STOCKS
    Button 1: on the STOCKS screen, jump to the next ticker.
              On the PET / STATS screens, open the radial menu (as before).
    Shake:    still makes the pet happy (PET screen only).
*/

#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <Adafruit_MPU6050.h>
#include <DHT.h>
#include <Preferences.h>
#include <math.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>

// =========================== BEGINNER SETTINGS ===========================
// Everything most builders will want to customize is in this one section.

// --- WiFi and stocks (NEW) ------------------------------------------------
// The ESP32-C3 only supports 2.4 GHz WiFi.
const char *WIFI_SSID = "YOUR_WIFI_NAME";
const char *WIFI_PASSWORD = "YOUR_WIFI_PASSWORD";

// Any Yahoo Finance symbol works. Examples:
//   Indexes: "^GSPC" (S&P 500), "^IXIC" (Nasdaq), "^NSEI" (Nifty 50)
//   Crypto:  "BTC-USD", "ETH-USD"
//   Other exchanges: "RELIANCE.NS" (NSE India), "SHOP.TO" (Toronto)
const char *TICKERS[] = {"AAPL", "MSFT", "NVDA", "^GSPC"};

// How often to refresh every ticker (milliseconds). Be kind to Yahoo:
// once a minute is plenty.
const uint32_t STOCK_REFRESH_MS = 60000;

// On the STOCKS screen, move to the next ticker by itself every N ms.
// Set to 0 to only change tickers with Button 1.
const uint32_t AUTO_ROTATE_MS = 8000;

// Set to false to boot into the pet screen instead of the stock screen.
const bool START_ON_STOCKS = true;

// --- Your board's pins ----------------------------------------------------
// These are ESP32-C3 GPIO numbers, with the XIAO pin labels beside them.
const int I2C_SDA_PIN = 6;       // XIAO D4: OLED + MPU6050 SDA
const int I2C_SCL_PIN = 7;       // XIAO D5: OLED + MPU6050 SCL
const int DHT_PIN = 3;           // XIAO D1: DHT11 data
const int BUTTON_ONE_PIN = 4;    // XIAO D2: opens/confirms the radial menu
const int BUTTON_TWO_PIN = 5;    // XIAO D3: cycles screens

// Set this to false if your submission does not have a DHT11.
const bool USE_DHT11 = true;

// Most parts use these addresses. Only change them if an I2C scanner says
// your part is different (common OLED alternative: 0x3D).
const uint8_t OLED_ADDRESS = 0x3C;
const uint8_t MPU6050_ADDRESS = 0x68;

// --- Starting stats -------------------------------------------------------
// Stats are 0 to 100. They only change when you select an action or shake
// Starbie; nothing slowly drains while it sits on your desk.
const int STARTING_JOY = 70;
const int STARTING_ENERGY = 75;
const int STARTING_FULLNESS = 65;

// Set true, upload once, then set it back to false if you want a fresh pet.
const bool RESET_SAVED_PET_ON_BOOT = false;

// --- Radial menu ----------------------------------------------------------
// These four actions appear at TOP, RIGHT, BOTTOM, then LEFT in the menu.
// The last word controls the matching little visual reaction.
enum PetReaction {
  NAP_REACTION,
  JUMP_REACTION,
  HEART_REACTION,
  RUN_REACTION,
};

struct MenuItem {
  const char *label;
  int joyChange;
  int energyChange;
  int fullnessChange;
  PetReaction reaction;
};

const MenuItem MENU_ITEMS[] = {
  {"NAP",   1,  18, -4, NAP_REACTION},   // top: sleep and emit Zs
  {"PLAY", 12, -9, -5, RUN_REACTION},    // right: two fast laps + hearts
  {"FEED",  3,  2,  18, JUMP_REACTION},  // bottom: wiggle and jump
  {"PET",   7,  0,  0, HEART_REACTION},  // left: jump and emit hearts
};
const int MENU_ITEM_COUNT = sizeof(MENU_ITEMS) / sizeof(MENU_ITEMS[0]);

// --- Movement feel --------------------------------------------------------
// Bigger MENU_TILT_LIMIT makes the menu ball move less.
const float MENU_TILT_LIMIT = 6.0f;

// MPU6050 orientation: change these settings instead of rewiring your board.
// 1. If the sensor is turned 90 degrees, set SWAP_MPU_AXES to true.
// 2. If a direction feels backwards, change that direction from 1.0f to -1.0f.
const bool SWAP_MPU_AXES = false;
const float MENU_X_DIRECTION = 1.0f;
const float MENU_Y_DIRECTION = -1.0f;

// A small dead zone makes the ball rest in the center until the board is tilted.
const float MENU_CENTER_DEADZONE = 0.8f;
const float SHAKE_THRESHOLD = 7.0f;

// Shake is the only movement that has an effect outside the radial menu.
const int SHAKE_JOY_CHANGE = 5;
const int SHAKE_ENERGY_CHANGE = -2;
const int SHAKE_FULLNESS_CHANGE = -1;

// --- Your pet's bitmap and animation -------------------------------------
// This is a simplified 32 by 32, one-bit version of the supplied character.
// You can replace it with your own art later; keep these size values matched
// to the sprite array you paste in.
const int PET_SPRITE_WIDTH = 32;
const int PET_SPRITE_HEIGHT = 32;

// The pet wanders unless it is asleep. A non-nap action does a tiny shake,
// then a jump, except PLAY which runs two fast laps.
const uint16_t PET_WALK_PIXEL_MS = 70;
const uint16_t PET_PRE_JUMP_MS = 230;
const uint16_t PET_JUMP_MS = 430;
const int PET_JUMP_HEIGHT = 16;
const uint32_t NAP_DURATION_MS = 48000;  // 48 seconds: four times the old nap.
const uint16_t HEARTS_DURATION_MS = 1600;
const uint16_t PLAY_LAP_MS = 800;
const uint8_t PLAY_LAP_COUNT = 2;

// Each byte stores eight pixels, left to right. This sparse outline keeps the
// supplied creature readable on a tiny, one-bit OLED: head, eye, legs, tail,
// and a small flower-like ear accent all stay separate instead of becoming a blob.
const uint8_t PROGMEM PET_SPRITE[] = {
  0x00, 0x00, 0x00, 0x00,
  0x00, 0x00, 0x00, 0x00,
  0x00, 0x00, 0x00, 0x00,
  0x00, 0x00, 0x00, 0x00,
  0x00, 0x00, 0x00, 0x00,
  0x00, 0x00, 0x00, 0x00,
  0x00, 0x70, 0x0a, 0x00,
  0x00, 0xf8, 0x1f, 0x00,
  0x00, 0x8c, 0x3f, 0x80,
  0x00, 0x43, 0xff, 0x00,
  0x00, 0x43, 0xff, 0x00,
  0x00, 0x30, 0x7e, 0x00,
  0x00, 0x98, 0x01, 0x00,
  0x01, 0x98, 0x01, 0x80,
  0x03, 0x00, 0x10, 0xc0,
  0x03, 0x04, 0x00, 0xc0,
  0x01, 0x8b, 0x01, 0x80,
  0x01, 0xcb, 0x03, 0x80,
  0x03, 0xc0, 0x03, 0xc0,
  0x03, 0xc0, 0x03, 0xc0,
  0x01, 0xc0, 0x03, 0x80,
  0x01, 0x80, 0x01, 0x80,
  0x00, 0x60, 0x06, 0x00,
  0x00, 0x3f, 0xfc, 0x00,
  0x00, 0x7f, 0xfe, 0x00,
  0x00, 0x70, 0x0e, 0x00,
  0x00, 0x00, 0x00, 0x00,
  0x00, 0x00, 0x00, 0x00,
  0x00, 0x00, 0x00, 0x00,
  0x00, 0x00, 0x00, 0x00,
  0x00, 0x00, 0x00, 0x00,
  0x00, 0x00, 0x00, 0x00,
};
// ===========================================================================
// You can read below without needing to understand every line. The rest of
// the sketch handles buttons, sensors, drawing, WiFi and saving automatically.

const int SCREEN_WIDTH = 128;
const int SCREEN_HEIGHT = 64;
const int MENU_CENTER_X = 64;
const int MENU_CENTER_Y = 32;
const int MENU_ITEM_X[] = {64, 108, 64, 20};
const int MENU_ITEM_Y[] = {12, 32, 53, 32};
const int MENU_BALL_X_RANGE = 37;
const int MENU_BALL_Y_RANGE = 22;
const uint16_t BUTTON_DEBOUNCE_MS = 30;
const uint16_t MPU_READ_INTERVAL_MS = 30;
const uint16_t DHT_READ_INTERVAL_MS = 2200;
const uint16_t SHAKE_COOLDOWN_MS = 650;
const float STANDARD_GRAVITY = 9.80665f;

Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, -1);
Adafruit_MPU6050 mpu;
DHT dht(DHT_PIN, DHT11);
Preferences preferences;

struct PetState {
  int joy;
  int energy;
  int fullness;
};

struct ButtonState {
  int pin;
  bool stableState;
  bool lastRawState;
  uint32_t lastChangedAt;
};

enum View {
  PET_VIEW,
  MENU_VIEW,
  STATS_VIEW,
  STOCKS_VIEW,
};

PetState pet = {STARTING_JOY, STARTING_ENERGY, STARTING_FULLNESS};
ButtonState buttonOne = {BUTTON_ONE_PIN, HIGH, HIGH, 0};
ButtonState buttonTwo = {BUTTON_TWO_PIN, HIGH, HIGH, 0};
View currentView = PET_VIEW;

bool mpuFound = false;
bool dhtFound = false;
float accelerationX = 0.0f;
float accelerationY = 0.0f;
float accelerationZ = STANDARD_GRAVITY;
float temperatureC = NAN;
float humidity = NAN;
float menuBallX = MENU_CENTER_X;
float menuBallY = MENU_CENTER_Y;
float menuCenterAccelerationX = 0.0f;
float menuCenterAccelerationY = 0.0f;
int selectedMenuItem = -1;
uint32_t lastMpuReadAt = 0;
uint32_t lastDhtReadAt = 0;
uint32_t lastShakeAt = 0;
uint32_t shakeAnimationEndsAt = 0;
uint32_t petJumpStartedAt = 0;
uint32_t nappingUntil = 0;
uint32_t heartAnimationEndsAt = 0;
uint32_t playRunStartedAt = 0;
int nappingPetX = 0;

// ------------------------------- Stock data --------------------------------
const int TICKER_COUNT = sizeof(TICKERS) / sizeof(TICKERS[0]);
const int MAX_SPARK_POINTS = 64;   // Points kept per ticker for the sparkline.
const int MAX_RAW_POINTS = 256;    // Most candles we will read from one reply.

struct StockData {
  bool valid;
  float price;
  float previousClose;
  int pointCount;
  float points[MAX_SPARK_POINTS];
};

enum NetStatus {
  NET_CONNECTING,
  NET_ONLINE,
  NET_ERROR,
};

// The WiFi work runs in its own background task so the pet and menu never
// freeze while a download is in progress. This lock keeps the two tasks from
// reading and writing the same numbers at the same instant.
portMUX_TYPE stockLock = portMUX_INITIALIZER_UNLOCKED;
StockData stocks[TICKER_COUNT];
volatile NetStatus netStatus = NET_CONNECTING;
int currentTicker = 0;
uint32_t lastTickerChangeAt = 0;

int petWalkingX(uint32_t now);

int keepInRange(int value, int smallest, int largest) {
  return constrain(value, smallest, largest);
}

void changePet(int joyChange, int energyChange, int fullnessChange) {
  pet.joy = keepInRange(pet.joy + joyChange, 0, 100);
  pet.energy = keepInRange(pet.energy + energyChange, 0, 100);
  pet.fullness = keepInRange(pet.fullness + fullnessChange, 0, 100);
}

void loadPet() {
  preferences.begin("starbie", false);
  if (RESET_SAVED_PET_ON_BOOT) {
    preferences.clear();
  }
  pet.joy = preferences.getInt("joy", STARTING_JOY);
  pet.energy = preferences.getInt("energy", STARTING_ENERGY);
  pet.fullness = preferences.getInt("full", STARTING_FULLNESS);
}

void savePet() {
  preferences.putInt("joy", pet.joy);
  preferences.putInt("energy", pet.energy);
  preferences.putInt("full", pet.fullness);
}

void setUpButton(ButtonState &button) {
  pinMode(button.pin, INPUT_PULLUP);
  button.stableState = digitalRead(button.pin);
  button.lastRawState = button.stableState;
  button.lastChangedAt = millis();
}

// Returns true exactly once for each press, even if the switch bounces.
bool wasPressed(ButtonState &button) {
  const bool rawState = digitalRead(button.pin);
  const uint32_t now = millis();

  if (rawState != button.lastRawState) {
    button.lastRawState = rawState;
    button.lastChangedAt = now;
  }

  if (rawState != button.stableState &&
      now - button.lastChangedAt >= BUTTON_DEBOUNCE_MS) {
    button.stableState = rawState;
    return button.stableState == LOW;
  }

  return false;
}

void updateMpu() {
  if (!mpuFound || millis() - lastMpuReadAt < MPU_READ_INTERVAL_MS) {
    return;
  }
  lastMpuReadAt = millis();

  sensors_event_t acceleration;
  sensors_event_t gyro;
  sensors_event_t sensorTemperature;
  mpu.getEvent(&acceleration, &gyro, &sensorTemperature);
  accelerationX = acceleration.acceleration.x;
  accelerationY = acceleration.acceleration.y;
  accelerationZ = acceleration.acceleration.z;
}

void updateDht() {
  if (!dhtFound || millis() - lastDhtReadAt < DHT_READ_INTERVAL_MS) {
    return;
  }
  lastDhtReadAt = millis();

  const float newHumidity = dht.readHumidity();
  const float newTemperatureC = dht.readTemperature();
  if (!isnan(newHumidity)) {
    humidity = newHumidity;
  }
  if (!isnan(newTemperatureC)) {
    temperatureC = newTemperatureC;
  }
}

// ------------------------- Stock downloading (NEW) -------------------------
// Asks Yahoo Finance for today's 15-minute candles for one symbol and fills
// in `result`. Returns false if anything goes wrong.
bool fetchStock(const char *symbol, StockData &result) {
  String encodedSymbol = symbol;
  encodedSymbol.replace("^", "%5E");  // Index symbols start with ^.
  const String url =
      String("https://query1.finance.yahoo.com/v8/finance/chart/") +
      encodedSymbol + "?interval=15m&range=1d";

  WiFiClientSecure client;
  client.setInsecure();  // Skips certificate checking: fine for public prices.

  HTTPClient http;
  http.setTimeout(8000);
  http.setUserAgent("Mozilla/5.0");
  if (!http.begin(client, url)) {
    return false;
  }
  const int httpCode = http.GET();
  if (httpCode != HTTP_CODE_OK) {
    Serial.printf("%s: HTTP %d\n", symbol, httpCode);
    http.end();
    return false;
  }
  const String body = http.getString();
  http.end();

  // Only keep the few fields we need so the JSON fits easily in memory.
  JsonDocument filter;
  filter["chart"]["result"][0]["meta"]["regularMarketPrice"] = true;
  filter["chart"]["result"][0]["meta"]["chartPreviousClose"] = true;
  filter["chart"]["result"][0]["meta"]["previousClose"] = true;
  filter["chart"]["result"][0]["indicators"]["quote"][0]["close"] = true;

  JsonDocument doc;
  const DeserializationError error =
      deserializeJson(doc, body, DeserializationOption::Filter(filter));
  if (error) {
    Serial.printf("%s: JSON error %s\n", symbol, error.c_str());
    return false;
  }

  JsonObject meta = doc["chart"]["result"][0]["meta"];
  if (meta.isNull() || meta["regularMarketPrice"].isNull()) {
    return false;
  }
  const float price = meta["regularMarketPrice"];
  float previousClose = meta["chartPreviousClose"] | 0.0f;
  if (previousClose <= 0.0f) {
    previousClose = meta["previousClose"] | 0.0f;
  }
  if (previousClose <= 0.0f) {
    previousClose = price;
  }

  // Collect today's closing prices (Yahoo uses null for empty candles).
  static float raw[MAX_RAW_POINTS];
  int rawCount = 0;
  JsonArray closes = doc["chart"]["result"][0]["indicators"]["quote"][0]["close"];
  for (JsonVariant value : closes) {
    if (!value.isNull() && rawCount < MAX_RAW_POINTS - 1) {
      raw[rawCount++] = value.as<float>();
    }
  }
  raw[rawCount++] = price;  // The newest point is the live price.

  // Shrink to at most MAX_SPARK_POINTS so it fits the screen.
  result.pointCount = 0;
  if (rawCount <= MAX_SPARK_POINTS) {
    for (int i = 0; i < rawCount; i++) {
      result.points[result.pointCount++] = raw[i];
    }
  } else {
    for (int i = 0; i < MAX_SPARK_POINTS; i++) {
      const int source = (i * (rawCount - 1)) / (MAX_SPARK_POINTS - 1);
      result.points[result.pointCount++] = raw[source];
    }
  }
  result.price = price;
  result.previousClose = previousClose;
  result.valid = true;
  return true;
}

bool connectToWifi() {
  if (WiFi.status() == WL_CONNECTED) {
    return true;
  }
  netStatus = NET_CONNECTING;
  WiFi.disconnect();
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  for (int attempt = 0; attempt < 40; attempt++) {  // Up to about 20 seconds.
    if (WiFi.status() == WL_CONNECTED) {
      return true;
    }
    vTaskDelay(500 / portTICK_PERIOD_MS);
  }
  return false;
}

// Runs forever in the background: connect, refresh every ticker, then sleep.
void stockTask(void *unused) {
  for (;;) {
    if (!connectToWifi()) {
      netStatus = NET_ERROR;
      vTaskDelay(10000 / portTICK_PERIOD_MS);
      continue;
    }

    bool anySuccess = false;
    for (int i = 0; i < TICKER_COUNT; i++) {
      StockData fresh;
      memset(&fresh, 0, sizeof(fresh));
      if (fetchStock(TICKERS[i], fresh)) {
        portENTER_CRITICAL(&stockLock);
        stocks[i] = fresh;
        portEXIT_CRITICAL(&stockLock);
        anySuccess = true;
      }
      vTaskDelay(300 / portTICK_PERIOD_MS);
    }
    netStatus = anySuccess ? NET_ONLINE : NET_ERROR;
    vTaskDelay(STOCK_REFRESH_MS / portTICK_PERIOD_MS);
  }
}

void nextTicker() {
  currentTicker = (currentTicker + 1) % TICKER_COUNT;
  lastTickerChangeAt = millis();
}

// ------------------------------ Pet behaviour ------------------------------
int petWalkingX(uint32_t now) {
  const int farthestX = SCREEN_WIDTH - PET_SPRITE_WIDTH;
  const uint32_t roundTrip = static_cast<uint32_t>(farthestX) * 2;
  const uint32_t step = (now / PET_WALK_PIXEL_MS) % roundTrip;
  return step <= farthestX ? step : roundTrip - step;
}

uint32_t playRunDuration() {
  return static_cast<uint32_t>(PLAY_LAP_MS) * PLAY_LAP_COUNT;
}

bool isNapping() {
  return nappingUntil != 0 && millis() < nappingUntil;
}

bool isPlaying() {
  return playRunStartedAt != 0 && millis() - playRunStartedAt < playRunDuration();
}

int playRunX(uint32_t now) {
  const int farthestX = SCREEN_WIDTH - PET_SPRITE_WIDTH;
  const uint32_t lapAge = (now - playRunStartedAt) % PLAY_LAP_MS;
  const float progress = static_cast<float>(lapAge) / PLAY_LAP_MS;
  return progress < 0.5f ? static_cast<int>(progress * 2.0f * farthestX)
                         : static_cast<int>((1.0f - progress) * 2.0f * farthestX);
}

void updatePetTimers() {
  if (nappingUntil != 0 && !isNapping()) {
    nappingUntil = 0;  // The pet wakes naturally when its nap time is over.
  }
  if (playRunStartedAt != 0 && !isPlaying()) {
    playRunStartedAt = 0;  // PLAY is exactly two left-to-right-to-left laps.
  }
}

// While the menu is open, the MPU6050 acts like a tiny air mouse. The position
// at the instant Button 1 opens the menu becomes the center for that menu use.
void updateMenuBall() {
  float xTilt = accelerationX - menuCenterAccelerationX;
  float yTilt = accelerationY - menuCenterAccelerationY;

  if (SWAP_MPU_AXES) {
    const float oldXTilt = xTilt;
    xTilt = yTilt;
    yTilt = oldXTilt;
  }
  xTilt = constrain(xTilt * MENU_X_DIRECTION, -MENU_TILT_LIMIT, MENU_TILT_LIMIT);
  yTilt = constrain(yTilt * MENU_Y_DIRECTION, -MENU_TILT_LIMIT, MENU_TILT_LIMIT);
  const float targetX = MENU_CENTER_X + (xTilt / MENU_TILT_LIMIT) * MENU_BALL_X_RANGE;
  const float targetY = MENU_CENTER_Y + (yTilt / MENU_TILT_LIMIT) * MENU_BALL_Y_RANGE;

  // This small amount of smoothing makes the ball feel less jittery.
  menuBallX += (targetX - menuBallX) * 0.20f;
  menuBallY += (targetY - menuBallY) * 0.20f;

  // The menu has four full sectors, not four tiny targets. Any tilt direction
  // past the center dead zone selects its matching top/right/bottom/left action.
  if (fabsf(xTilt) < MENU_CENTER_DEADZONE && fabsf(yTilt) < MENU_CENTER_DEADZONE) {
    selectedMenuItem = -1;
  } else if (fabsf(xTilt) > fabsf(yTilt)) {
    selectedMenuItem = xTilt > 0.0f ? 1 : 3;  // right or left
  } else {
    selectedMenuItem = yTilt > 0.0f ? 2 : 0;  // bottom or top
  }
}

void checkForShake() {
  if (!mpuFound || currentView != PET_VIEW) {
    return;
  }

  const float magnitude = sqrtf(accelerationX * accelerationX +
                                accelerationY * accelerationY +
                                accelerationZ * accelerationZ);
  const uint32_t now = millis();
  if (fabsf(magnitude - STANDARD_GRAVITY) >= SHAKE_THRESHOLD &&
      now - lastShakeAt >= SHAKE_COOLDOWN_MS) {
    lastShakeAt = now;
    nappingUntil = 0;  // A shake wakes a sleeping pet right away.
    shakeAnimationEndsAt = now + 350;
    changePet(SHAKE_JOY_CHANGE, SHAKE_ENERGY_CHANGE, SHAKE_FULLNESS_CHANGE);
    savePet();
  }
}

void openMenu() {
  currentView = MENU_VIEW;
  selectedMenuItem = -1;
  menuBallX = MENU_CENTER_X;
  menuBallY = MENU_CENTER_Y;
  // Calibrate here so every menu starts from however the builder is holding it.
  menuCenterAccelerationX = accelerationX;
  menuCenterAccelerationY = accelerationY;
}

void chooseMenuItem() {
  if (selectedMenuItem == -1) {
    return;  // Keep the menu open until the ball reaches a choice.
  }

  const MenuItem &item = MENU_ITEMS[selectedMenuItem];
  changePet(item.joyChange, item.energyChange, item.fullnessChange);
  savePet();
  const uint32_t now = millis();
  nappingUntil = 0;
  heartAnimationEndsAt = 0;
  playRunStartedAt = 0;

  if (item.reaction == NAP_REACTION) {
    petJumpStartedAt = 0;
    nappingPetX = petWalkingX(now);
    nappingUntil = now + NAP_DURATION_MS;
  } else if (item.reaction == RUN_REACTION) {
    petJumpStartedAt = 0;
    playRunStartedAt = now;
    heartAnimationEndsAt = now + playRunDuration();
  } else {
    // A confirmed action gets a little anticipation shake before the jump.
    petJumpStartedAt = now;
    if (item.reaction == HEART_REACTION) {
      heartAnimationEndsAt = now + HEARTS_DURATION_MS;
    }
  }
  currentView = PET_VIEW;
}

void handleButtons() {
  if (wasPressed(buttonOne)) {
    if (currentView == MENU_VIEW) {
      chooseMenuItem();
    } else if (currentView == STOCKS_VIEW) {
      nextTicker();  // On the stock screen Button 1 flips through tickers.
    } else {
      openMenu();
    }
  }

  if (wasPressed(buttonTwo)) {
    // Button 2 never changes the pet. It only moves between screens.
    switch (currentView) {
      case STOCKS_VIEW: currentView = PET_VIEW; break;
      case PET_VIEW:    currentView = STATS_VIEW; break;
      case STATS_VIEW:  currentView = STOCKS_VIEW; break;
      default:          currentView = PET_VIEW; break;  // Leaving the menu.
    }
    lastTickerChangeAt = millis();
  }

  // Hands-free mode: rotate through the watchlist while the stock screen is up.
  if (currentView == STOCKS_VIEW && AUTO_ROTATE_MS > 0 && TICKER_COUNT > 1 &&
      millis() - lastTickerChangeAt >= AUTO_ROTATE_MS) {
    nextTicker();
  }
}

// -------------------------------- Drawing ---------------------------------
void drawHeart(int x, int y) {
  // A tiny seven-pixel-wide heart that stays crisp on the OLED.
  display.fillRect(x - 2, y, 2, 2, SSD1306_WHITE);
  display.fillRect(x + 1, y, 2, 2, SSD1306_WHITE);
  display.fillRect(x - 3, y + 2, 7, 2, SSD1306_WHITE);
  display.fillRect(x - 2, y + 4, 5, 1, SSD1306_WHITE);
  display.fillRect(x - 1, y + 5, 3, 1, SSD1306_WHITE);
  display.drawPixel(x, y + 6, SSD1306_WHITE);
}

void drawHearts(uint32_t now, int petX, int petY) {
  if (now >= heartAnimationEndsAt) {
    return;
  }

  const float progress =
      1.0f - static_cast<float>(heartAnimationEndsAt - now) / HEARTS_DURATION_MS;
  const int rise = static_cast<int>(progress * 18.0f);
  drawHeart(petX + 10, petY - 3 - rise);
  drawHeart(petX + 22, petY - 9 - rise / 2);
}

void drawSleepZs(uint32_t now, int petX, int petY) {
  // The Zs float upward, then loop back down while the pet is napping.
  const int rise = static_cast<int>((now / 300UL) % 15UL);
  display.setTextSize(1);
  display.setTextColor(SSD1306_WHITE);
  display.setCursor(petX + 23, petY - 2 - rise);
  display.print("z");
  display.setCursor(petX + 28, petY - 8 - rise / 2);
  display.print("z");
}

void drawPet() {
  display.clearDisplay();

  const uint32_t now = millis();
  int petX = isNapping() ? nappingPetX : petWalkingX(now);
  if (isPlaying()) {
    petX = playRunX(now);
  }
  int petY = SCREEN_HEIGHT - PET_SPRITE_HEIGHT;

  // The pet always wanders except during a nap; PLAY temporarily takes over.
  if (now < shakeAnimationEndsAt) {
    petX += static_cast<int>(sinf(now / 18.0f) * 3.0f);
  }

  if (!isNapping() && petJumpStartedAt != 0) {
    const uint32_t animationAge = now - petJumpStartedAt;

    if (animationAge < PET_PRE_JUMP_MS) {
      petX += static_cast<int>(sinf(now / 16.0f) * 3.0f);
    } else if (animationAge < PET_PRE_JUMP_MS + PET_JUMP_MS) {
      const float jumpProgress =
          static_cast<float>(animationAge - PET_PRE_JUMP_MS) / PET_JUMP_MS;
      petY -= static_cast<int>(sinf(jumpProgress * PI) * PET_JUMP_HEIGHT);
    } else {
      petJumpStartedAt = 0;
    }
  }

  petX = constrain(petX, 0, SCREEN_WIDTH - PET_SPRITE_WIDTH);
  display.drawBitmap(petX, petY, PET_SPRITE, PET_SPRITE_WIDTH, PET_SPRITE_HEIGHT,
                     SSD1306_WHITE);

  if (isNapping()) {
    drawSleepZs(now, petX, petY);
  }
  drawHearts(now, petX, petY);
}

void drawMenuItem(int item) {
  const int boxWidth = 33;
  const int boxHeight = 12;
  const int boxX = MENU_ITEM_X[item] - boxWidth / 2;
  const int boxY = MENU_ITEM_Y[item] - boxHeight / 2;
  const bool isSelected = item == selectedMenuItem;

  if (isSelected) {
    display.fillRoundRect(boxX, boxY, boxWidth, boxHeight, 3, SSD1306_WHITE);
    display.setTextColor(SSD1306_BLACK);
  } else {
    display.drawRoundRect(boxX, boxY, boxWidth, boxHeight, 3, SSD1306_WHITE);
    display.setTextColor(SSD1306_WHITE);
  }

  display.setTextSize(1);
  const int labelLength = strlen(MENU_ITEMS[item].label);
  display.setCursor(MENU_ITEM_X[item] - labelLength * 3, MENU_ITEM_Y[item] - 3);
  display.print(MENU_ITEMS[item].label);
  display.setTextColor(SSD1306_WHITE);
}

void drawMenu() {
  display.clearDisplay();
  // The two diagonal lines divide the screen into four big selection sectors.
  display.drawLine(0, 0, SCREEN_WIDTH - 1, SCREEN_HEIGHT - 1, SSD1306_WHITE);
  display.drawLine(0, SCREEN_HEIGHT - 1, SCREEN_WIDTH - 1, 0, SSD1306_WHITE);
  for (int item = 0; item < MENU_ITEM_COUNT; item++) {
    drawMenuItem(item);
  }

  display.drawCircle(MENU_CENTER_X, MENU_CENTER_Y, 11, SSD1306_WHITE);
  display.fillCircle(static_cast<int>(menuBallX), static_cast<int>(menuBallY), 4,
                     SSD1306_WHITE);
}

void drawStatBar(int y, const char *label, int value) {
  display.setTextSize(1);
  display.setTextColor(SSD1306_WHITE);
  display.setCursor(4, y);
  display.print(label);
  display.drawRect(48, y, 57, 8, SSD1306_WHITE);
  display.fillRect(49, y + 1, map(value, 0, 100, 0, 55), 6, SSD1306_WHITE);
  display.setCursor(109, y);
  display.print(value);
}

void drawStats() {
  display.clearDisplay();
  drawStatBar(5, "JOY", pet.joy);
  drawStatBar(20, "ENERGY", pet.energy);
  drawStatBar(35, "FULL", pet.fullness);

  display.setCursor(4, 53);
  if (!isnan(temperatureC)) {
    display.print("TEMP ");
    display.print(static_cast<int>(temperatureC));
    display.print("C");
  } else {
    display.print("TEMP --");
  }

  display.setCursor(76, 53);
  if (!isnan(humidity)) {
    display.print("H ");
    display.print(static_cast<int>(humidity));
    display.print("%");
  } else {
    display.print("H --");
  }
}

// Draws a small filled triangle: pointing up for gains, down for losses.
void drawArrow(int x, int y, bool up) {
  if (up) {
    display.fillTriangle(x, y + 6, x + 3, y, x + 6, y + 6, SSD1306_WHITE);
  } else {
    display.fillTriangle(x, y, x + 3, y + 6, x + 6, y, SSD1306_WHITE);
  }
}

// Draws today's price line in the area under the numbers, with a dotted line
// where yesterday's close was so you can see if you are above or below it.
void drawSparkline(const StockData &stock) {
  const int left = 0;
  const int right = SCREEN_WIDTH - 1;
  const int top = 40;
  const int bottom = SCREEN_HEIGHT - 1;

  if (stock.pointCount < 2) {
    return;
  }

  float lowest = stock.previousClose;
  float highest = stock.previousClose;
  for (int i = 0; i < stock.pointCount; i++) {
    lowest = min(lowest, stock.points[i]);
    highest = max(highest, stock.points[i]);
  }
  float range = highest - lowest;
  if (range < 0.0001f) {
    range = 1.0f;  // A perfectly flat line: avoid dividing by zero.
  }

  auto yFor = [&](float value) {
    return bottom - static_cast<int>(((value - lowest) / range) * (bottom - top));
  };

  const int previousCloseY = yFor(stock.previousClose);
  for (int x = left; x <= right; x += 4) {
    display.drawPixel(x, previousCloseY, SSD1306_WHITE);
  }

  int lastX = left;
  int lastY = yFor(stock.points[0]);
  for (int i = 1; i < stock.pointCount; i++) {
    const int x = left + (i * (right - left)) / (stock.pointCount - 1);
    const int y = yFor(stock.points[i]);
    display.drawLine(lastX, lastY, x, y, SSD1306_WHITE);
    lastX = x;
    lastY = y;
  }
  display.fillCircle(lastX, lastY, 2, SSD1306_WHITE);  // Dot on the latest price.
}

void drawStocks() {
  display.clearDisplay();
  display.setTextSize(1);
  display.setTextColor(SSD1306_WHITE);

  StockData stock;
  portENTER_CRITICAL(&stockLock);
  stock = stocks[currentTicker];
  portEXIT_CRITICAL(&stockLock);

  display.setCursor(0, 0);
  display.print(TICKERS[currentTicker]);

  if (!stock.valid) {
    display.setCursor(0, 26);
    if (netStatus == NET_ERROR) {
      display.print("No data. Check WiFi");
      display.setCursor(0, 38);
      display.print("name, password and");
      display.setCursor(0, 50);
      display.print("ticker symbols.");
    } else {
      display.print("Loading prices...");
    }
    return;
  }

  const float change = stock.price - stock.previousClose;
  const float percent = (stock.previousClose > 0.0f)
                            ? (change / stock.previousClose) * 100.0f
                            : 0.0f;
  const bool isUp = change >= 0.0f;

  // Top right: arrow and percent change.
  char percentText[12];
  snprintf(percentText, sizeof(percentText), "%+.2f%%", percent);
  const int percentX = SCREEN_WIDTH - static_cast<int>(strlen(percentText)) * 6;
  drawArrow(percentX - 9, 1, isUp);
  display.setCursor(percentX, 0);
  display.print(percentText);

  // Big price.
  char priceText[16];
  if (stock.price >= 100000.0f) {
    snprintf(priceText, sizeof(priceText), "%.0f", stock.price);
  } else if (stock.price >= 1000.0f) {
    snprintf(priceText, sizeof(priceText), "%.1f", stock.price);
  } else {
    snprintf(priceText, sizeof(priceText), "%.2f", stock.price);
  }
  display.setTextSize(2);
  display.setCursor(0, 11);
  display.print(priceText);
  display.setTextSize(1);

  // Dollar change on the left, ticker position on the right.
  char changeText[16];
  snprintf(changeText, sizeof(changeText), "%+.2f today", change);
  display.setCursor(0, 29);
  display.print(changeText);

  if (TICKER_COUNT > 1) {
    char positionText[8];
    snprintf(positionText, sizeof(positionText), "%d/%d", currentTicker + 1,
             TICKER_COUNT);
    display.setCursor(SCREEN_WIDTH - static_cast<int>(strlen(positionText)) * 6, 29);
    display.print(positionText);
  }

  drawSparkline(stock);
}

void drawCurrentView() {
  if (currentView == MENU_VIEW) {
    drawMenu();
  } else if (currentView == STATS_VIEW) {
    drawStats();
  } else if (currentView == STOCKS_VIEW) {
    drawStocks();
  } else {
    drawPet();
  }
  display.display();
}

void setup() {
  Serial.begin(115200);
  Wire.begin(I2C_SDA_PIN, I2C_SCL_PIN);
  setUpButton(buttonOne);
  setUpButton(buttonTwo);
  loadPet();

  if (!display.begin(SSD1306_SWITCHCAPVCC, OLED_ADDRESS)) {
    Serial.println("OLED not found. Check power, GND, SDA, SCL, and address.");
    while (true) {
      delay(10);
    }
  }

  mpuFound = mpu.begin(MPU6050_ADDRESS, &Wire);
  if (mpuFound) {
    mpu.setAccelerometerRange(MPU6050_RANGE_8_G);
    mpu.setGyroRange(MPU6050_RANGE_500_DEG);
    mpu.setFilterBandwidth(MPU6050_BAND_21_HZ);
  } else {
    Serial.println("MPU6050 not found. The menu will not move until it is connected.");
  }

  if (USE_DHT11) {
    dht.begin();
    dhtFound = true;
  }

  // Start the background WiFi + stock task. It needs a big stack for HTTPS.
  WiFi.mode(WIFI_STA);
  xTaskCreate(stockTask, "stocks", 16384, nullptr, 1, nullptr);

  currentView = START_ON_STOCKS ? STOCKS_VIEW : PET_VIEW;
  lastTickerChangeAt = millis();
  drawCurrentView();
}

void loop() {
  updateMpu();
  updateDht();
  updatePetTimers();
  handleButtons();

  if (currentView == MENU_VIEW) {
    updateMenuBall();
  } else {
    checkForShake();
  }

  drawCurrentView();
  delay(16);  // Smooth display updates without making the code complicated.
}
