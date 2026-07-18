// soundBud — step 1 hardware check.
//
// Proves button + I2S mic + PSRAM buffer + IPS screen all work, with no
// network and no API key. Hold the button, speak, release.
//
//   idle       "READY / hold button"
//   recording  live peak-level bar + elapsed seconds
//   released   duration, bytes captured, peak level
//
// The level bar is the point: it is the only way to see that the mic is
// actually capturing sound rather than silently returning zeros.
//
// Board: Axiometa PIXIE M1 (ESP32-S3, 4MB flash / 2MB PSRAM).
// Arduino IDE: select an ESP32-S3 board, and set PSRAM to "OPI PSRAM".
// Libraries: Adafruit_ST7735, Adafruit_GFX. (ESP_I2S ships with core 3.x.)

#include <Adafruit_GFX.h>
#include <Adafruit_ST7735.h>
#include <ESP_I2S.h>

// ─────────────────────────────────────────────────────────────────────────
// SET THESE. Pin numbers come from PIN_MTA0007.png on the PIXIE M1 product
// page; the display driver is stamped on the LCD module itself.
// Everything below this block is board-agnostic.
// ─────────────────────────────────────────────────────────────────────────

// Push-to-talk button. The PIXIE M1 has an onboard User button — use its
// GPIO here and you need no external wiring to test.
constexpr int PIN_BUTTON = 0;  // active low, internal pullup

// IPS LCD, SPI.
constexpr int PIN_TFT_CS   = 10;
constexpr int PIN_TFT_DC   = 9;
constexpr int PIN_TFT_RST  = 8;
constexpr int PIN_TFT_SCLK = 12;
constexpr int PIN_TFT_MOSI = 11;

// I2S digital mic (INMP441 / ICS-43434). Tie the mic's L/R pin to GND for
// the left channel, which is what SLOT_MODE_MONO reads.
constexpr int PIN_I2S_BCLK = 4;  // mic SCK
constexpr int PIN_I2S_WS   = 5;  // mic WS / LRCL
constexpr int PIN_I2S_DIN  = 6;  // mic SD

// 0.96" IPS is usually 160x80 ST7735S. If text is offset or mirrored, the
// initR() tab constant in setup() is the thing to change first.
constexpr int SCREEN_W = 160;
constexpr int SCREEN_H = 80;

// ─────────────────────────────────────────────────────────────────────────

constexpr uint32_t SAMPLE_RATE   = 16000;  // what speech-to-text wants
constexpr uint32_t MAX_SECONDS   = 10;
constexpr size_t   BUFFER_BYTES  = SAMPLE_RATE * 2 * MAX_SECONDS;  // 320KB
constexpr uint32_t DEBOUNCE_MS   = 40;

Adafruit_ST7735 tft(PIN_TFT_CS, PIN_TFT_DC, PIN_TFT_RST);
I2SClass i2s;

int16_t *recording = nullptr;  // PSRAM
size_t   recordedBytes = 0;
bool     isRecording = false;
uint32_t recordStartMs = 0;
int16_t  peak = 0;

// Redraw only what changed — the full-screen clear is slow enough to stutter
// the level bar if done every frame.
int lastBarWidth = -1;
int lastSeconds  = -1;

void showMessage(const char *line1, const char *line2, uint16_t colour) {
  tft.fillScreen(ST77XX_BLACK);
  tft.setTextColor(colour);
  tft.setTextSize(2);
  tft.setCursor(6, 16);
  tft.print(line1);
  if (line2) {
    tft.setTextSize(1);
    tft.setCursor(6, 46);
    tft.print(line2);
  }
}

void showFatal(const char *what) {
  showMessage("ERROR", what, ST77XX_RED);
  Serial.printf("FATAL: %s\n", what);
  while (true) delay(1000);
}

void setup() {
  Serial.begin(115200);

  pinMode(PIN_BUTTON, INPUT_PULLUP);

  SPI.begin(PIN_TFT_SCLK, -1, PIN_TFT_MOSI, PIN_TFT_CS);
  tft.initR(INITR_MINI160x80);  // wrong colours or offset? try INITR_BLACKTAB
  tft.setRotation(1);
  tft.fillScreen(ST77XX_BLACK);

  // PSRAM must be enabled in the board menu, or this returns null and we'd
  // otherwise crash on the first sample write.
  recording = (int16_t *)ps_malloc(BUFFER_BYTES);
  if (!recording) showFatal("no PSRAM - enable\nit in board menu");

  i2s.setPins(PIN_I2S_BCLK, PIN_I2S_WS, -1, PIN_I2S_DIN, -1);
  if (!i2s.begin(I2S_MODE_STD, SAMPLE_RATE, I2S_DATA_BIT_WIDTH_16BIT,
                 I2S_SLOT_MODE_MONO)) {
    showFatal("i2s mic failed\ncheck wiring");
  }

  Serial.printf("ready: %u byte buffer, %u s max\n",
                (unsigned)BUFFER_BYTES, (unsigned)MAX_SECONDS);
  showMessage("READY", "hold button to talk", ST77XX_WHITE);
}

// Reads whatever the mic has ready, appends it to the PSRAM buffer, and
// returns the loudest sample seen in this chunk.
int16_t captureChunk() {
  static int16_t chunk[512];

  size_t room = BUFFER_BYTES - recordedBytes;
  if (room == 0) return 0;

  size_t want = min(sizeof(chunk), room);
  size_t got  = i2s.readBytes((char *)chunk, want);
  if (got == 0) return 0;

  memcpy((uint8_t *)recording + recordedBytes, chunk, got);
  recordedBytes += got;

  int16_t chunkPeak = 0;
  for (size_t i = 0; i < got / 2; i++) {
    int16_t v = chunk[i] < 0 ? -chunk[i] : chunk[i];  // abs, INT16_MIN-safe enough
    if (v > chunkPeak) chunkPeak = v;
  }
  return chunkPeak;
}

void drawRecordingScreen(int16_t level, uint32_t elapsedMs) {
  int barWidth = map(level, 0, 8000, 0, SCREEN_W - 12);
  barWidth = constrain(barWidth, 0, SCREEN_W - 12);
  int seconds = elapsedMs / 1000;

  if (seconds != lastSeconds) {
    tft.fillRect(0, 8, SCREEN_W, 22, ST77XX_BLACK);
    tft.setTextColor(ST77XX_RED);
    tft.setTextSize(2);
    tft.setCursor(6, 10);
    tft.printf("REC %ds", seconds);
    lastSeconds = seconds;
  }

  if (barWidth != lastBarWidth) {
    tft.fillRect(6, 44, SCREEN_W - 12, 14, ST77XX_BLACK);
    tft.fillRect(6, 44, barWidth, 14, ST77XX_GREEN);
    lastBarWidth = barWidth;
  }
}

void loop() {
  static bool     lastPressed = false;
  static uint32_t lastChangeMs = 0;

  bool pressed = digitalRead(PIN_BUTTON) == LOW;

  if (pressed != lastPressed && millis() - lastChangeMs > DEBOUNCE_MS) {
    lastChangeMs = millis();
    lastPressed = pressed;

    if (pressed) {
      recordedBytes = 0;
      peak = 0;
      isRecording = true;
      recordStartMs = millis();
      lastBarWidth = -1;
      lastSeconds = -1;
      tft.fillScreen(ST77XX_BLACK);
    } else if (isRecording) {
      isRecording = false;
      uint32_t ms = millis() - recordStartMs;

      char summary[64];
      snprintf(summary, sizeof(summary), "%.1fs  %uKB\npeak %d",
               ms / 1000.0f, (unsigned)(recordedBytes / 1024), peak);
      // Silence almost always means wiring, not a quiet room.
      showMessage(peak > 200 ? "GOT IT" : "SILENT", summary,
                  peak > 200 ? ST77XX_GREEN : ST77XX_YELLOW);

      Serial.printf("captured %u bytes in %u ms, peak %d\n",
                    (unsigned)recordedBytes, (unsigned)ms, peak);
    }
  }

  if (isRecording) {
    int16_t level = captureChunk();
    if (level > peak) peak = level;
    drawRecordingScreen(level, millis() - recordStartMs);

    if (recordedBytes >= BUFFER_BYTES) {
      isRecording = false;
      showMessage("FULL", "10s limit reached", ST77XX_YELLOW);
    }
  }
}
