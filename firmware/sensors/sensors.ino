// Ambient sensor node — DHT11 (P1), GNSS ATGM336H (P3), ST7735 display (P4)
// Posts temp/humidity/GPS to BACKEND_URL every 60 seconds.
// Backend response is parsed for place, sky, outside_c, time — shown on display.
//
// Second board, on a Genesis Mini. It never talks to the speaker: both boards
// only know the backend URL, so either can be off without affecting the other.
//
// It has a screen but no way to know the weather or where it is, so the reply
// to its own POST carries that back. No second request, no second endpoint.
//
// Copy secrets.h.example to secrets.h before building.

#include "secrets.h"
#include <WiFi.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <Adafruit_Sensor.h>
#include <DHT.h>
#include <DHT_U.h>
#include <TinyGPSPlus.h>
#include <SPI.h>
#include <Adafruit_GFX.h>
#include <Adafruit_ST7735.h>

// ── Pins ──────────────────────────────────────────────────────────────────────
#define DHTPIN   P1_IO1   // DHT11 single-wire data (P1)
#define DHTTYPE  DHT11
// GPS UART: P3_IO1 = RX (connect to GPS module TX), P3_IO2 = TX (connect to GPS module RX)
// 115200, not the 9600 the datasheet claims: at 9600 the raw echo showed garbage,
// which means bytes were arriving at the wrong rate rather than not at all.
// Display (P4, shared FSPI bus): CS=P4_IO0, RST=P4_IO1, DC=P4_IO2
#define TFT_CS   P4_IO0
#define TFT_RST  P4_IO1
#define TFT_DC   P4_IO2

// ── Display objects ───────────────────────────────────────────────────────────
SPIClass           mySPI(FSPI);
Adafruit_ST7735    tft = Adafruit_ST7735(&mySPI, TFT_CS, TFT_DC, TFT_RST);
GFXcanvas16        canvas(160, 80);

// BGR colour helper — this panel has swapped R/B channels; always pass (r,g,b) here.
inline uint16_t col(uint8_t r, uint8_t g, uint8_t b) { return tft.color565(b, g, r); }
const uint16_t C_BG    = 0x0000;
const uint16_t C_DIM   = col(100, 100, 100);
const uint16_t C_WHITE = col(255, 255, 255);
const uint16_t C_CYAN  = col(  0, 255, 255);
const uint16_t C_RED   = col(255,   0,   0);

// ── Sensor objects ────────────────────────────────────────────────────────────
DHT_Unified     dht(DHTPIN, DHTTYPE);
TinyGPSPlus     gps;
HardwareSerial  GPS_Serial(1);   // UART1; `Serial` stays the USB-CDC link to Studio

// ── Display state — updated after each POST ───────────────────────────────────
struct DispState {
  bool   hasData   = false;   // false until the first successful POST
  bool   postOk    = false;   // false → show offline marker
  float  tempC     = NAN;
  float  humidity  = NAN;
  float  outsideC  = NAN;
  String place     = "";
  String sky       = "";
  String timeStr   = "";
};
DispState disp;

// ── Timing ────────────────────────────────────────────────────────────────────
const unsigned long POST_INTERVAL     = 60000UL;
const unsigned long GPS_ECHO_DURATION =  5000UL;  // echo raw GPS bytes for 5 s after boot
unsigned long lastPost = 0;

// ── Forward declarations ───────────────────────────────────────────────────────
void updateDisplay();
void showBootMessage(const char* line1, const char* line2 = nullptr);

// ── Setup ─────────────────────────────────────────────────────────────────────
void setup() {
  Serial.begin(115200);
  Serial.println("\n=== Ambient node booting ===");

  mySPI.begin(SCK, MISO, MOSI);
  tft.initR(INITR_MINI160x80);
  tft.setRotation(3);
  showBootMessage("Connecting...");

  dht.begin();

  GPS_Serial.begin(115200, SERIAL_8N1, P3_IO1, P3_IO2);
  Serial.println("GPS UART started at 115200 baud.");
  Serial.println("GPS raw-echo ON for 5 s — readable $GP... = correct baud; garbage = wrong baud.");

  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  Serial.printf("Connecting to WiFi '%s'", WIFI_SSID);
  unsigned long t0 = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - t0 < 15000) {
    while (GPS_Serial.available()) gps.encode(GPS_Serial.read());  // drain GPS during join
    delay(500);
    Serial.print(".");
  }
  if (WiFi.status() == WL_CONNECTED)
    Serial.printf("\nConnected! IP: %s\n", WiFi.localIP().toString().c_str());
  else
    Serial.printf("\nWiFi failed for '%s' — check credentials.\n", WIFI_SSID);

  showBootMessage("Connecting...", "Waiting for data...");

  lastPost = millis() - POST_INTERVAL;  // first POST fires immediately
  Serial.println("First POST firing immediately.");
}

// ── Loop ──────────────────────────────────────────────────────────────────────
void loop() {
  // Feed GPS parser on EVERY iteration — must not be gated on the 60-s timer.
  // During the first 5 s also echo raw bytes to confirm baud rate visually.
  static unsigned long gpsBytesTotal = 0;
  while (GPS_Serial.available()) {
    uint8_t b = GPS_Serial.read();
    gpsBytesTotal++;
    if (millis() < GPS_ECHO_DURATION) Serial.write(b);
    gps.encode(b);
  }
  static bool gpsWarnPrinted = false;
  if (!gpsWarnPrinted && millis() > 10000 && gpsBytesTotal == 0) {
    Serial.println("WARNING: zero GPS bytes received after 10 s — check P3_IO1→GPS-TX wiring and module power");
    gpsWarnPrinted = true;
  }

  unsigned long now = millis();
  if (now - lastPost < POST_INTERVAL) return;
  lastPost = now;

  // ── Read DHT11 (once per cycle; 60 s >> 2 s minimum interval) ───────────────
  sensors_event_t tempEv, humEv;
  dht.temperature().getEvent(&tempEv);
  dht.humidity().getEvent(&humEv);
  disp.tempC    = tempEv.temperature;
  disp.humidity = humEv.relative_humidity;

  Serial.printf("GPS sats: %d  fix: %s\n",
                (int)gps.satellites.value(),
                gps.location.isValid() ? "YES" : "NO");

  // ── Build JSON payload ───────────────────────────────────────────────────────
  DynamicJsonDocument doc(384);  // ArduinoJson 6.21.5; JsonDocument (v7 API) not available

  if (isnan(tempEv.temperature))         doc["temp_c"]   = nullptr;
  else                                   doc["temp_c"]   = tempEv.temperature;
  if (isnan(humEv.relative_humidity))    doc["humidity"] = nullptr;
  else                                   doc["humidity"] = humEv.relative_humidity;
  if (gps.location.isValid()) {
    doc["lat"]     = gps.location.lat();
    doc["lon"]     = gps.location.lng();
    doc["has_fix"] = true;
  } else {
    doc["has_fix"] = false;
  }

  String payload;
  serializeJson(doc, payload);
  Serial.print("Payload: ");
  Serial.println(payload);

  // ── POST ─────────────────────────────────────────────────────────────────────
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("WiFi not connected — skipping POST, will retry next cycle");
    disp.postOk = false;
    updateDisplay();
    return;
  }

  WiFiClient client;
  HTTPClient http;
  String url = String(BACKEND_URL) + "/ambient";
  Serial.printf("POST %s\n", url.c_str());  // trailing-slash check: should not show double //
  http.begin(client, url);                  // two-arg form required on recent ESP32 cores
  http.addHeader("Content-Type", "application/json");
  int code = http.POST(payload);

  if (code > 0) {
    Serial.printf("POST -> HTTP %d\n", code);
    if (code == 200) {
      String body = http.getString();
      DynamicJsonDocument resp(512);
      DeserializationError err = deserializeJson(resp, body);
      if (!err) {
        // Place: use only the part before the first comma
        String placeFull = String(resp["place"] | "");
        int comma = placeFull.indexOf(',');
        disp.place   = (comma > 0) ? placeFull.substring(0, comma) : placeFull;
        disp.sky     = String(resp["sky"]  | "");
        disp.timeStr = String(resp["time"] | "");
        disp.outsideC = resp["outside_c"].isNull() ? NAN : resp["outside_c"].as<float>();
      }
      disp.postOk  = true;
      disp.hasData = true;
    }
  } else {
    Serial.printf("POST failed (HTTPClient error %d) — will retry next cycle\n", code);
    disp.postOk = false;
  }

  http.end();
  updateDisplay();
}

// ── Display helpers ────────────────────────────────────────────────────────────

// Boot splash: one or two centred lines in dim grey.
void showBootMessage(const char* line1, const char* line2) {
  canvas.fillScreen(C_BG);
  canvas.setTextColor(C_DIM);
  canvas.setTextSize(1);
  if (!line2) {
    canvas.setCursor(4, 36);
    canvas.print(line1);
  } else {
    canvas.setCursor(4, 28);
    canvas.print(line1);
    canvas.setCursor(4, 42);
    canvas.print(line2);
  }
  tft.drawRGBBitmap(0, 0, canvas.getBuffer(), 160, 80);
}

// Layout (all Y positions verified against 160×80 bounds):
//   y= 2  header: time (left) + place (right), size 1 (8 px tall) → y 2–9   ✓
//   y=12  divider line (1 px)                                                 ✓
//   y=16  IN row, size 2 (16 px tall) → y 16–31   "IN" | temp | humidity     ✓
//   y=36  OUT row, size 2 (16 px tall) → y 36–51  "OUT" | outside_c          ✓
//   y=56  sky description, size 1 (8 px tall) → y 56–63, indented x=30       ✓
//   y=70  offline marker, size 1 (8 px tall) → y 70–77                       ✓
//
// Size-2 char = 12×16 px.  Size-1 char = 6×8 px.
void updateDisplay() {
  if (!disp.hasData) return;  // still showing boot message

  canvas.fillScreen(C_BG);
  char buf[32];

  // ── Header ───────────────────────────────────────────────────────────────────
  canvas.setTextSize(1);
  canvas.setTextColor(C_DIM);
  canvas.setCursor(2, 2);
  canvas.print(disp.timeStr.length() ? disp.timeStr : "--:--");

  // Place: right-align, cap at 16 chars (16 × 6 px = 96 px; rightmost x = 158)
  String placeTrunc = disp.place;
  if (placeTrunc.length() > 16) placeTrunc = placeTrunc.substring(0, 16);
  int placeX = 158 - (int)placeTrunc.length() * 6;
  if (placeX < 80) placeX = 80;  // don't crowd the time text
  canvas.setCursor(placeX, 2);
  canvas.print(placeTrunc);

  // ── Divider ───────────────────────────────────────────────────────────────────
  canvas.drawFastHLine(0, 12, 160, C_DIM);

  // ── IN row ────────────────────────────────────────────────────────────────────
  // Columns: "IN" x=0 (24px), temp x=30, humidity x=90
  canvas.setTextSize(2);
  canvas.setTextColor(C_WHITE);
  canvas.setCursor(0, 16);
  canvas.print("IN");

  canvas.setCursor(30, 16);
  if (!isnan(disp.tempC)) {
    snprintf(buf, sizeof(buf), "%d%cC", (int)round(disp.tempC), (char)247);
  } else {
    snprintf(buf, sizeof(buf), "--C");
  }
  canvas.print(buf);

  canvas.setCursor(90, 16);
  if (!isnan(disp.humidity)) {
    snprintf(buf, sizeof(buf), "%d%%", (int)round(disp.humidity));
  } else {
    snprintf(buf, sizeof(buf), "--%");
  }
  canvas.print(buf);

  // ── OUT row ───────────────────────────────────────────────────────────────────
  // "OUT" x=0 (36px), outside_c x=42 (leaves space; "OUT" = 3 chars × 12 px = 36 px)
  canvas.setTextColor(C_CYAN);
  canvas.setCursor(0, 36);
  canvas.print("OUT");

  canvas.setCursor(42, 36);
  if (!isnan(disp.outsideC)) {
    snprintf(buf, sizeof(buf), "%d%cC", (int)round(disp.outsideC), (char)247);
  } else {
    snprintf(buf, sizeof(buf), "--");
  }
  canvas.print(buf);

  // ── Sky description (size 1, indented) ───────────────────────────────────────
  // x=42 aligns with OUT temp column. Max 19 chars (19×6=114 px; 42+114=156 ≤ 160).
  canvas.setTextSize(1);
  canvas.setCursor(42, 56);
  String skyTrunc = disp.sky;
  if (skyTrunc.length() > 19) skyTrunc = skyTrunc.substring(0, 19);
  canvas.print(skyTrunc);

  // ── Offline marker ────────────────────────────────────────────────────────────
  if (!disp.postOk) {
    canvas.setTextColor(C_RED);
    canvas.setTextSize(1);
    canvas.setCursor(2, 70);
    canvas.print("offline");
  }

  tft.drawRGBBitmap(0, 0, canvas.getBuffer(), 160, 80);
}
