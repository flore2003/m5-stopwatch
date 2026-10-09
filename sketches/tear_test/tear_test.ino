// Tearing test: wide white bars moving sideways, full screen, every refresh.
//
// A torn frame shows as a step in a bar (the part below the tear is a few
// pixels further along). KEY A cycles three modes:
// - chasing ON: the library default (every row written right behind the scan).
// - chasing OFF: still starts at TE. A direct full-frame write (~24 us/row) is
//   faster than the scan (29.7 us/row, starting ~3 ms after TE), so it stays
//   ahead and lands cleanly in the same refresh: also tear-free.
// - NO SYNC: frames start at arbitrary times (a 13 ms loop against a 16.76 ms
//   refresh). This one tears; it shows what tearing looks like here.
// KEY B cycles the speed; tapping toggles 60/30 fps.
// The label shows the mode; serial prints timing and the library's tear check
// (torn draws per second, closest call). Sending 'm' over serial = KEY A.
#include <M5Unified.h>
#include <StopWatchDisplay.h>

static constexpr int kBarW = 40;        // px per bar
static constexpr int kPeriod = 156;     // px from one bar to the next (3 bars across)
static const int kSpeeds[] = {2, 4, 8}; // px per refresh
static int speedIdx = 1;
static volatile int offset = 0;         // read by the render core during a draw
static const uint16_t kWhite = swd::rgb565(255, 255, 255), kBlack = swd::rgb565(20, 20, 30);
static swd::Overlay label;

static void render(uint16_t *dst, int x0, int y0, int w, int rows, void *) {
  // One row is enough to compute; the rest of the strip copies it.
  uint16_t *row = dst;
  int o = offset;
  for (int x = 0; x < w; x++) row[x] = ((x0 + x - o + kPeriod * 4) % kPeriod < kBarW) ? kWhite : kBlack;
  for (int r = 1; r < rows; r++) memcpy(dst + r * w, row, w * 2);
  label.composite(dst, x0, y0, w, rows);
}

static swd::StripRenderer renderer;
static swd::FramePacer pacer(1);
static volatile bool toggleScan = false, toggleRate = false, nextSpeed = false;
enum Mode { kChasing, kSyncedOnly, kNoSync };
static int mode = kChasing;

static void updateLabel(float fps) {
  char text[40];
  static const char *kNames[] = {"chasing ON", "chasing OFF", "NO SYNC"};
  snprintf(text, sizeof(text), "%s  %.0f fps  %dpx", kNames[mode], fps, kSpeeds[speedIdx]);
  auto &c = label.canvas();
  label.clear();
  c.fillRoundRect(0, 0, c.width(), c.height(), c.height() / 2,
                  mode == kChasing ? c.color565(0, 90, 40) : mode == kSyncedOnly ? c.color565(120, 90, 0)
                                                                                   : c.color565(150, 20, 20));
  c.setTextColor(TFT_WHITE);
  c.setTextDatum(middle_center);
  c.setFont(&fonts::FreeSansBold9pt7b);
  c.drawString(text, c.width() / 2, c.height() / 2 + 1);
}

static void inputTask(void *) {
  for (;;) {
    M5.update();
    if (M5.BtnA.wasPressed()) toggleScan = true;
    if (M5.BtnB.wasPressed()) nextSpeed = true;
    if (M5.Touch.getDetail().wasPressed()) toggleRate = true;
    vTaskDelay(pdMS_TO_TICKS(10));
  }
}

void setup() {
  Serial.begin(115200);
  Serial.setTxTimeoutMs(0);
  auto cfg = M5.config();
  cfg.fallback_board = m5::board_t::board_M5StopWatch;
  M5.begin(cfg);
  swd::setupDisplay();
  swd::Vsync::begin();
  if (!renderer.begin(render)) Serial.println("error: renderer init failed");
  label.begin(220, 26);
  label.setPosition((swd::kWidth - label.width()) / 2, 40);
  updateLabel(0);
  xTaskCreatePinnedToCore(inputTask, "input", 4096, nullptr, 1, nullptr, 1 - xPortGetCoreID());
  Serial.println("ready");
}

void loop() {
  static uint32_t windowStart = millis(), frames = 0, worstUs = 0, tornStart = 0;
  static int32_t minSlack = INT32_MAX;
  while (Serial.available()) {
    if (Serial.read() == 'm') toggleScan = true;
  }
  static int interval = 1;
  if (toggleScan) {
    toggleScan = false;
    mode = (mode + 1) % 3;
    renderer.setFollowScan(mode == kChasing);
  }
  if (nextSpeed) {
    nextSpeed = false;
    speedIdx = (speedIdx + 1) % 3;
  }
  if (toggleRate) {
    toggleRate = false;
    interval = 3 - interval;  // 1 <-> 2
    pacer.setInterval(interval);
  }

  if (mode == kNoSync) {
    // Free-running at ~77 Hz, so frames start all over the refresh.
    static uint32_t next = micros();
    if (int32_t(micros() - next) > 13000) next = micros();  // re-entering the mode: don't catch up
    while (int32_t(micros() - next) < 0) {
    }
    next += 13000 * interval;
  } else {
    pacer.wait();
  }
  offset = (offset + kSpeeds[speedIdx] * interval) % kPeriod;
  worstUs = max(worstUs, renderer.drawFrame());
  minSlack = min(minSlack, renderer.lastStats().slackUs);
  frames++;

  uint32_t elapsed = millis() - windowStart;
  if (elapsed >= 1000) {
    float fps = frames * 1000.0f / elapsed;
    updateLabel(fps);
    Serial.printf("fps=%.1f mode=%d frame_worst_us=%u skipped=%u torn=%u/%u min_slack_us=%d refresh=%d period_us=%u\n",
                  fps, mode, worstUs, pacer.skipped(), renderer.tornCount() - tornStart, frames, minSlack,
                  renderer.lastStats().refresh, swd::Vsync::periodUs());
    tornStart = renderer.tornCount();
    minSlack = INT32_MAX;
    windowStart = millis();
    frames = 0;
    worstUs = 0;
  }
}
