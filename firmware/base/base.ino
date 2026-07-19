// soundBud base — the thing that makes noise.
//
//   P1 NeoPixel 5x5 | P2 DHT11 | P4 rotary encoder
//   P5 ST7735 display | P6 passive buzzer | P7 MAX98357A amp
//
// No microphone on this board. It long-polls /playback and acts on anything with
// a sequence number higher than the last one it handled. The remote does the
// listening; the two boards never talk to each other.
//
// Copy secrets.h.example to secrets.h before building.

#include "secrets.h"
#include <WiFi.h>
#include <HTTPClient.h>
#include <SPI.h>
#include <Adafruit_GFX.h>
#include <Adafruit_ST7735.h>
#include <Audio.h>
#include <ArduinoJson.h>
#include <RotaryEncoder.h>
#include <Adafruit_NeoPixel.h>
#include <Adafruit_Sensor.h>
#include <DHT.h>
#include <DHT_U.h>

// ── Pins ─────────────────────────────────────────────────────────
// P5 — ST7735 TFT
#define TFT_CS   P5_IO0
#define TFT_RST  P5_IO1
#define TFT_DC   P5_IO2
// P7 — amp: BCLK=P7_IO1, LRC=P7_IO2, DIN=P7_IO0
// P4 — rotary encoder
#define ENC_BTN  P4_IO0
#define ENC_CLK  P4_IO1
#define ENC_DT   P4_IO2
// P1 — NeoPixel 5x5. Keep this off P8: that is GPIO44 / UART0, and serial
// output gets clocked into the LEDs as pixel data — half the matrix flickers
// and digits come out as scatter.
#define MATRIX_PIN P1_IO1
// P6 — passive buzzer
#define BUZZER_PIN     P6_IO1
#define BUZZER_ENABLED 1
// P2 — DHT11
#define DHTPIN  P2_IO1
#define DHTTYPE DHT11

enum AppState { IDLE, PROCESSING, SPEAKING, PLAYING };
AppState appState = IDLE;

SPIClass          mySPI(FSPI);
Adafruit_ST7735   tft = Adafruit_ST7735(&mySPI, TFT_CS, TFT_DC, TFT_RST);
GFXcanvas16       canvas(160, 80);
GFXcanvas16       barCanvas(160, 64);
Audio             audio;
DHT_Unified       dht(DHTPIN, DHTTYPE);

char          screenText[24]  = "";
unsigned long playStartedMs   = 0;
bool          musicPending    = false;
bool          fetchTrack      = false;
bool          keepPlaying     = false;
bool          isPaused        = false;
bool          displayDirty    = true;
unsigned long lastDrawMs      = 0;
long          lastSeq         = -1;
unsigned long lastAmbientMs   = 0;
unsigned long lastPollMs      = 0;

RotaryEncoder     encoder(ENC_CLK, ENC_DT, RotaryEncoder::LatchMode::TWO03);
Adafruit_NeoPixel strip(25, MATRIX_PIN, NEO_GRB + NEO_KHZ800);
int           displayVolume   = 5;
long          encLastPos      = 0;
unsigned long volChangedMs    = 0;
int           matrixLastLevel = -1;
bool          encBtnLast      = LOW;

// ── Buzzer ───────────────────────────────────────────────────────
struct Beep { uint16_t freq; uint16_t ms; };

const Beep BEEP_WORK[]   = {{1600, 15}, {0, 20}, {2000, 25}};
const Beep BEEP_TRACK[]  = {{1400, 15}, {0, 20}, {1800, 15}, {0, 20}, {2200, 30}};
const Beep BEEP_PAUSE[]  = {{900,  25}, {0, 25}, {700,  35}};
const Beep BEEP_RESUME[] = {{700,  25}, {0, 25}, {900,  35}};
const Beep BEEP_TICK[]   = {{2600,  6}};
const Beep BEEP_LIMIT[]  = {{2600,  6}, {0,  8}, {2600,  6}};
const Beep BEEP_ERROR[]  = {{400,  60}, {0, 40}, {400,  60}};

#define PLAY_BEEP(arr) playBeep(arr, sizeof(arr)/sizeof(arr[0]))

const Beep*   beepSeq    = nullptr;
uint8_t       beepLen    = 0, beepIdx = 0;
unsigned long beepStepMs = 0;
unsigned long lastTickMs = 0;

// Sequenced from loop(), never with delay() — a blocking beep would stall
// audio.loop() and glitch the stream.
void playBeep(const Beep* seq, uint8_t len) {
#if BUZZER_ENABLED
  beepSeq = seq; beepLen = len; beepIdx = 0;
  beepStepMs = millis();
  noTone(BUZZER_PIN);
  if (seq[0].freq > 0) tone(BUZZER_PIN, seq[0].freq);
#endif
}

void updateBeeps() {
#if BUZZER_ENABLED
  if (!beepSeq || beepIdx >= beepLen) return;
  if (millis() - beepStepMs < beepSeq[beepIdx].ms) return;
  beepIdx++;
  if (beepIdx >= beepLen) { noTone(BUZZER_PIN); beepSeq = nullptr; return; }
  beepStepMs = millis();
  if (beepSeq[beepIdx].freq > 0) tone(BUZZER_PIN, beepSeq[beepIdx].freq);
  else                            noTone(BUZZER_PIN);
#endif
}

// ── Equaliser state ──────────────────────────────────────────────
float            barH[10]        = {};
float            peakH[10]       = {};
float            synthTgt[10]    = {};
unsigned long    synthNextMs[10] = {};
unsigned long    lastVizMs       = 0;
bool             vizTitleDirty   = true;
bool             vizModeLogged   = false;
volatile int16_t audioAmp        = 0;
bool             hookSeen        = false;

// ── Matrix ───────────────────────────────────────────────────────
// Pause glyph and digits are both left-aligned. Digits are 3 wide, bit2 first.
const uint8_t PAUSE_GLYPH[5] = {0b10100, 0b10100, 0b10100, 0b10100, 0b10100};
const uint8_t DIGITS[10][5]  = {
  {0b111,0b101,0b101,0b101,0b111},  // 0
  {0b010,0b110,0b010,0b010,0b111},  // 1
  {0b111,0b001,0b111,0b100,0b111},  // 2
  {0b111,0b001,0b111,0b001,0b111},  // 3
  {0b101,0b101,0b111,0b001,0b001},  // 4
  {0b111,0b100,0b111,0b001,0b111},  // 5
  {0b111,0b100,0b111,0b101,0b111},  // 6
  {0b111,0b001,0b001,0b001,0b001},  // 7
  {0b111,0b101,0b111,0b101,0b111},  // 8
  {0b111,0b101,0b111,0b001,0b111},  // 9
};

// 1 = serpentine (odd rows reversed), which is common on 5x5 panels. If digits
// come out mirrored on alternate rows, this is the knob.
#define MATRIX_SERPENTINE 1
#define MATRIX_FLIP_Y     0   // 1 if pixel 0 is bottom-left
#define MATRIX_IDENTIFY   0   // lights pixels 0-24 in order, to map the wiring
#define MATRIX_DIGIT_TEST 0   // sweeps 0-9 at boot

int pixelIndex(int row, int col) {
#if MATRIX_FLIP_Y
  row = 4 - row;
#endif
#if MATRIX_SERPENTINE
  return row * 5 + ((row % 2) ? (4 - col) : col);
#else
  return row * 5 + col;
#endif
}

void setVolume(int v) {
  v = max(0, min(9, v));
  displayVolume = v;
  audio.setVolume((v * 21 + 4) / 9);   // 0..9 knob scale -> 0..21 library scale
  volChangedMs = millis();
}

void updateMatrix() {
  int desired;
  bool volRecent = (millis() - volChangedMs <= 3000);
  if      (isPaused && !volRecent)           desired = 10;
  else if (volRecent || appState == PLAYING) desired = displayVolume;
  else                                       desired = -1;

  // NeoPixel writes disable interrupts, so pushing every loop glitches the I2S
  // stream audibly. Only ever push when the content actually changed.
  if (desired == matrixLastLevel) return;
  matrixLastLevel = desired;

  if (desired == -1) {
    for (int i = 0; i < 25; i++) strip.setPixelColor(i, 0);
  } else if (desired == 10) {
    uint32_t c = strip.Color(180, 100, 0);
    for (int row = 0; row < 5; row++)
      for (int col = 0; col < 5; col++) {
        bool lit = (PAUSE_GLYPH[row] >> (4 - col)) & 1;
        strip.setPixelColor(pixelIndex(row, col), lit ? c : 0);
      }
  } else {
    uint32_t colour;
    if      (displayVolume <= 3) colour = strip.Color(0,   180,   0);
    else if (displayVolume <= 6) colour = strip.Color(180, 100,   0);
    else                         colour = strip.Color(180,   0,   0);
    const uint8_t* glyph = DIGITS[displayVolume];
    for (int row = 0; row < 5; row++)
      for (int col = 0; col < 5; col++) {
        bool lit = (col <= 2) && (glyph[row] & (0b100 >> col));
        strip.setPixelColor(pixelIndex(row, col), lit ? colour : 0);
      }
  }
  strip.show();
}

// Weak-linked by ESP32-audioI2S when the version exposes it; drives the real
// equaliser. When it does not exist, hookSeen stays false and the bars animate
// synthetically instead.
void audio_process_i2s(int16_t* buff, uint16_t n, uint8_t, bool*) {
  int32_t pk = 0;
  for (uint16_t i = 0; i < n; i++) {
    int32_t s = abs((int32_t)buff[i]);
    if (s > pk) pk = s;
  }
  audioAmp = (int16_t)min((int32_t)32767, pk);
  hookSeen = true;
}

// BGR panel — col() takes (r,g,b) and swaps.
uint16_t col(uint8_t r, uint8_t g, uint8_t b) { return tft.color565(b, g, r); }

// ── Equaliser ────────────────────────────────────────────────────
void drawVizTitle() {
  tft.fillRect(0, 0, 160, 14, 0x0000);
  tft.setTextColor(col(0, 150, 80));
  tft.setTextSize(1);
  tft.setCursor(4, 3);
  tft.print(screenText[0] ? screenText : "Playing");
  vizTitleDirty = false;
}

void renderBar(int idx, int h, bool dim) {
  int x  = idx * 16;
  int gH = min(h, 21);
  int aH = max(0, min(h - 21, 21));
  int rH = max(0, h - 42);
  uint8_t d = dim ? 30 : 100;
  uint16_t cG = col(0,            (200*d)/100, (80*d)/100);
  uint16_t cA = col((220*d)/100,  (160*d)/100, 0);
  uint16_t cR = col((230*d)/100,  (40*d)/100,  (40*d)/100);
  if (gH > 0) barCanvas.fillRect(x, 64 - gH,           14, gH, cG);
  if (aH > 0) barCanvas.fillRect(x, 64 - gH - aH,      14, aH, cA);
  if (rH > 0) barCanvas.fillRect(x, 64 - gH - aH - rH, 14, rH, cR);
  // Peak-hold cap — this is what makes it read as a meter, not rectangles.
  if (!dim) {
    int ph = (int)peakH[idx];
    if (ph > h && ph <= 62) {
      int capY = 63 - ph;
      if (capY >= 0) barCanvas.fillRect(x, capY, 14, 2, 0xFFFF);
    }
  }
}

void clearVisualizer() {
  barCanvas.fillScreen(0);
  tft.drawRGBBitmap(0, 14, barCanvas.getBuffer(), 160, 64);
}

void resetVisualizer() {
  for (int i = 0; i < 10; i++) { barH[i] = 0; peakH[i] = 0; }
  audioAmp      = 0;
  hookSeen      = false;
  vizTitleDirty = true;
  vizModeLogged = false;
  lastVizMs     = 0;
  clearVisualizer();
  drawVizTitle();
}

void updateVisualizer() {
  if (appState != PLAYING) return;
  unsigned long now = millis();
  if (now - lastVizMs < 50) return;   // 20fps; faster costs SPI the decoder needs
  lastVizMs = now;

  if (vizTitleDirty) drawVizTitle();
  if (!vizModeLogged) {
    vizModeLogged = true;
    Serial.println(hookSeen ? "EQ: real audio hook active" : "EQ: synthetic mode");
  }

  float volScale = max(0.05f, displayVolume / 9.0f);
  barCanvas.fillScreen(0);

  for (int i = 0; i < 10; i++) {
    float target;
    if (hookSeen) {
      float amp   = (audioAmp / 32767.0f) * volScale;
      float phase = sinf(now / 180.0f + i * 0.75f) * 0.15f;   // offset so bars don't lockstep
      target = constrain(amp + phase, 0.0f, 1.0f);
    } else {
      if (now >= synthNextMs[i]) {
        synthTgt[i]    = (random(10, 88) / 100.0f) * volScale;
        synthNextMs[i] = now + random(120, 600);
      }
      target = synthTgt[i];
    }

    if (!isPaused) {
      float rate = (target * 62.0f > barH[i]) ? 0.5f : 0.2f;   // fast attack, slow decay
      barH[i] += (target * 62.0f - barH[i]) * rate;
      barH[i]  = constrain(barH[i], 0.0f, 62.0f);
      if (barH[i] > peakH[i]) peakH[i] = barH[i];
      else                     peakH[i] = max(0.0f, peakH[i] - 1.0f);
    }
    renderBar(i, (int)barH[i], isPaused);
  }
  // Only the bar strip goes over SPI; the title above is static.
  tft.drawRGBBitmap(0, 14, barCanvas.getBuffer(), 160, 64);
}

// ── Display ──────────────────────────────────────────────────────
void redrawDisplay() {
  if (appState == PLAYING) return;   // the visualiser owns that screen
  canvas.fillScreen(0x0000);
  switch (appState) {
    case IDLE:
      canvas.setTextColor(col(200, 200, 200));
      canvas.setTextSize(2);
      canvas.setCursor(16, 10); canvas.print("Listening");
      canvas.setTextSize(1);
      canvas.setTextColor(col(120, 120, 120));
      canvas.setCursor(20, 62); canvas.print("Waiting for work...");
      break;
    case PROCESSING:
      canvas.setTextColor(col(255, 200, 50));
      canvas.setTextSize(2);
      if (keepPlaying && !musicPending) {
        canvas.setCursor(16, 10); canvas.print("Next");
        canvas.setCursor(16, 38); canvas.print("track...");
      } else {
        canvas.setCursor(16, 10); canvas.print("Loading");
        canvas.setCursor(28, 38); canvas.print("track...");
      }
      break;
    case SPEAKING:
      canvas.setTextColor(col(100, 180, 255));
      canvas.setTextSize(2);
      canvas.setCursor(4, 10);
      canvas.print(screenText[0] ? screenText : "Speaking");
      canvas.setTextSize(1);
      canvas.setTextColor(col(150, 150, 150));
      canvas.setCursor(16, 62); canvas.print("Making your track");
      break;
    case PLAYING:
      break;
  }
  tft.drawRGBBitmap(0, 0, canvas.getBuffer(), 160, 80);
}

// End of stream is polled rather than taken from a callback: ESP32-audioI2S
// fires audio_eof_mp3 for connecttoFS but audio_eof_stream for connecttohost,
// and which one exists varies by version.
void onPlaybackFinished() {
  if (appState == SPEAKING && musicPending) {
    Serial.println("Reply finished — collecting track");
    fetchTrack = true;
    return;
  }
  if (appState == PLAYING && keepPlaying) {
    Serial.println("Keep playing — fetching next track");
    clearVisualizer();
    fetchTrack = true;
    return;
  }
  if (appState == PLAYING) clearVisualizer();
  Serial.println("Playback finished");
  appState = IDLE;
  displayDirty = true;
}

// ── Collect the generated track ──────────────────────────────────
void fetchAndPlayTrack() {
  fetchTrack   = false;
  musicPending = false;

  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("WiFi lost — cannot fetch track");
    PLAY_BEEP(BEEP_ERROR);
    appState = IDLE; displayDirty = true; return;
  }

  appState = PROCESSING;
  redrawDisplay();

  HTTPClient http;
  WiFiClient client;
  http.begin(client, String(BACKEND_URL) + "/track");
  http.setTimeout(180000);   // generation is slow; a short timeout abandons good work
  int code = http.GET();
  Serial.printf("GET /track -> HTTP %d\n", code);

  if (code != 200) {
    http.end();
    PLAY_BEEP(BEEP_ERROR);
    keepPlaying = false;     // do not spin against a dead backend
    appState = IDLE; displayDirty = true; return;
  }

  String body = http.getString();
  http.end();

  DynamicJsonDocument doc(1024);
  if (deserializeJson(doc, body)) {
    PLAY_BEEP(BEEP_ERROR);
    appState = IDLE; displayDirty = true; return;
  }

  const char* url = doc["audio_url"] | "";
  const char* scr = doc["screen"] | "";
  snprintf(screenText, sizeof(screenText), "%s", scr);
  keepPlaying = doc["keep_playing"] | false;

  if (strlen(url) == 0) {
    PLAY_BEEP(BEEP_ERROR);
    keepPlaying = false;
    appState = IDLE; displayDirty = true; return;
  }

  Serial.printf("Streaming track: %s\n", url);
  PLAY_BEEP(BEEP_TRACK);
  appState = PLAYING;
  displayDirty = true;
  resetVisualizer();
  playStartedMs = millis();
  audio.connecttohost(url);
}

// ── Poll for work ────────────────────────────────────────────────
void pollPlayback(int pollTimeout = 30000) {
  if (WiFi.status() != WL_CONNECTED) { delay(2000); return; }

  HTTPClient http;
  WiFiClient client;
  http.begin(client, String(BACKEND_URL) + "/playback?since=" + String(lastSeq));
  http.setTimeout(pollTimeout);
  int code = http.GET();

  if (code != 200) {
    http.end();
    if (code > 0) {
      Serial.printf("Poll error %d\n", code);
      PLAY_BEEP(BEEP_ERROR);
    }
    if (appState == IDLE) delay(2000);
    return;
  }

  String body = http.getString();
  http.end();

  DynamicJsonDocument doc(2048);
  if (deserializeJson(doc, body)) {
    Serial.println("Poll JSON parse error");
    return;
  }

  long seq = doc["seq"] | -1L;
  if (seq <= lastSeq) return;

  // A board that has just booted starts at lastSeq = -1 and would otherwise be
  // handed whatever was queued before it existed, replaying it as if new. That
  // is why a restart used to resume the previous request instead of idling.
  float age = doc["age_s"] | 9999.0f;
  if (age > 30.0f) {
    lastSeq = seq;
    Serial.printf("ignoring stale command (%.0fs old)\n", age);
    return;
  }
  lastSeq = seq;

  const char* action = doc["action"] | "";
  Serial.printf("Action: %s\n", action);

  // pause and resume act on the CURRENT stream, so they must not stop it first.
  if (strcmp(action, "pause") == 0) {
    if (appState == PLAYING && !isPaused) {
      audio.pauseResume(); isPaused = true; PLAY_BEEP(BEEP_PAUSE); displayDirty = true;
    }
    return;
  }
  if (strcmp(action, "resume") == 0) {
    if (appState == PLAYING && isPaused) {
      audio.pauseResume(); isPaused = false; playStartedMs = millis();
      PLAY_BEEP(BEEP_RESUME); displayDirty = true;
    }
    return;
  }

  // Everything else supersedes what is playing — that is what "play another
  // song" means, and the old track must not keep running underneath the new one.
  audio.stopSong();
  isPaused = false;
  clearVisualizer();

  if (strcmp(action, "stop") == 0) {
    keepPlaying = false;     // silence was asked for; autoplay would undo it
    appState = IDLE; displayDirty = true;
    return;
  }

  const char* scr = doc["screen"] | "";
  snprintf(screenText, sizeof(screenText), "%s", scr);
  keepPlaying  = doc["keep_playing"]  | false;
  musicPending = doc["music_pending"] | false;

  // Only obey the backend's volume when it says so; otherwise it is just its
  // copy of ours and applying it would stamp on the knob.
  if (doc["set_volume"] | false) {
    setVolume((int)((doc["volume"] | 0.6f) * 9 + 0.5f));
    encLastPos = encoder.getPosition();
  }

  const char* speechUrl = doc["speech_url"] | "";
  const char* audioUrl  = doc["audio_url"]  | "";

  PLAY_BEEP(BEEP_WORK);

  if (strlen(speechUrl) > 0) {
    Serial.printf("Speaking: %s\n", speechUrl);
    appState = SPEAKING;
    displayDirty = true;
    redrawDisplay();
    playStartedMs = millis();
    audio.connecttohost(speechUrl);
  } else if (strlen(audioUrl) > 0) {
    Serial.printf("Streaming: %s\n", audioUrl);
    PLAY_BEEP(BEEP_TRACK);
    appState = PLAYING;
    displayDirty = true;
    resetVisualizer();
    playStartedMs = millis();
    audio.connecttohost(audioUrl);
  } else if (musicPending) {
    fetchTrack = true;
  }
}

// ── Ambient ──────────────────────────────────────────────────────
void postAmbient() {
  sensors_event_t tempEv, humEv;
  dht.temperature().getEvent(&tempEv);
  dht.humidity().getEvent(&humEv);

  char body[80];
  if (isnan(tempEv.temperature) || isnan(humEv.relative_humidity))
    snprintf(body, sizeof(body), "{\"temp_c\":null,\"humidity\":null}");
  else
    snprintf(body, sizeof(body), "{\"temp_c\":%.1f,\"humidity\":%.1f}",
             tempEv.temperature, humEv.relative_humidity);

  Serial.printf("Ambient: %s\n", body);
  if (WiFi.status() != WL_CONNECTED) return;

  HTTPClient http;
  WiFiClient client;
  http.begin(client, String(BACKEND_URL) + "/ambient");
  http.addHeader("Content-Type", "application/json");
  http.setTimeout(10000);
  http.POST(body);
  http.end();
}

// ── Setup ────────────────────────────────────────────────────────
void setup() {
  Serial.begin(115200);

  mySPI.begin(SCK, MISO, MOSI);
  tft.initR(INITR_MINI160x80);
  tft.setRotation(3);
  tft.fillScreen(0x0000);
  tft.setTextColor(0xFFFF);
  tft.setTextSize(1);
  tft.setCursor(30, 36);
  tft.print("Connecting...");

  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  Serial.printf("Connecting to WiFi '%s'", WIFI_SSID);
  unsigned long t0 = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - t0 < 15000) {
    delay(500); Serial.print(".");
  }
  if (WiFi.status() == WL_CONNECTED)
    Serial.printf("\nConnected! IP: %s\n", WiFi.localIP().toString().c_str());
  else
    Serial.printf("\nWiFi failed for '%s'\n", WIFI_SSID);

  dht.begin();
  audio.setPinout(P7_IO1, P7_IO2, P7_IO0);

  strip.begin();
  strip.setBrightness(25);   // far brighter than needed indoors otherwise
  strip.show();

#if MATRIX_IDENTIFY
  for (int i = 0; i < 25; i++) {
    strip.clear();
    strip.setPixelColor(i, strip.Color(0, 120, 0));
    strip.show();
    Serial.printf("pixel %d\n", i);
    delay(300);
  }
  strip.clear(); strip.show();
#endif

#if MATRIX_DIGIT_TEST
  for (int d = 0; d <= 9; d++) {
    strip.clear();
    uint32_t colour = (d <= 3) ? strip.Color(0,180,0)
                    : (d <= 6) ? strip.Color(180,100,0) : strip.Color(180,0,0);
    const uint8_t* glyph = DIGITS[d];
    for (int row = 0; row < 5; row++)
      for (int col = 0; col < 5; col++) {
        bool lit = (col <= 2) && (glyph[row] & (0b100 >> col));
        strip.setPixelColor(pixelIndex(row, col), lit ? colour : 0);
      }
    strip.show();
    Serial.printf("digit %d\n", d);
    delay(1000);
  }
  strip.clear(); strip.show();
#endif

  pinMode(ENC_BTN, INPUT);      // active HIGH, built-in pull-down
  pinMode(BUZZER_PIN, OUTPUT);

  setVolume(5);
  encLastPos = encoder.getPosition();
  lastAmbientMs = millis() - 59000UL;   // first reading shortly after boot

  appState = IDLE;
  displayDirty = true;
  Serial.println("Ready — polling for work");
}

// ── Loop ─────────────────────────────────────────────────────────
void loop() {
  audio.loop();      // must run every pass while audio is active
  updateBeeps();

  // Volume is local and instant — it never touches the backend, and never
  // changes appState, so turning it cannot interrupt playback.
  encoder.tick();
  long encPos = encoder.getPosition();
  if (encPos != encLastPos) {
    int prevVol = displayVolume;
    setVolume(displayVolume + (int)(encPos - encLastPos));
    encLastPos = encPos;
    unsigned long nowTick = millis();
    if (nowTick - lastTickMs >= 40) {     // rate-limit or a fast spin machine-guns
      lastTickMs = nowTick;
      if (displayVolume == prevVol) PLAY_BEEP(BEEP_LIMIT);
      else                          PLAY_BEEP(BEEP_TICK);
    }
  }

  bool encBtnNow     = digitalRead(ENC_BTN);
  bool encBtnPressed = (encBtnLast == LOW && encBtnNow == HIGH);
  encBtnLast = encBtnNow;
  if (encBtnPressed && appState == PLAYING) {
    isPaused = !isPaused;
    audio.pauseResume();
    if (isPaused) PLAY_BEEP(BEEP_PAUSE);
    else        { PLAY_BEEP(BEEP_RESUME); playStartedMs = millis(); }
    displayDirty = true;
  }

  updateMatrix();
  updateVisualizer();

  if (displayDirty && millis() - lastDrawMs >= 33 && appState != PLAYING) {
    redrawDisplay();
    lastDrawMs = millis();
    displayDirty = false;
  }

  // isPaused must be excluded: isRunning() returns false while paused, which
  // would otherwise read as the track having finished.
  if ((appState == SPEAKING || appState == PLAYING) && !fetchTrack
      && !isPaused && millis() - playStartedMs > 1500 && !audio.isRunning()) {
    onPlaybackFinished();
  }

  if (fetchTrack) { fetchAndPlayTrack(); return; }

  if (appState == IDLE && millis() - lastAmbientMs >= 60000UL) {
    lastAmbientMs = millis();
    postAmbient();
  }

  // Idle: back-to-back long polls, so a command lands the instant it exists and
  // the board makes about two requests a minute. Playing: a brief check every
  // 5s, because audio.loop() needs the CPU more than the poll does.
  unsigned long pollGap     = (appState == IDLE) ? 0UL   : 5000UL;
  int           pollTimeout = (appState == IDLE) ? 30000 : 500;
  if (!fetchTrack && millis() - lastPollMs >= pollGap) {
    lastPollMs = millis();
    pollPlayback(pollTimeout);
  }
}
