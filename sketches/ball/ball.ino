// A bouncing ball over the animated gradient, at 60 or 30 fps.
//
// The gradient needs ~24 ms on the bus for a full frame, so it can only
// update every second refresh. The ball can still move on every refresh:
//
// - 30 fps: draw the whole frame (gradient + ball) every second refresh.
// - 60 fps: the same full frame every second refresh, plus, in the refresh
//   in between, a small patch covering the ball's old and new position.
//
// Both draws chase the scan (StripRenderer's followScan): each strip is sent
// just after the panel has scanned it, so everything shows exactly one
// refresh after it was drawn, and the ball gets a new position on every
// refresh. The patch uses the same gradient state as the frame around it, so
// it blends in exactly. See lib/StopWatchDisplay for the measured timing.
//
// Dead ends on the way: alternating screen halves at 60 fps (one half showed
// a refresh late, so the ball tore where the halves met), and patching with
// a scan model that missed the panel's ~3 ms blanking before row 0 (patches
// landed a refresh early: 30 fps look plus tearing).
//
// KEY A switches between 60 and 30 fps. Tapping kicks the ball.
// Serial prints timing; the label shows how often the ball moves.
#include <M5Unified.h>
#include <GradientBackground.h>
#include <StopWatchDisplay.h>

#include "rect.h"

static swd::Gradient::State gradient;
static int theme = 1;

// ---- Ball

static constexpr int kBallR = 26;
static constexpr float kGravity = 1400;      // px/s^2, screen down
static constexpr float kRestitution = 0.88;  // energy kept per bounce
static constexpr float kRefreshS = swd::kRefreshUs / 1e6f;
static swd::Overlay ball, label;
static float px = swd::kCenterX, py = swd::kCenterY - 80, vx = 260, vy = 0;
static volatile bool kick = false;

static void drawBallSprite() {
  auto &c = ball.canvas();
  ball.clear();
  int r = kBallR;
  // No anti-aliasing (edges would blend into the overlay key color).
  c.fillCircle(r, r, r, TFT_WHITE);
  c.fillCircle(r - 2, r - 2, r - 4, c.color565(250, 250, 250));
  c.fillCircle(r - 8, r - 9, r / 3, c.color565(255, 255, 255));
  c.drawCircle(r, r, r, c.color565(40, 40, 60));
}

// Ball stays inside the visible circle; bounces reflect off its edge.
// Stepped by whole refresh periods so the motion is even on screen.
static void stepBall(float dt) {
  if (kick) {
    kick = false;
    vy -= 900;
    vx += (esp_random() & 1) ? 300 : -300;
  }
  vy += kGravity * dt;
  px += vx * dt;
  py += vy * dt;
  float limit = swd::kRadius - kBallR - 4;
  float dx = px - swd::kCenterX, dy = py - swd::kCenterY;
  float d = sqrtf(dx * dx + dy * dy);
  if (d > limit) {
    float nx = dx / d, ny = dy / d;
    px = swd::kCenterX + nx * limit;
    py = swd::kCenterY + ny * limit;
    float vn = vx * nx + vy * ny;
    if (vn > 0) {
      vx -= (1 + kRestitution) * vn * nx;
      vy -= (1 + kRestitution) * vn * ny;
    }
  }
}

static Rect placeBall() {
  ball.setPosition(int(px) - kBallR, int(py) - kBallR);
  return {int(px) - kBallR - 1, int(py) - kBallR - 1, int(px) + kBallR + 2, int(py) + kBallR + 2};
}

// ---- FPS label

static void updateLabel(float fps, bool fast) {
  char text[24];
  snprintf(text, sizeof(text), "ball %.0f fps", fps);
  auto &c = label.canvas();
  label.clear();
  c.fillRoundRect(0, 0, c.width(), c.height(), c.height() / 2, fast ? c.color565(0, 90, 40) : TFT_BLACK);
  c.setTextColor(TFT_WHITE);
  c.setTextDatum(middle_center);
  c.setFont(&fonts::FreeSansBold9pt7b);
  c.drawString(text, c.width() / 2, c.height() / 2 + 1);
}

// Runs on the render core.
static void render(uint16_t *dst, int x0, int y0, int w, int rows, void *) {
  swd::Gradient::render(gradient, dst, x0, y0, w, rows);
  ball.composite(dst, x0, y0, w, rows);
  label.composite(dst, x0, y0, w, rows);
}

static swd::StripRenderer renderer;
static volatile bool fastMode = true;
static volatile bool toggleMode = false;

static void inputTask(void *) {
  for (;;) {
    M5.update();
    if (M5.Touch.getDetail().wasPressed()) kick = true;
    if (M5.BtnA.wasPressed()) toggleMode = true;
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
  ball.begin(kBallR * 2 + 1, kBallR * 2 + 1);
  drawBallSprite();
  label.begin(120, 26);
  label.setPosition((swd::kWidth - label.width()) / 2, 34);
  updateLabel(0, fastMode);
  xTaskCreatePinnedToCore(inputTask, "input", 4096, nullptr, 1, nullptr, 1 - xPortGetCoreID());
  Serial.println("ready");
}

void loop() {
  static uint32_t nextFrameTE = swd::Vsync::count() + 1, skipped = 0;
  static uint32_t windowStart = millis(), moves = 0, worstFrameUs = 0, worstPatchUs = 0;
  static Rect prev = placeBall();

  if (toggleMode) {
    toggleMode = false;
    fastMode = !fastMode;
    Serial.printf("mode %s\n", fastMode ? "60" : "30");
  }
  bool fast = fastMode;

  // Full frame every second refresh. A patch for a ball near the bottom ends
  // shortly after the next frame's TE edge; that's fine, since the frame
  // waits ~4 ms for the scan anyway. Only a bigger overrun costs a refresh.
  uint32_t now = swd::Vsync::count();
  if (int32_t(now - nextFrameTE) > 0 || (now == nextFrameTE && micros() - swd::Vsync::lastEdgeUs() > 3000)) {
    skipped++;
    nextFrameTE = now + 1;
  }
  swd::Vsync::waitFor(nextFrameTE);
  uint32_t frameTE = nextFrameTE;
  nextFrameTE += 2;
  stepBall(fast ? kRefreshS : 2 * kRefreshS);
  prev = placeBall();
  swd::Gradient::update(gradient, swd::kGradientThemes[theme], millis() / 1000.0f);
  worstFrameUs = max(worstFrameUs, renderer.drawFrame());
  moves++;

  if (fast) {
    // The frame takes longer than one refresh, so the next TE has normally
    // passed by now; the patch chases that refresh's scan.
    swd::Vsync::waitFor(frameTE + 1);
    stepBall(kRefreshS);
    Rect cur = placeBall(), dirty = unite(prev, cur);
    prev = cur;
    worstPatchUs = max(worstPatchUs, renderer.drawRect(dirty.x0, dirty.y0, dirty.x1 - dirty.x0, dirty.y1 - dirty.y0));
    moves++;
  }

  uint32_t elapsed = millis() - windowStart;
  if (elapsed >= 1000) {
    float fps = moves * 1000.0f / elapsed;
    updateLabel(fps, fast);  // safe: the renderer is idle between draws
    Serial.printf("mode=%s ball_fps=%.1f frame_worst_us=%u patch_worst_us=%u skipped=%u\n", fast ? "60" : "30",
                  fps, worstFrameUs, worstPatchUs, skipped);
    windowStart = millis();
    moves = 0;
    worstFrameUs = worstPatchUs = 0;
  }
}
