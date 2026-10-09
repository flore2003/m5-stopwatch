// Animated full-screen gradient at 60 fps, on swd::Engine.
//
// Tap the screen to switch color theme. The stats pill shows fps and the
// renderer's tear count; serial prints timing.
#include <M5Unified.h>
#include <Layers.h>

static swd::Engine engine;
static swd::GradientLayer background;
static swd::StatsLayer stats(engine);
static volatile bool nextTheme = false;

static void inputTask(void *) {
  for (;;) {
    M5.update();
    if (M5.Touch.getDetail().wasPressed()) nextTheme = true;
    vTaskDelay(pdMS_TO_TICKS(10));
  }
}

void setup() {
  Serial.begin(115200);
  Serial.setTxTimeoutMs(0);  // never block on a full USB buffer
  auto cfg = M5.config();
  cfg.fallback_board = m5::board_t::board_M5StopWatch;
  M5.begin(cfg);
  if (!engine.begin()) Serial.println("error: engine init failed");
  stats.begin();
  engine.add(background);
  engine.add(stats);
  xTaskCreatePinnedToCore(inputTask, "input", 4096, nullptr, 1, nullptr, 1 - xPortGetCoreID());
  Serial.println("ready");
}

void loop() {
  static int theme = 0;
  static uint32_t windowStart = millis(), frames = 0, worstUs = 0;
  if (nextTheme) {
    nextTheme = false;
    theme = (theme + 1) % swd::kGradientThemeCount;
    background.setTheme(swd::kGradientThemes[theme]);
  }

  engine.frame();
  frames++;
  worstUs = max(worstUs, engine.lastFrame().drawUs);

  uint32_t elapsed = millis() - windowStart;
  if (elapsed >= 1000) {
    auto &st = engine.renderer().lastStats();
    Serial.printf("fps=%.1f worst_draw_us=%u torn=%u skipped=%u | last: render=%u wait_render=%u wait_scan=%u\n",
                  frames * 1000.0f / elapsed, worstUs, engine.renderer().tornCount(), engine.skipped(), st.render,
                  st.waitRender, st.waitScan);
    windowStart = millis();
    frames = worstUs = 0;
  }
}
