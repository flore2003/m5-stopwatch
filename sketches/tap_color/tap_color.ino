// Tap the screen to fill it with a random color.
#include <M5Unified.h>

// Tear-free updates on this panel: wait for the tearing-effect (TE) pulse on
// GPIO38, which fires once per 60 Hz refresh as the scan wraps to the top,
// then finish the full-screen write well within that refresh. A streamed
// fillScreen takes ~11 ms and never tears. Pushing a full 468x466 sprite from
// PSRAM takes ~31 ms and tears in either write direction, so avoid
// full-frame sprite pushes for screen-wide changes.
static constexpr int kPinTE = 38;

static bool waitForTE(uint32_t timeoutUs = 50000) {
  uint32_t t0 = micros();
  while (digitalRead(kPinTE)) {  // let any pulse in progress finish
    if (micros() - t0 > timeoutUs) return false;
  }
  while (!digitalRead(kPinTE)) {
    if (micros() - t0 > timeoutUs) return false;
  }
  return true;
}

static void drawFrame(const char *text, const lgfx::IFont *font, uint16_t fg, uint16_t bg) {
  M5.Display.startWrite();
  waitForTE();
  M5.Display.fillScreen(bg);
  M5.Display.setTextColor(fg);
  M5.Display.setTextDatum(middle_center);
  M5.Display.setFont(font);
  M5.Display.drawString(text, M5.Display.width() / 2, M5.Display.height() / 2);
  M5.Display.endWrite();
}

static void fillRandom() {
  uint8_t r = esp_random() & 0xFF, g = esp_random() & 0xFF, b = esp_random() & 0xFF;
  // Dark text on light colors, light text on dark ones.
  bool light = (r * 299 + g * 587 + b * 114) > 128000;
  char hex[8];
  snprintf(hex, sizeof(hex), "#%02X%02X%02X", r, g, b);
  drawFrame(hex, &fonts::FreeMonoBold12pt7b, light ? TFT_BLACK : TFT_WHITE,
            M5.Display.color565(r, g, b));
  Serial.printf("color %s\n", hex);
}

void setup() {
  Serial.begin(115200);
  // Without a host reading, a full USB TX buffer would block every print.
  Serial.setTxTimeoutMs(0);
  auto cfg = M5.config();
  cfg.fallback_board = m5::board_t::board_M5StopWatch;
  M5.begin(cfg);

  // The default panel config leaves a black bar at the edge; this geometry
  // comes from ESPtember (archive/repos/esptember). Never draw rows 466-467.
  auto *panel = M5.Display.getPanel();
  auto pc = panel->config();
  pc.panel_width = 468;
  pc.panel_height = 466;
  pc.offset_x = 6;
  pc.offset_y = 0;
  panel->config(pc);
  M5.Display.setRotation(0);
  M5.Display.setBrightness(255);
  pinMode(kPinTE, INPUT);

  drawFrame("Tap me", &fonts::FreeSansBold18pt7b, TFT_WHITE, TFT_BLACK);
  Serial.printf("ready board=%d size=%dx%d touch=%d\n", int(M5.getBoard()),
                M5.Display.width(), M5.Display.height(), M5.Touch.isEnabled());
}

void loop() {
  M5.update();
  if (M5.Touch.getDetail().wasPressed()) {
    fillRandom();
    M5.Power.setVibration(120);
    delay(40);
    M5.Power.setVibration(0);
  }
  delay(10);
}
