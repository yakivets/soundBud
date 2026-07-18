// soundBud — voice in, generated music out.
//
// P2 ST7735 display | P8 MAX98357A amp | P5 PDM mic | P3 button
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

// P2 — TFT
#define TFT_CS   P2_IO0
#define TFT_RST  P2_IO1
#define TFT_DC   P2_IO2
// P8 — amp: BCLK=P8_IO1, LRC=P8_IO2, DIN=P8_IO0
// P5 — PDM mic: SEL=P5_IO0, CLK=P5_IO2, DATA=P5_IO1
// P3 — button
#define BTN_PIN  P3_IO1
#define MIC_SEL  P5_IO0
#define MIC_CLK  P5_IO2
#define MIC_DATA P5_IO1
#define AMP_BCLK P8_IO1
#define AMP_LRC  P8_IO2
#define AMP_DIN  P8_IO0

enum AppState { IDLE, RECORDING, PROCESSING, SPEAKING, PLAYING };
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
bool          musicPending = false;  // backend is generating a track for us
bool          fetchTrack   = false;  // set from the audio callback, acted on in loop()

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
    case SPEAKING:
      canvas.setTextColor(col(120, 180, 255));
      canvas.setTextSize(2);
      canvas.setCursor(8, 20);
      canvas.print(screenText[0] ? screenText : "...");
      canvas.setTextSize(1);
      canvas.setTextColor(col(90, 90, 90));
      canvas.setCursor(8, 62);  canvas.print("Making your track");
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
  pinMode(MIC_SEL, OUTPUT);
  digitalWrite(MIC_SEL, LOW);          // SEL low → left/mono slot
  i2s.setPinsPdmRx(MIC_CLK, MIC_DATA);   // CLK, DATA
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

  const char* speech = doc["speech_url"] | "";
  const char* scr    = doc["screen"]     | "";
  musicPending = doc["music_pending"] | false;
  int vol = (int)((doc["volume"] | 0.6f) * 21);   // backend sends 0..1, library wants 0..21
  snprintf(screenText, sizeof(screenText), "%s", scr);
  Serial.printf("screen: %s  volume: %d  pending: %d\n", scr, vol, musicPending);

  audio.setVolume(vol);

  // The spoken reply comes back in a couple of seconds while the music is still
  // generating, so the user hears an answer instead of waiting in silence.
  if (strlen(speech) > 0) {
    appState = SPEAKING;
    displayDirty = true;
    redrawDisplay();
    audio.connecttohost(speech);
    return;
  }

  // No voice (TTS failed, or a volume/transport command with nothing to say).
  if (musicPending) { fetchTrack = true; return; }
  appState = IDLE; displayDirty = true;
}

// Ask the backend for the track it has been generating since the POST. Blocks
// until it is ready, which by now is usually immediate.
void fetchAndPlayTrack() {
  fetchTrack = false;
  musicPending = false;

  appState = PROCESSING;
  redrawDisplay();

  HTTPClient http;
  WiFiClient client;
  http.begin(client, String(BACKEND_URL) + "/track");
  http.setTimeout(180000);

  int code = http.GET();
  if (code != 200) {
    http.end();
    Serial.printf("GET /track -> HTTP %d\n", code);
    appState = IDLE; displayDirty = true; return;
  }

  String body = http.getString();
  http.end();

  JsonDocument doc;
  if (deserializeJson(doc, body)) {
    Serial.println("track JSON parse error");
    appState = IDLE; displayDirty = true; return;
  }

  const char* url = doc["audio_url"] | "";
  const char* scr = doc["screen"]    | "";
  if (scr[0]) snprintf(screenText, sizeof(screenText), "%s", scr);
  Serial.printf("track: %s\n", url);

  if (strlen(url) == 0) { appState = IDLE; displayDirty = true; return; }

  appState = PLAYING;
  displayDirty = true;
  redrawDisplay();
  audio.connecttohost(url);
}

// Weak-linked by ESP32-audioI2S; fires when a stream ends. Runs in the audio
// path, so it only sets a flag — the HTTP call happens back in loop().
void audio_eof_mp3(const char* info) {
  if (appState == SPEAKING && musicPending) {
    fetchTrack = true;      // reply finished; go collect the music
    return;
  }
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

  audio.setPinout(AMP_BCLK, AMP_LRC, AMP_DIN);

  appState = IDLE;
  displayDirty = true;
  Serial.println("Ready — hold the button to record");
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

  // Collect the music once the spoken reply has finished.
  if (fetchTrack) fetchAndPlayTrack();

  // PLAYING and SPEAKING are included so a follow-up command can interrupt.
  if (pressed && (appState == IDLE || appState == PLAYING || appState == SPEAKING)) {
    musicPending = false;   // abandon whatever was queued; a new request supersedes it
    fetchTrack = false;
    beginRecording();
  }
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
