// soundBud — voice in, generated music out.
//
// P1 ST7735 display | P2 MAX98357A amp | P3 PDM mic | P4 button
//
// Hold the button to record, release to send. The backend transcribes, decides
// what you meant, generates a track, and returns JSON with a URL. We stream the
// MP3 straight from that URL — it is never stored on the device.
//
// Press the button while music is playing to interrupt it and talk again; that
// is how follow-up commands ("make it calmer", "louder") work, and it doubles as
// echo cancellation since playback stops before the mic opens.
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

#define SAMPLE_RATE      16000
#define MAX_RECORD_SECS  8

// P1 — TFT
#define TFT_CS   P1_IO0
#define TFT_RST  P1_IO1
#define TFT_DC   P1_IO2
// P2 — amp: BCLK=P2_IO1, LRC=P2_IO2, DIN=P2_IO0
// P3 — PDM mic: SEL=P3_IO0, CLK=P3_IO2, DATA=P3_IO1
// P4 — button
#define BTN_PIN  P4_IO1

enum AppState { IDLE, RECORDING, PROCESSING, PLAYING };
AppState appState = IDLE;

SPIClass          mySPI(FSPI);
Adafruit_ST7735   tft = Adafruit_ST7735(&mySPI, TFT_CS, TFT_DC, TFT_RST);
GFXcanvas16       canvas(160, 80);
I2SClass          i2s;
Audio             audio;

File   recFile;                // open only while recording
size_t recSamples = 0;
bool   micActive  = false;

bool          btnLast      = HIGH;
bool          displayDirty = true;
unsigned long lastDrawMs   = 0;
char          screenText[24] = "";   // last `screen` field from the backend

// This panel is BGR, so swap on the way in.
uint16_t col(uint8_t r, uint8_t g, uint8_t b) { return tft.color565(b, g, r); }

// ── display ─────────────────────────────────────────────────────────────────

void redrawDisplay() {
  canvas.fillScreen(0x0000);
  switch (appState) {
    case IDLE:
      canvas.setTextColor(col(200, 200, 200));
      canvas.setTextSize(2);
      canvas.setCursor(8, 10);  canvas.print("Hold button");
      canvas.setCursor(20, 38); canvas.print("to record");
      break;
    case RECORDING:
      canvas.fillScreen(col(160, 20, 20));
      canvas.setTextColor(0xFFFF);
      canvas.setTextSize(3);
      canvas.setCursor(28, 8);  canvas.print("REC");
      canvas.setTextSize(1);
      canvas.setCursor(16, 62); canvas.print("Release to send");
      break;
    case PROCESSING:
      canvas.setTextColor(col(255, 200, 50));
      canvas.setTextSize(2);
      canvas.setCursor(16, 10); canvas.print("Sending");
      canvas.setCursor(16, 38); canvas.print("audio...");
      break;
    case PLAYING:
      canvas.setTextColor(col(50, 220, 100));
      canvas.setTextSize(2);
      canvas.setCursor(8, 20);
      // The backend sizes `screen` to 20 chars for exactly this.
      canvas.print(screenText[0] ? screenText : "Playing");
      canvas.setTextSize(1);
      canvas.setTextColor(col(90, 90, 90));
      canvas.setCursor(8, 62);  canvas.print("Hold to interrupt");
      break;
  }
  tft.drawRGBBitmap(0, 0, canvas.getBuffer(), 160, 80);
}

// ── WAV header ──────────────────────────────────────────────────────────────

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

// Sample count is unknown until the button comes up, so the header goes down as
// a placeholder and the two size fields get patched in afterwards.
void patchWavSizes(File& f, uint32_t numSamples) {
  uint32_t dataBytes = numSamples * 2;
  f.seek(4);  writeU32LE(f, 36 + dataBytes);
  f.seek(40); writeU32LE(f, dataBytes);
}

// ── mic ─────────────────────────────────────────────────────────────────────

bool startMic() {
  if (micActive) return true;
  pinMode(P3_IO0, OUTPUT);
  digitalWrite(P3_IO0, LOW);          // SEL low → left/mono slot
  i2s.setPinsPdmRx(P3_IO2, P3_IO1);   // CLK, DATA
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

// ── record ──────────────────────────────────────────────────────────────────

void beginRecording() {
  audio.stopSong();   // also stops the speaker feeding the mic
  delay(20);

  recFile = SPIFFS.open("/rec.wav", "w");
  if (!recFile) { Serial.println("Cannot open /rec.wav"); return; }
  writeWavHeader(recFile, 0);
  recSamples = 0;

  if (!startMic()) { recFile.close(); return; }

  appState = RECORDING;
  displayDirty = true;
  Serial.println("Recording...");
}

void sendAndPlay() {
  stopMic();
  patchWavSizes(recFile, recSamples);
  recFile.close();

  appState = PROCESSING;
  redrawDisplay();   // draw now; the POST below blocks for up to 3 minutes

  Serial.printf("Captured %u samples (%.1f s)\n",
                recSamples, recSamples / (float)SAMPLE_RATE);

  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("WiFi lost — skipping send");
    appState = IDLE; displayDirty = true; return;
  }

  File wavFile = SPIFFS.open("/rec.wav", "r");
  if (!wavFile) {
    Serial.println("WAV re-open failed");
    appState = IDLE; displayDirty = true; return;
  }

  HTTPClient http;
  WiFiClient client;
  http.begin(client, String(BACKEND_URL) + "/utterance");
  http.addHeader("Content-Type", "audio/wav");
  http.setTimeout(180000);   // generation is slow; a short timeout abandons good requests

  int code = http.sendRequest("POST", &wavFile, wavFile.size());
  wavFile.close();
  Serial.printf("POST /utterance -> HTTP %d\n", code);

  if (code != 200) {
    http.end();
    appState = IDLE; displayDirty = true; return;
  }

  String body = http.getString();
  http.end();

  // Grows as needed — a fixed capacity silently fails on a long `music` field.
  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, body);
  if (err) {
    Serial.printf("JSON parse error: %s\n", err.c_str());
    appState = IDLE; displayDirty = true; return;
  }

  const char* url = doc["audio_url"] | "";
  const char* scr = doc["screen"]    | "";
  int vol = (int)((doc["volume"] | 0.6f) * 21);   // backend sends 0..1, library wants 0..21
  snprintf(screenText, sizeof(screenText), "%s", scr);
  Serial.printf("screen: %s  volume: %d  url: %s\n", scr, vol, url);

  audio.setVolume(vol);

  // Null for volume and transport commands — nothing new to fetch, and the
  // volume above has already been applied to whatever is playing.
  if (strlen(url) == 0) { appState = IDLE; displayDirty = true; return; }

  appState = PLAYING;
  displayDirty = true;
  redrawDisplay();
  audio.connecttohost(url);
}

// Weak-linked by ESP32-audioI2S; fires when the stream ends.
void audio_eof_mp3(const char* info) {
  Serial.println("Playback finished");
  appState = IDLE;
  displayDirty = true;
}

// ── setup ───────────────────────────────────────────────────────────────────

void setup() {
  Serial.begin(115200);
  pinMode(BTN_PIN, INPUT);

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

  // -10025 here means the partition scheme has no SPIFFS partition to format.
  if (!SPIFFS.begin(true)) Serial.println("SPIFFS mount failed");

  audio.setPinout(P2_IO1, P2_IO2, P2_IO0);

  appState = IDLE;
  displayDirty = true;
  Serial.println("Ready — hold P4 button to record");
}

// ── loop ────────────────────────────────────────────────────────────────────

void loop() {
  audio.loop();   // must run every pass while audio is active

  if (displayDirty && millis() - lastDrawMs >= 33) {   // ~30fps ceiling
    redrawDisplay();
    lastDrawMs = millis();
    displayDirty = false;
  }

  bool btnNow   = digitalRead(BTN_PIN);
  bool pressed  = (btnLast == HIGH && btnNow == LOW);
  bool released = (btnLast == LOW  && btnNow == HIGH);
  btnLast = btnNow;

  // PLAYING is included so a follow-up command can interrupt the track.
  if (pressed && (appState == IDLE || appState == PLAYING)) beginRecording();
  if (released && appState == RECORDING) sendAndPlay();

  if (appState == RECORDING && recFile && micActive) {
    // 1KB at a time straight to flash — the whole clip never sits in RAM.
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
