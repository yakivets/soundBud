// soundBud — voice in, generated music out.
//
//   P1 rotary encoder | P2 ST7735 display | P3 button
//   P4 NeoPixel 5x5   | P5 PDM mic        | P8 MAX98357A amp
//
// Hold the button to record, release to send. Then two things come back:
//
//   1. A spoken reply, within a couple of seconds. The backend voices Claude's
//      answer while the music is still generating, so the device talks back
//      instead of going silent for fifteen seconds.
//   2. The track itself. When the reply finishes playing we GET /track, which
//      blocks until generation is done — usually no wait at all by then.
//
// Press the button during either one to interrupt and talk again; that is how
// follow-up commands ("make it calmer", "louder") work, and it doubles as echo
// cancellation since playback stops before the mic opens.
//
// The encoder sets volume locally with no network round trip — a knob has to
// respond instantly, and going through the backend would put seconds on it.
// The NeoPixel matrix shows the level as a digit, or a pause glyph.
//
// While a track plays the TFT shows a 10-bar equaliser. It uses real audio
// amplitude if this version of ESP32-audioI2S exposes audio_process_i2s, and
// falls back to a synthetic animation if not — the serial log says which.
//
// Copy secrets.h.example to secrets.h before building.
// Board: ESP32-S3. Set PSRAM to "OPI PSRAM" and pick a partition scheme that
// includes SPIFFS, or recording has nowhere to write.

#include "secrets.h"
#include <WiFi.h>
#include <HTTPClient.h>
#include <FS.h>
#include <SPIFFS.h>
#include <SPI.h>
#include <Adafruit_GFX.h>
#include <Adafruit_ST7735.h>
#include <ESP_I2S.h>
#include <Audio.h>
#include <ArduinoJson.h>
#include <RotaryEncoder.h>
#include <Adafruit_NeoPixel.h>

// ── Config ────────────────────────────────────────────────────
#define SAMPLE_RATE      16000
#define MAX_RECORD_SECS  8

// ── Pins ──────────────────────────────────────────────────────
// P2 — TFT
#define TFT_CS   P2_IO0
#define TFT_RST  P2_IO1
#define TFT_DC   P2_IO2
// P8 — Audio Amp: BCLK=P8_IO1, LRC=P8_IO2, DIN=P8_IO0
// P5 — PDM Mic: SEL=P5_IO0, CLK=P5_IO2, DATA=P5_IO1
// P3 — Button (IO1 per module spec)
#define BTN_PIN  P3_IO1
// P1 — Rotary Encoder: BTN=IO0, CLK=IO1, DT=IO2
#define ENC_BTN  P1_IO0
#define ENC_CLK  P1_IO1
#define ENC_DT   P1_IO2
// P4 — NeoPixel Matrix 5×5: DATA=IO1
#define MATRIX_PIN P4_IO1

// ── State machine ─────────────────────────────────────────────
enum AppState { IDLE, RECORDING, PROCESSING, SPEAKING, PLAYING };
AppState appState = IDLE;

// ── Globals ───────────────────────────────────────────────────
SPIClass          mySPI(FSPI);
Adafruit_ST7735   tft = Adafruit_ST7735(&mySPI, TFT_CS, TFT_DC, TFT_RST);
GFXcanvas16       canvas(160, 80);
GFXcanvas16       barCanvas(160, 64);   // bar region only — see updateVisualizer()
I2SClass          i2s;
Audio             audio;

File   recFile;                // open during recording, closed on release
size_t recSamples = 0;
bool   micActive  = false;

char          screenText[24] = "";
unsigned long playStartedMs  = 0;
bool          musicPending  = false;
bool          fetchTrack    = false;
bool          btnLast      = HIGH;
bool          displayDirty = true;
unsigned long lastDrawMs   = 0;

RotaryEncoder encoder(ENC_CLK, ENC_DT, RotaryEncoder::LatchMode::TWO03);
Adafruit_NeoPixel strip(25, MATRIX_PIN, NEO_GRB + NEO_KHZ800);
int           displayVolume   = 5;
long          encLastPos      = 0;
unsigned long volChangedMs    = 0;
int           matrixLastLevel = -1;  // -1=blank, 0-9=digit, 10=pause glyph
bool          isPaused        = false;
bool          encBtnLast      = LOW;

// ── Equaliser visualiser globals ──────────────────────────────
float            barH[10]        = {};
float            peakH[10]       = {};
float            synthTgt[10]    = {};
unsigned long    synthNextMs[10] = {};
unsigned long    lastVizMs       = 0;
bool             vizTitleDirty   = true;
bool             vizModeLogged   = false;
volatile int16_t audioAmp        = 0;
bool             hookSeen        = false;

// ── Volume / Matrix ─────────────────────────────────────────
// 3×5 digit font — low 3 bits per row, bit2 = leftmost column
const uint8_t PAUSE_GLYPH[5]  = {0b01010, 0b01010, 0b01010, 0b01010, 0b01010};
const uint8_t DIGITS[10][5] = {
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

// Row-major: row*5+col. If digits zigzag on alternate rows, switch to serpentine:
// row*5 + (row%2 ? 4-col : col)
int pixelIndex(int row, int col) { return row * 5 + col; }

void setVolume(int v) {
  v = max(0, min(9, v));             // 0..9 display scale
  displayVolume = v;
  audio.setVolume((v * 21 + 4) / 9); // map to 0..21 for the library
  volChangedMs = millis();
}

void updateMatrix() {
  // Desired state: 10=pause glyph, 0-9=digit, -1=blank
  int desired;
  bool volRecent = (millis() - volChangedMs <= 3000);
  if (isPaused && !volRecent) {
    desired = 10;           // pause glyph stays until resumed
  } else if (volRecent) {
    desired = displayVolume; // digit for 3 s after any knob turn
  } else {
    desired = -1;            // blank
  }

  if (desired == matrixLastLevel) return;  // nothing changed — skip show()
  matrixLastLevel = desired;

  if (desired == -1) {
    for (int i = 0; i < 25; i++) strip.setPixelColor(i, 0);
  } else if (desired == 10) {
    uint32_t c = strip.Color(180, 100, 0);  // amber pause icon
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
        bool lit = (col >= 1 && col <= 3) && (glyph[row] & (0b100 >> (col - 1)));
        strip.setPixelColor(pixelIndex(row, col), lit ? colour : 0);
      }
  }
  strip.show();
}

// ── Equaliser visualiser ────────────────────────────────────
// audio_process_i2s is a weak-linked user callback in ESP32-audioI2S.
// If the installed version exposes it, it fires per decoded I2S block.
// If not, the function is unreferenced (no error), hookSeen stays false,
// and the synthetic animation runs instead.
void audio_process_i2s(int16_t* buff, uint16_t n, uint8_t /*bps*/, bool* /*cont*/) {
  int32_t pk = 0;
  for (uint16_t i = 0; i < n; i++) {
    int32_t s = abs((int32_t)buff[i]);
    if (s > pk) pk = s;
  }
  audioAmp = (int16_t)min((int32_t)32767, pk);
  hookSeen = true;
}

void drawVizTitle() {
  tft.fillRect(0, 0, 160, 14, 0x0000);
  tft.setTextColor(col(0, 150, 80));
  tft.setTextSize(1);
  tft.setCursor(4, 3);
  tft.print(screenText[0] ? screenText : "Playing");
  vizTitleDirty = false;
}

// Render one bar (14 px wide) into barCanvas at column idx*16.
// h = bar height 0..62, dim = paused (30% brightness).
void renderBar(int idx, int h, bool dim) {
  int x  = idx * 16;
  int gH = min(h, 21);
  int aH = max(0, min(h - 21, 21));
  int rH = max(0, h - 42);
  uint8_t d = dim ? 30 : 100;
  uint16_t cG = col(0,            (200*d)/100, (80*d)/100);
  uint16_t cA = col((220*d)/100,  (160*d)/100, 0);
  uint16_t cR = col((230*d)/100,  (40*d)/100,  (40*d)/100);
  if (gH > 0) barCanvas.fillRect(x, 64 - gH,            14, gH, cG);
  if (aH > 0) barCanvas.fillRect(x, 64 - gH - aH,       14, aH, cA);
  if (rH > 0) barCanvas.fillRect(x, 64 - gH - aH - rH,  14, rH, cR);
  // Peak-hold cap — 2 px white line just above the bar. This is what makes it
  // read as a level meter rather than random rectangles.
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
  // 20fps. Faster looks no better and costs SPI time the decoder needs.
  if (now - lastVizMs < 50) return;
  lastVizMs = now;

  if (vizTitleDirty) drawVizTitle();

  // Log EQ mode once (audio.loop() runs before us so hook has had a chance to fire)
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

  // Only the 160x64 bar strip goes over SPI; the title above is static.
  tft.drawRGBBitmap(0, 14, barCanvas.getBuffer(), 160, 64);
}

// ── Color helper (panel is BGR) ───────────────────────────────
uint16_t col(uint8_t r, uint8_t g, uint8_t b) {
  return tft.color565(b, g, r);
}

// ── Display ───────────────────────────────────────────────────
void redrawDisplay() {
  if (appState == PLAYING) return;  // visualiser owns the PLAYING screen
  canvas.fillScreen(0x0000);
  switch (appState) {
    case IDLE:
      canvas.setTextColor(col(200, 200, 200));
      canvas.setTextSize(2);
      canvas.setCursor(8, 10);
      canvas.print("Hold button");
      canvas.setCursor(20, 38);
      canvas.print("to record");
      break;
    case RECORDING:
      canvas.fillScreen(col(160, 20, 20));
      canvas.setTextColor(0xFFFF);
      canvas.setTextSize(3);
      canvas.setCursor(28, 8);
      canvas.print("REC");
      canvas.setTextSize(1);
      canvas.setCursor(16, 62);
      canvas.print("Release to send");
      break;
    case PROCESSING:
      canvas.setTextColor(col(255, 200, 50));
      canvas.setTextSize(2);
      canvas.setCursor(16, 10);
      canvas.print("Sending");
      canvas.setCursor(16, 38);
      canvas.print("audio...");
      break;
    case SPEAKING:
      canvas.setTextColor(col(100, 180, 255));
      canvas.setTextSize(2);
      canvas.setCursor(4, 10);
      canvas.print(screenText[0] ? screenText : "Speaking");
      canvas.setTextSize(1);
      canvas.setTextColor(col(150, 150, 150));
      canvas.setCursor(16, 62);
      canvas.print("Making your track");
      break;
    case PLAYING:
      canvas.setTextColor(col(50, 220, 100));
      canvas.setTextSize(2);
      canvas.setCursor(4, 10);
      canvas.print(screenText[0] ? screenText : "Playing");
      canvas.setTextSize(1);
      canvas.setTextColor(col(150, 150, 150));
      canvas.setCursor(10, 62);
      canvas.print("Hold to interrupt");
      break;
  }
  tft.drawRGBBitmap(0, 0, canvas.getBuffer(), 160, 80);
}

// ── WAV header ────────────────────────────────────────────────
void writeU16LE(File& f, uint16_t v) { f.write((uint8_t*)&v, 2); }
void writeU32LE(File& f, uint32_t v) { f.write((uint8_t*)&v, 4); }

void writeWavHeader(File& f, uint32_t numSamples) {
  uint32_t dataBytes = numSamples * 2;
  f.write((const uint8_t*)"RIFF", 4);  writeU32LE(f, 36 + dataBytes);
  f.write((const uint8_t*)"WAVE", 4);
  f.write((const uint8_t*)"fmt ", 4);  writeU32LE(f, 16);
  writeU16LE(f, 1);                    // PCM
  writeU16LE(f, 1);                    // mono
  writeU32LE(f, SAMPLE_RATE);
  writeU32LE(f, SAMPLE_RATE * 2);      // byte rate
  writeU16LE(f, 2);                    // block align
  writeU16LE(f, 16);                   // bits per sample
  f.write((const uint8_t*)"data", 4);  writeU32LE(f, dataBytes);
}

// Seek back to patch the two size fields after recording completes
void patchWavSizes(File& f, uint32_t numSamples) {
  uint32_t dataBytes = numSamples * 2;
  f.seek(4);  writeU32LE(f, 36 + dataBytes);
  f.seek(40); writeU32LE(f, dataBytes);
}

// ── Mic lifecycle ─────────────────────────────────────────────
bool startMic() {
  if (micActive) return true;
  pinMode(P5_IO0, OUTPUT);
  digitalWrite(P5_IO0, LOW);           // SEL low → left/mono slot
  i2s.setPinsPdmRx(P5_IO2, P5_IO1);   // CLK, DATA
  if (!i2s.begin(I2S_MODE_PDM_RX, SAMPLE_RATE,
                 I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_MONO)) {
    Serial.println("Mic init failed");
    return false;
  }
  micActive = true;
  return true;
}

void stopMic() {
  if (!micActive) return;
  i2s.end();
  micActive = false;
}

// ── Record ────────────────────────────────────────────────────
void beginRecording() {
  audio.stopSong();
  isPaused = false;
  delay(20);

  recFile = SPIFFS.open("/rec.wav", "w");
  if (!recFile) { Serial.println("Cannot open /rec.wav"); return; }
  writeWavHeader(recFile, 0);   // placeholder sizes — patched on release
  recSamples = 0;

  if (!startMic()) { recFile.close(); return; }

  appState = RECORDING;
  displayDirty = true;
  Serial.println("Recording...");
}

// ── Send WAV → parse JSON → stream MP3 ───────────────────────
void sendAndPlay() {
  stopMic();

  // Patch sizes in the open file, then close
  patchWavSizes(recFile, recSamples);
  recFile.close();

  appState = PROCESSING;
  redrawDisplay();   // immediate — we're about to block

  Serial.printf("Captured %u samples (%.1f s)\n",
                recSamples, recSamples / (float)SAMPLE_RATE);

  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("WiFi lost — skipping send");
    appState = IDLE; displayDirty = true; return;
  }

  File wavFile = SPIFFS.open("/rec.wav", "r");
  if (!wavFile) { Serial.println("WAV re-open failed"); appState = IDLE; displayDirty = true; return; }

  HTTPClient http;
  WiFiClient client;
  // volume_now tells the backend where the knob actually is, so "turn it down"
  // is computed from reality rather than its own stale copy.
  String url = String(BACKEND_URL) + "/utterance?volume_now="
               + String(displayVolume / 9.0f, 2);
  http.begin(client, url);
  http.addHeader("Content-Type", "audio/wav");
  http.setTimeout(180000);   // 3 min

  int code = http.sendRequest("POST", &wavFile, wavFile.size());
  wavFile.close();
  Serial.printf("POST /utterance → HTTP %d\n", code);

  if (code == 200) {
    String body = http.getString();
    http.end();

    DynamicJsonDocument doc(4096);
    DeserializationError err = deserializeJson(doc, body);
    if (err) {
      Serial.printf("JSON parse error: %s\n", err.c_str());
      appState = IDLE; displayDirty = true; return;
    }

    const char* speechUrl  = doc["speech_url"] | "";
    bool         pending     = doc["music_pending"] | false;
    const char*  scr         = doc["screen"] | "";
    snprintf(screenText, sizeof(screenText), "%s", scr);
    musicPending = pending;
    // Only obey the backend's volume when the user actually asked for one;
    // otherwise it is just its copy of ours and would stamp on the knob.
    if (doc["set_volume"] | false) {
      setVolume((int)((doc["volume"] | 0.6f) * 9 + 0.5f));
      encLastPos = encoder.getPosition();   // resync knob to the new level
    }
    Serial.printf("speech_url: %s  music_pending: %d  screen: %s  vol: %d\n",
                  speechUrl, pending, screenText, displayVolume);

    if (strlen(speechUrl) > 0) {
      appState = SPEAKING;
      displayDirty = true;
      redrawDisplay();
      playStartedMs = millis();
      audio.connecttohost(speechUrl);
    } else if (musicPending) {
      // TTS failed but music is coming — queue the fetch immediately
      fetchTrack = true;
      appState = SPEAKING;
      displayDirty = true;
    } else {
      Serial.println("No speech_url and no music_pending");
      appState = IDLE; displayDirty = true;
    }
  } else {
    http.end();
    Serial.printf("API error %d\n", code);
    appState = IDLE; displayDirty = true;
  }
}

// ── Playback-finished handler (polled via isRunning()) ──────────
// ESP32-audioI2S fires audio_eof_mp3 for connecttoFS but audio_eof_stream for
// connecttohost, and which exists varies by version — polling sidesteps both.
void onPlaybackFinished() {
  if (appState == SPEAKING && musicPending) {
    Serial.println("Reply finished — collecting track");
    fetchTrack = true;
    return;
  }
  if (appState == PLAYING) clearVisualizer();
  Serial.println("Playback finished");
  appState = IDLE;
  displayDirty = true;
}

// ── Fetch music track after speech finishes ───────────────────
void fetchAndPlayTrack() {
  fetchTrack   = false;
  musicPending = false;

  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("WiFi lost — cannot fetch track");
    appState = IDLE; displayDirty = true; return;
  }

  HTTPClient http;
  WiFiClient client;
  http.begin(client, String(BACKEND_URL) + "/track");
  http.setTimeout(180000);
  int code = http.GET();
  Serial.printf("GET /track → HTTP %d\n", code);

  if (code != 200) {
    http.end();
    Serial.printf("Track fetch error %d\n", code);
    appState = IDLE; displayDirty = true; return;
  }

  String body = http.getString();
  http.end();

  DynamicJsonDocument doc(1024);
  DeserializationError err = deserializeJson(doc, body);
  if (err) {
    Serial.printf("Track JSON error: %s\n", err.c_str());
    appState = IDLE; displayDirty = true; return;
  }

  const char* url = doc["audio_url"] | "";
  const char* scr = doc["screen"] | "";
  snprintf(screenText, sizeof(screenText), "%s", scr);

  if (strlen(url) == 0) {
    Serial.println("No audio_url in /track response");
    appState = IDLE; displayDirty = true; return;
  }

  Serial.printf("Streaming track: %s\n", url);
  appState = PLAYING;
  displayDirty = true;
  resetVisualizer();
  playStartedMs = millis();
  audio.connecttohost(url);
}

// ── Setup ─────────────────────────────────────────────────────
void setup() {
  Serial.begin(115200);

  pinMode(BTN_PIN, INPUT);

  // Display — show "Connecting…" immediately
  mySPI.begin(SCK, MISO, MOSI);
  tft.initR(INITR_MINI160x80);
  tft.setRotation(3);
  tft.fillScreen(0x0000);
  tft.setTextColor(0xFFFF);
  tft.setTextSize(1);
  tft.setCursor(30, 36);
  tft.print("Connecting...");

  // WiFi
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

  // SPIFFS — -10025 here means the partition scheme has no SPIFFS partition.
  if (!SPIFFS.begin(true)) Serial.println("SPIFFS mount failed");

  // Audio amp
  audio.setPinout(P8_IO1, P8_IO2, P8_IO0);

  // NeoPixel matrix — low brightness, clear on start
  strip.begin();
  strip.setBrightness(25);
  strip.show();

  // Encoder button (active HIGH, built-in pull-down)
  pinMode(ENC_BTN, INPUT);

  setVolume(5);    // sets audio + displayVolume + queues initial matrix draw
  encLastPos = encoder.getPosition();

  appState = IDLE;
  displayDirty = true;
  Serial.println("Ready — hold the button to record");
}

// ── Loop ──────────────────────────────────────────────────────
void loop() {
  audio.loop();   // must run every pass while audio is active

  // Rotary encoder — volume (never interrupts playback state)
  encoder.tick();
  long encPos = encoder.getPosition();
  if (encPos != encLastPos) {
    setVolume(displayVolume + (int)(encPos - encLastPos));
    encLastPos = encPos;
  }

  // Encoder button — pause/resume while PLAYING
  bool encBtnNow     = digitalRead(ENC_BTN);
  bool encBtnPressed = (encBtnLast == LOW && encBtnNow == HIGH);
  encBtnLast = encBtnNow;
  if (encBtnPressed && appState == PLAYING) {
    isPaused = !isPaused;
    audio.pauseResume();
    if (!isPaused) playStartedMs = millis();  // reset grace period on resume
    displayDirty = true;
  }

  // Matrix — self-guarding; show() only fires on state change
  updateMatrix();

  // Visualiser — self-throttled to 50 ms; never routed through displayDirty
  updateVisualizer();

  // Display throttle ~30 fps (skipped while visualiser runs)
  if (displayDirty && millis() - lastDrawMs >= 33 && appState != PLAYING) {
    redrawDisplay();
    lastDrawMs = millis();
    displayDirty = false;
  }

  // Poll for stream end. isPaused must be excluded: isRunning() returns false
  // while paused, which would otherwise read as the track having finished.
  if ((appState == SPEAKING || appState == PLAYING) && !fetchTrack
      && !isPaused && millis() - playStartedMs > 1500 && !audio.isRunning()) {
    onPlaybackFinished();
  }

  // Deferred track fetch (safe to block here, not on audio path)
  if (fetchTrack) fetchAndPlayTrack();

  // Button edge detection
  bool btnNow  = digitalRead(BTN_PIN);
  bool pressed  = (btnLast == HIGH && btnNow == LOW);
  bool released = (btnLast == LOW  && btnNow == HIGH);
  btnLast = btnNow;

  if (pressed  && (appState == IDLE || appState == PLAYING || appState == SPEAKING)) {
    musicPending = false;
    fetchTrack   = false;
    beginRecording();
  }
  if (released && appState == RECORDING) sendAndPlay();

  // Stream PDM → SPIFFS while recording (1 KB at a time)
  if (appState == RECORDING && recFile && micActive) {
    int16_t tmp[512];
    size_t got = i2s.readBytes((char*)tmp, sizeof(tmp));
    if (got > 0) {
      recFile.write((uint8_t*)tmp, got);
      recSamples += got / 2;
      if (recSamples >= (size_t)MAX_RECORD_SECS * SAMPLE_RATE) {
        Serial.println("Max length reached, sending...");
        sendAndPlay();
      }
    }
  }
}
