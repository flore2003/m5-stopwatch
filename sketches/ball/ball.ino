// A bouncing ball over the animated gradient, on swd::Engine.
//
// The engine redraws only what layers invalidate. KEY A switches the
// gradient between updating every refresh (full frames at 60 fps) and every
// second refresh (30 fps background). In the second mode the engine draws a
// full frame when the gradient changes and just the ball's area in between,
// so the ball still moves at 60 fps; the gradient renders those patches with
// its unchanged state, so they blend in. Tapping kicks the ball.
//
// The stats pill shows fps and the renderer's tear count. Serial prints
// timing; sending 'm' over serial = KEY A.
#include <M5Unified.h>
#include <Layers.h>

static constexpr int kBallR = 26;
static constexpr float kGravity = 1400;      // px/s^2, screen down
static constexpr float kRestitution = 0.88;  // energy kept per bounce
static volatile bool kick = false, toggleMode = false;

class Ball : public swd::SpriteLayer {
 public:
  bool begin() {
    if (!SpriteLayer::begin(kBallR * 2 + 1, kBallR * 2 + 1)) return false;
    auto &c = canvas();
    clear();
    int r = kBallR;
    // No anti-aliasing (edges would blend into the transparent key color).
    c.fillCircle(r, r, r, TFT_WHITE);
    c.fillCircle(r - 8, r - 9, r / 3, c.color565(255, 255, 255));
    c.drawCircle(r, r, r, c.color565(40, 40, 60));
    place();
    return true;
  }

  // Physics in update(): dt is whole refreshes, so motion is even on screen.
  void update(const swd::FrameInfo &f) override {
    float dt = f.dt;
    if (kick) {
      kick = false;
      vy_ -= 900;
      vx_ += (esp_random() & 1) ? 300 : -300;
    }
    vy_ += kGravity * dt;
    px_ += vx_ * dt;
    py_ += vy_ * dt;
    // Stay inside the visible circle; bounce off its edge.
    float limit = swd::kRadius - kBallR - 4;
    float dx = px_ - swd::kCenterX, dy = py_ - swd::kCenterY, d = sqrtf(dx * dx + dy * dy);
    if (d > limit) {
      float nx = dx / d, ny = dy / d;
      px_ = swd::kCenterX + nx * limit;
      py_ = swd::kCenterY + ny * limit;
      float vn = vx_ * nx + vy_ * ny;
      if (vn > 0) {
        vx_ -= (1 + kRestitution) * vn * nx;
        vy_ -= (1 + kRestitution) * vn * ny;
      }
    }
    place();
  }

 private:
  void place() { moveTo(int(px_) - kBallR, int(py_) - kBallR); }
  float px_ = swd::kCenterX, py_ = swd::kCenterY - 80, vx_ = 260, vy_ = 0;
};

static swd::Engine engine;
static swd::GradientLayer background;
static Ball ball;
static swd::StatsLayer stats(engine);

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
  if (!engine.begin()) Serial.println("error: engine init failed");
  background.setTheme(swd::kGradientThemes[1]);
  ball.begin();
  stats.begin();
  engine.add(background);
  engine.add(ball);
  engine.add(stats);
  xTaskCreatePinnedToCore(inputTask, "input", 4096, nullptr, 1, nullptr, 1 - xPortGetCoreID());
  Serial.println("ready");
}

void loop() {
  static int interval = 1;
  static uint32_t windowStart = millis(), frames = 0, fullFrames = 0, worstUs = 0, tornStart = 0;
  static int32_t minSlack = INT32_MAX;
  while (Serial.available()) {
    if (Serial.read() == 'm') toggleMode = true;
  }
  if (toggleMode) {
    toggleMode = false;
    interval = 3 - interval;  // 1 <-> 2
    background.setInterval(interval);
  }

  engine.frame();
  const auto &f = engine.lastFrame();
  frames++;
  if (f.full) fullFrames++;
  worstUs = max(worstUs, f.drawUs);
  if (f.regions) minSlack = min(minSlack, f.slackUs);

  uint32_t elapsed = millis() - windowStart;
  if (elapsed >= 1000) {
    uint32_t torn = engine.renderer().tornCount();
    Serial.printf("gradient_every=%d fps=%.1f full=%u patches=%u worst_draw_us=%u torn=%u min_slack_us=%d skipped=%u\n",
                  interval, frames * 1000.0f / elapsed, fullFrames, frames - fullFrames, worstUs, torn - tornStart,
                  minSlack, engine.skipped());
    windowStart = millis();
    frames = fullFrames = worstUs = 0;
    tornStart = torn;
    minSlack = INT32_MAX;
  }
}
