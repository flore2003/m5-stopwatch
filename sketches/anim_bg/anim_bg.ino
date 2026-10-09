// Animated full-screen gradient, tear-free at 60 fps, with an fps indicator.
//
// Rendering, pacing, the gradient and the panel quirks live in
// lib/StopWatchDisplay; this sketch wires them together.
//
// Tap the screen to switch color theme; KEY A toggles circle-only spans.
// Serial prints frame timing.
#include <M5Unified.h>
#include <GradientBackground.h>
#include <StopWatchDisplay.h>

static swd::Gradient::State gradient;
static volatile int theme = 0;
static swd::Overlay label;

static void updateLabel(float fps) {
  char text[16];
  snprintf(text, sizeof(text), "%.0f fps", fps);
  auto &c = label.canvas();
  label.clear();
  c.fillRoundRect(0, 0, c.width(), c.height(), c.height() / 2, TFT_BLACK);
  c.setTextColor(TFT_WHITE);
  c.setTextDatum(middle_center);
  c.setFont(&fonts::FreeSansBold9pt7b);
  c.drawString(text, c.width() / 2, c.height() / 2 + 1);
}

// Runs on the render core.
static void render(uint16_t *dst, int x0, int y0, int w, int rows, void *) {
  swd::Gradient::render(gradient, dst, x0, y0, w, rows);
  label.composite(dst, x0, y0, w, rows);
}

static swd::StripRenderer renderer;
static swd::FramePacer pacer(1);  // every refresh: 60 fps
static volatile bool toggleCircle = false;

// Input runs on the render core at low priority, so touch I2C reads never
// delay the start of a frame.
static void inputTask(void *) {
  for (;;) {
    M5.update();
    if (M5.Touch.getDetail().wasPressed()) {
      theme = (theme + 1) % swd::kGradientThemeCount;
      Serial.printf("theme %d\n", theme);
    }
    if (M5.BtnA.wasPressed()) toggleCircle = true;
    vTaskDelay(pdMS_TO_TICKS(10));
  }
}

void setup() {
  Serial.begin(115200);
  Serial.setTxTimeoutMs(0);  // never block on a full USB buffer
  auto cfg = M5.config();
  cfg.fallback_board = m5::board_t::board_M5StopWatch;
  M5.begin(cfg);
  swd::setupDisplay();
  swd::Vsync::begin();

  if (!renderer.begin(render)) Serial.println("error: renderer init failed");
  label.begin(84, 26);
  label.setPosition((swd::kWidth - label.width()) / 2, 34);
  updateLabel(0);
  xTaskCreatePinnedToCore(inputTask, "input", 4096, nullptr, 1, nullptr, 1 - xPortGetCoreID());
  Serial.println("ready");
}

void loop() {
  static uint32_t windowStart = millis(), windowFrames = 0, worstUs = 0;

  if (toggleCircle) {
    toggleCircle = false;
    renderer.setCircleOnly(!renderer.circleOnly());
  }
  swd::Gradient::update(gradient, swd::kGradientThemes[theme], millis() / 1000.0f);

  pacer.wait();
  uint32_t us = renderer.drawFrame();
  if (us > worstUs) worstUs = us;
  windowFrames++;

  uint32_t elapsed = millis() - windowStart;
  if (elapsed >= 1000) {
    float fps = windowFrames * 1000.0f / elapsed;
    updateLabel(fps);  // safe: the renderer is idle between frames
    auto &st = renderer.lastStats();
    Serial.printf("fps=%.1f frame_worst_us=%u skipped=%u circle_only=%d | last: render=%u wait_render=%u flush=%u\n",
                  fps, worstUs, pacer.skipped(), renderer.circleOnly(), st.render, st.waitRender, st.flush);
    windowStart = millis();
    windowFrames = 0;
    worstUs = 0;
  }
}
