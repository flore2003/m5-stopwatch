// Benchmark: where does the time go when pushing strips to the panel?
// Results go to serial; the screen just shows test patterns.
#include <M5Unified.h>
#include <esp_heap_caps.h>

static constexpr int W = 468, H = 466;
static constexpr int kStripRows = 32;
static uint16_t *strip;

static void report(const char *name, uint32_t us) {
  Serial.printf("bench %-40s %6u us\n", name, us);
}

static uint32_t timeStrips(int count, int w, bool waitEach) {
  uint32_t t = micros();
  M5.Display.startWrite();
  for (int i = 0; i < count; i++) {
    int y = (i * kStripRows) % H, rows = min(kStripRows, H - y);
    M5.Display.pushImageDMA((W - w) / 2 & ~1, y, w, rows, (const lgfx::swap565_t *)strip);
    if (waitEach) M5.Display.waitDisplay();
  }
  M5.Display.endWrite();
  return micros() - t;
}

void setup() {
  Serial.begin(115200);
  Serial.setTxTimeoutMs(0);
  auto cfg = M5.config();
  cfg.fallback_board = m5::board_t::board_M5StopWatch;
  M5.begin(cfg);
  auto *panel = M5.Display.getPanel();
  auto pc = panel->config();
  pc.panel_width = 468; pc.panel_height = 466; pc.offset_x = 6; pc.offset_y = 0;
  panel->config(pc);
  M5.Display.setRotation(0);

  strip = (uint16_t *)heap_caps_malloc(W * kStripRows * 2, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
  for (int i = 0; i < W * kStripRows; i++) strip[i] = 0x1F00;
  delay(2000);

  uint32_t t = micros(); M5.Display.startWrite(); M5.Display.endWrite();
  report("empty startWrite/endWrite", micros() - t);
  report("1 strip x468", timeStrips(1, W, false));
  report("2 strips x468", timeStrips(2, W, false));
  report("15 strips x468", timeStrips(15, W, false));
  report("15 strips x468, waitDisplay each", timeStrips(15, W, true));
  report("15 strips x392 (84%)", timeStrips(15, 392, false));
  report("15 strips x234 (50%)", timeStrips(15, 234, false));
  report("30 strips x234 (same pixels as 15x468)", timeStrips(30, 234, false));
  t = micros(); M5.Display.startWrite(); M5.Display.fillRect(0, 0, W, H, TFT_NAVY); M5.Display.endWrite();
  report("fillRect full", micros() - t);
  t = micros(); M5.Display.startWrite(); M5.Display.fillRect(0, 0, W / 2, H, TFT_NAVY); M5.Display.endWrite();
  report("fillRect half width", micros() - t);
  Serial.println("bench done");
}

void loop() { delay(1000); }
