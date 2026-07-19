// soundBud remote — the thing you hold.
//
//   P1 button | P2 passive buzzer | P3 ST7735 display | P4 PDM mic
//
// Hold the button, speak, release. The clip is POSTed to /utterance and the
// reply is shown on screen. There is deliberately no speaker here — the base
// board owns playback and learns about the work by polling the backend. The two
// boards never talk to each other.
//
// Between requests it polls /playback for status so the screen can show what the
// base is doing. That poll is rate-limited with a short timeout on purpose: the
// button must stay responsive, and status does not need to be fresh. Polling on
// every pass is what previously made the button feel dead.
//
// Copy secrets.h.example to secrets.h before building.

#include "secrets.h"
#include <WiFi.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <SPIFFS.h>
#include <SPI.h>
#include <Adafruit_GFX.h>
#include <Adafruit_ST7735.h>
#include <ESP_I2S.h>

// ── Pins ───────────────────────────────────────────────────────────────────────
#define BTN_PIN   P1_IO1   // Push button (P1) — LOW when pressed, hardware debounce
#define BUZ_PIN   P2_IO1   // Passive buzzer (P2)
#define TFT_CS    P3_IO0   // ST7735 display (P3, FSPI bus)
#define TFT_RST   P3_IO1
#define TFT_DC    P3_IO2
#define MIC_SEL   P4_IO0   // PDM mic (P4): SEL low = mono/left slot
#define MIC_DATA  P4_IO1   // setPinsPdmRx takes CLK then DATA
#define MIC_CLK   P4_IO2

#define WAV_PATH    "/rec.wav"
#define SAMPLE_RATE 16000

// ── Display ────────────────────────────────────────────────────────────────────
SPIClass         mySPI(FSPI);
Adafruit_ST7735  tft = Adafruit_ST7735(&mySPI, TFT_CS, TFT_DC, TFT_RST);
GFXcanvas16      canvas(160, 80);

// BGR panel: R and B are swapped, so col() takes (r,g,b) and fixes it on the way in.
inline uint16_t col(uint8_t r, uint8_t g, uint8_t b) { return tft.color565(b, g, r); }
const uint16_t C_BG    = 0x0000;
const uint16_t C_DIM   = col(100, 100, 100);
const uint16_t C_WHITE = col(255, 255, 255);
const uint16_t C_CYAN  = col(  0, 255, 255);
const uint16_t C_RED   = col(255,   0,   0);
const uint16_t C_BLUE  = col( 30, 144, 255);
const uint16_t C_GREEN = col(  0, 200,  80);
const uint16_t C_AMBER = col(255, 160,   0);

I2SClass i2s;

// REPLY and SCREEN are resting states. The button works from all three — it
// originally only worked from IDLE, which meant the second press did nothing.
enum AppState { IDLE, RECORDING, SENDING, REPLY, SCREEN };
AppState state = IDLE;

bool btnLast = HIGH;

File     wavFile;
uint32_t recBytes   = 0;
double   volSumSq   = 0.0;
uint32_t volSamples = 0;

String dispSay    = "";
String dispScreen = "";

// Mirrored from the base, for the idle screen only.
int    lastSeq     = -1;
String statusState = "idle";
String nowPlaying  = "";
bool   keepPlaying = false;

unsigned long replyStart   = 0;
bool          displayDirty = true;
unsigned long lastDraw     = 0;

const unsigned long DRAW_MS  = 33UL;    // ~30fps
const unsigned long REPLY_MS = 5000UL;  // show `say`, then fall back to `screen`

void drawScreen();
void wordWrap(GFXcanvas16 &c, const String &text, int x, int y, int lineChars, uint16_t colour);
void startRecording();
void stopRecording();
void sendRecording();
void writeWavHeader(File &f, uint32_t dataSz);
void pollPlayback();

// ── Setup ──────────────────────────────────────────────────────────────────────
void setup() {
  Serial.begin(115200);
  Serial.println("\n=== Voice remote booting ===");

  mySPI.begin(SCK, MISO, MOSI);
  tft.initR(INITR_MINI160x80);
  tft.setRotation(3);
  canvas.fillScreen(C_BG);
  canvas.setTextColor(C_DIM);
  canvas.setTextSize(1);
  canvas.setCursor(4, 36);
  canvas.print("Connecting...");
  tft.drawRGBBitmap(0, 0, canvas.getBuffer(), 160, 80);

  pinMode(BTN_PIN, INPUT);        // module has its own pull-up and debounce
  pinMode(BUZ_PIN, OUTPUT);
  digitalWrite(BUZ_PIN, LOW);
  pinMode(MIC_SEL, OUTPUT);
  digitalWrite(MIC_SEL, LOW);     // SEL low = mono slot; must precede i2s.begin

  if (!SPIFFS.begin(true)) Serial.println("SPIFFS mount failed");

  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  Serial.printf("Connecting to '%s'", WIFI_SSID);
  unsigned long t0 = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - t0 < 15000) {
    delay(500); Serial.print(".");
  }
  if (WiFi.status() == WL_CONNECTED)
    Serial.printf("\nConnected! IP: %s\n", WiFi.localIP().toString().c_str());
  else
    Serial.println("\nWiFi failed — POSTs will retry when reconnected.");

  state = IDLE;
  displayDirty = true;
  Serial.println("Ready — hold button to record.");
}

// ── Loop ───────────────────────────────────────────────────────────────────────
void loop() {
  unsigned long now = millis();

  // Button first, every pass — nothing below is allowed to delay it.
  bool btnNow = digitalRead(BTN_PIN);
  if (btnNow != btnLast) {
    btnLast = btnNow;
    bool canRecord = (state == IDLE || state == SCREEN || state == REPLY);
    if (btnNow == LOW && canRecord) {
      startRecording();
    } else if (btnNow == HIGH && state == RECORDING) {
      stopRecording();
      sendRecording();   // blocks on HTTP; state -> REPLY or IDLE on return
    }
  }

  // Status poll: only at rest, every 5s, short timeout. Polling harder gains
  // nothing — the screen is the only consumer — and every call blocks the loop.
  static unsigned long lastPollMs = 0;
  bool resting = (state == IDLE || state == SCREEN);
  if (resting && now >= 3000 && now - lastPollMs >= 5000) {
    lastPollMs = now;
    pollPlayback();
  }

  // Drain the mic into SPIFFS in 1KB chunks — the clip never sits in RAM.
  if (state == RECORDING) {
    int16_t buf[512];
    size_t n = i2s.readBytes((char*)buf, sizeof(buf));
    if (n > 0 && wavFile) {
      wavFile.write((uint8_t*)buf, n);
      recBytes += n;
      int samps = (int)(n / 2);
      for (int i = 0; i < samps; i++) volSumSq += (double)buf[i] * buf[i];
      volSamples += samps;
    }
  }

  if (state == REPLY && now - replyStart >= REPLY_MS) {
    state = SCREEN;
    displayDirty = true;
  }

  if (displayDirty && now - lastDraw >= DRAW_MS) {
    drawScreen();
    lastDraw = now;
    displayDirty = false;
  }
}

// ── Recording ──────────────────────────────────────────────────────────────────
void startRecording() {
  recBytes = 0; volSumSq = 0.0; volSamples = 0;

  i2s.setPinsPdmRx(MIC_CLK, MIC_DATA);   // CLK first, DATA second
  if (!i2s.begin(I2S_MODE_PDM_RX, SAMPLE_RATE, I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_MONO)) {
    Serial.println("PDM mic init failed — not recording");
    return;
  }

  wavFile = SPIFFS.open(WAV_PATH, FILE_WRITE);
  if (!wavFile) {
    Serial.println("SPIFFS open failed");
    i2s.end();
    return;
  }
  writeWavHeader(wavFile, 0);   // sizes unknown until release; patched on stop

  state = RECORDING;
  displayDirty = true;
  tone(BUZ_PIN, 880, 80);
  Serial.println("Recording...");
}

void stopRecording() {
  i2s.end();
  if (wavFile) {
    wavFile.seek(0);
    writeWavHeader(wavFile, recBytes);
    wavFile.close();
  }
  tone(BUZ_PIN, 1047, 60);
  Serial.printf("Recorded %u bytes (%.1f s)\n", recBytes, (float)recBytes / (SAMPLE_RATE * 2.0f));
}

void sendRecording() {
  state = SENDING;
  displayDirty = true;
  drawScreen();   // draw before the POST blocks, or the screen lies for 5 seconds

  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("WiFi not connected — discarding recording");
    state = IDLE; displayDirty = true; return;
  }

  float volNow = (volSamples > 0)
    ? constrain((float)(sqrt(volSumSq / (double)volSamples) / 32767.0), 0.0f, 1.0f)
    : 0.0f;

  File f = SPIFFS.open(WAV_PATH, FILE_READ);
  if (!f) {
    Serial.println("WAV read failed");
    state = IDLE; displayDirty = true; return;
  }

  WiFiClient client;
  HTTPClient http;
  String url = String(BACKEND_URL) + "/utterance?volume_now=" + String(volNow, 3);
  Serial.printf("POST %s (%u bytes)\n", url.c_str(), (unsigned)f.size());
  http.begin(client, url);
  http.addHeader("Content-Type", "audio/wav");
  int code = http.sendRequest("POST", &f, f.size());
  f.close();

  if (code == 200) {
    String body = http.getString();
    DynamicJsonDocument resp(2048);
    DeserializationError err = deserializeJson(resp, body);
    if (!err) {
      dispSay    = String(resp["say"]    | "");
      dispScreen = String(resp["screen"] | "");
      Serial.printf("say: %s\nscreen: %s\n", dispSay.c_str(), dispScreen.c_str());
      // speech_url / audio_url / music_pending are for the base, not for us.
    } else {
      Serial.printf("JSON parse failed: %s\n", err.c_str());
    }
    state = REPLY;
    replyStart = millis();
  } else {
    Serial.printf("POST failed (HTTP %d) — ready for next press\n", code);
    state = IDLE;
  }
  http.end();
  displayDirty = true;
}

// 44-byte WAV header, 16-bit mono PCM.
void writeWavHeader(File &f, uint32_t dataSz) {
  uint32_t sr = SAMPLE_RATE;
  uint32_t br = SAMPLE_RATE * 2;
  uint32_t fz = 36 + dataSz;
  uint32_t u;  uint16_t v;
  f.write((uint8_t*)"RIFF", 4);  f.write((uint8_t*)&fz, 4);
  f.write((uint8_t*)"WAVE", 4);  f.write((uint8_t*)"fmt ", 4);
  u = 16; f.write((uint8_t*)&u, 4);
  v =  1; f.write((uint8_t*)&v, 2);   // PCM
  v =  1; f.write((uint8_t*)&v, 2);   // mono
  f.write((uint8_t*)&sr, 4);
  f.write((uint8_t*)&br, 4);
  v =  2; f.write((uint8_t*)&v, 2);   // block align
  v = 16; f.write((uint8_t*)&v, 2);   // bits per sample
  f.write((uint8_t*)"data", 4);  f.write((uint8_t*)&dataSz, 4);
}

// Word-wrap at size 1: 6px per char, 10px line pitch.
void wordWrap(GFXcanvas16 &c, const String &text, int x, int y, int lineChars, uint16_t colour) {
  c.setTextSize(1);
  c.setTextColor(colour);
  int cy = y, start = 0, len = text.length();
  while (start < len && cy < 76) {
    int avail = len - start;
    if (avail <= lineChars) {
      c.setCursor(x, cy); c.print(text.substring(start)); break;
    }
    int sp = text.lastIndexOf(' ', start + lineChars - 1);
    int end = (sp > start) ? sp : start + lineChars;
    c.setCursor(x, cy); c.print(text.substring(start, end));
    start = end + ((end < len && text[end] == ' ') ? 1 : 0);
    cy += 10;
  }
}

// ── Status poll ────────────────────────────────────────────────────────────────
// since=-1 always: we want current state, not a change notification, so the
// backend answers immediately instead of long-polling. Long-polling here would
// block the button.
void pollPlayback() {
  if (WiFi.status() != WL_CONNECTED) return;
  WiFiClient client;
  HTTPClient http;
  http.begin(client, String(BACKEND_URL) + "/playback?since=-1");
  http.setTimeout(800);
  int code = http.GET();
  if (code == 200) {
    String body = http.getString();
    DynamicJsonDocument doc(512);
    if (!deserializeJson(doc, body)) {
      int seq = doc["seq"] | -1;
      if (seq > lastSeq) {
        lastSeq     = seq;
        statusState = String(doc["state"]       | "idle");
        nowPlaying  = String(doc["now_playing"] | "");
        keepPlaying = doc["keep_playing"]       | false;
        displayDirty = true;
      }
    }
  }
  http.end();
}

// ── Screens ────────────────────────────────────────────────────────────────────
void drawScreen() {
  canvas.fillScreen(C_BG);
  switch (state) {

    case IDLE: {
      uint16_t stCol   = C_DIM;
      String   stLabel = "Ready";
      if      (statusState == "speaking") { stCol = C_BLUE;  stLabel = "Speaking"; }
      else if (statusState == "playing")  { stCol = C_GREEN; stLabel = "Playing";  }
      canvas.setTextSize(1); canvas.setTextColor(stCol);
      canvas.setCursor(2, 2); canvas.print(stLabel);
      canvas.drawFastHLine(0, 11, 160, C_DIM);

      if (nowPlaying.length()) {
        canvas.setTextColor(C_WHITE); canvas.setTextSize(2);
        int np_len = (int)nowPlaying.length();
        if (np_len <= 13) {                        // size-2 char is 12px wide
          canvas.setCursor(max(2, 80 - np_len * 6), 26);
          canvas.print(nowPlaying);
        } else {
          int sp = nowPlaying.lastIndexOf(' ', 12);
          String l1 = (sp > 0) ? nowPlaying.substring(0, sp)  : nowPlaying.substring(0, 13);
          String l2 = (sp > 0) ? nowPlaying.substring(sp + 1) : nowPlaying.substring(13);
          if ((int)l2.length() > 13) l2 = l2.substring(0, 12) + ".";
          canvas.setCursor(max(2, 80 - (int)l1.length() * 6), 20); canvas.print(l1);
          canvas.setCursor(max(2, 80 - (int)l2.length() * 6), 36); canvas.print(l2);
        }
      } else {
        canvas.setTextColor(C_DIM); canvas.setTextSize(1);
        canvas.setCursor(2, 32); canvas.print("Nothing playing");
      }

      if (keepPlaying) {
        canvas.setTextColor(C_AMBER); canvas.setTextSize(1);
        canvas.setCursor(2, 62); canvas.print("AUTO");
      }
      canvas.setTextColor(C_DIM); canvas.setTextSize(1);
      canvas.setCursor(2, 70); canvas.print("Hold to talk");
      break;
    }

    case RECORDING:
      canvas.fillScreen(C_RED);
      canvas.setTextColor(C_WHITE); canvas.setTextSize(3);
      canvas.setCursor(10, 16); canvas.print("REC");
      canvas.setTextSize(1);
      canvas.setCursor(2, 68); canvas.print("Release to send");
      break;

    case SENDING:
      canvas.setTextColor(C_DIM); canvas.setTextSize(1);
      canvas.setCursor(2, 36); canvas.print("Thinking...");
      break;

    case REPLY:
      wordWrap(canvas, dispSay, 2, 4, 26, C_WHITE);
      break;

    case SCREEN:
      if (dispScreen.length()) {
        canvas.setTextColor(C_WHITE); canvas.setTextSize(2);
        canvas.setCursor(2, 32);
        canvas.print(dispScreen.substring(0, 13));
      } else {
        canvas.setTextColor(C_DIM); canvas.setTextSize(1);
        canvas.setCursor(2, 36); canvas.print("Hold to talk");
      }
      break;
  }
  tft.drawRGBBitmap(0, 0, canvas.getBuffer(), 160, 80);
}
