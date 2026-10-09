// Focus: a ring timer for the round display.
//
// Drag around the rim to set 1-60 minutes (a haptic click per minute), tap
// the center to start, pause or dismiss; KEY A does the same, KEY B rewinds
// to the set time. The arc winds down as time passes and warms up in the
// last minute; when it closes, particles burst out from the ring.
//
// Engine notes: the dial (glow, track, dim ticks) is drawn once into an
// ImageLayer; everything that changes is a vector item, and only the areas
// that change are redrawn. The progress arc is 60 prebuilt one-minute
// segments that switch on and off, plus a tip for the partial minute, so
// dragging, rewinding and the countdown only redraw what moved. Paused, ready and done states don't animate, so the engine sleeps.
//
// Serial test hooks: 'a' = KEY A, 'b' = KEY B, 'x' = 3 s left.
#include <M5Unified.h>
#include <Layers.h>
#include <Text.h>
#include <esp_timer.h>

using swd::Shape;

static constexpr float kCX = swd::kCenterX, kCY = swd::kCenterY;
static constexpr float kRingR = 188, kRingW = 26;  // progress ring centerline and width
static constexpr uint32_t kTrack = 0x161C2A, kTickDim = 0x434C60, kTickLit = 0xE8ECF4;
static constexpr uint32_t kText = 0xF2F4F8, kMuted = 0x8C95A8, kDim = 0x5A6478;
static const swd::ColorStop kCool[] = {{0, 0x22D3EE}, {0.55f, 0x6366F1}, {1, 0xA855F7}};
static const swd::ColorStop kWarm[] = {{0, 0xFFC371}, {0.6f, 0xFF7A45}, {1, 0xFF4D6D}};

static swd::Engine engine;
static swd::ImageLayer dial;

static void polar(float deg, float r, float &x, float &y) {
  float a = deg * PI / 180;
  x = kCX + sinf(a) * r, y = kCY - cosf(a) * r;
}

static float easeOutCubic(float t) {
  t = 1 - t;
  return 1 - t * t * t;
}

static void drawDial() {
  dial.radialGradient(0x1C2436, 0x04060A, swd::kRadius + 20);
  swd::ShapeLayer shapes;  // scratch layer, painted in batches
  shapes.add(Shape::ring(kCX, kCY, kRingR, kRingW, kTrack));
  dial.paint(shapes);
  shapes.clear();
  for (int m = 0; m < 60; m++) {
    float ax, ay, bx, by;
    bool five = m % 5 == 0;
    polar(m * 6, five ? 146 : 151, ax, ay);
    polar(m * 6, 160, bx, by);
    shapes.add(Shape::capsule(ax, ay, bx, by, five ? 1.6f : 0.7f, five ? kTickDim : 0x262D3C));
    if (shapes.count() == swd::ShapeLayer::kMaxShapes) {
      dial.paint(shapes);
      shapes.clear();
    }
  }
  dial.paint(shapes);
}

class Focus : public swd::VectorLayer {
 public:
  enum State { Empty, Ready, Running, Paused, Done };

  void begin() {
    for (int i = 0; i < 12; i++) {
      float ax, ay, bx, by;
      polar(i * 30, 146, ax, ay);
      polar(i * 30, 160, bx, by);
      ticks_[i].set(Shape::capsule(ax, ay, bx, by, 1.6f, kTickLit));
      ticks_[i].setVisible(false);
      add(ticks_[i]);
    }
    breath_.setVisible(false);
    add(breath_);
    // Butt-ended segments overlapping their neighbors by 0.6 degrees: no
    // seams, and far fewer edges than round caps. The arc's start gets a
    // round cap of its own (startCap_).
    for (int i = 0; i < 60; i++) {
      segPaths_[i].arc(kCX, kCY, kRingR, max(0.0f, i * 6 - 0.6f), (i + 1) * 6 + 0.6f);
      segs_[i].setPath(segPaths_[i]);
      segs_[i].noFill();
      segs_[i].setVisible(false);
      add(segs_[i]);
    }
    tip_.noFill();
    tip_.setVisible(false);
    add(tip_);
    startCap_.setVisible(false);
    add(startCap_);
    setPalette(false);
    knobShadow_.set(Shape::circle(0, 0, 15, 0x000000, 100));
    knob_.set(Shape::circle(0, 0, 13, 0xFFFFFF));
    knobDot_.set(Shape::circle(0, 0, 4.5f, kCool[1].color));
    add(knobShadow_), add(knob_), add(knobDot_);
    for (auto &p : particles_) {
      p.item.setVisible(false);
      add(p.item);
    }

    digits_.setFont(swd::fonts::InterSemiBold);
    digits_.setSize(96);
    digits_.setAlign(swd::TextItem::Center);
    digits_.setBaseline(swd::TextItem::Middle);
    digits_.setTabularNumbers(true);
    digits_.setPosition(kCX, kCY - 8);
    add(digits_);
    caption_.setFont(swd::fonts::InterRegular);
    caption_.setSize(17);
    caption_.setColor(kMuted);
    caption_.setAlign(swd::TextItem::Center);
    caption_.setPosition(kCX, kCY + 62);
    add(caption_);

    setDuration(25 * 60);
    showArc(150);
    enter(Ready);
  }

  bool animating() const override {
    return dragging_ || state_ == Running || tween_.active || burstLeft_ > 0;
  }

  bool onEvent(const swd::Event &e) override {
    float dx = e.x - kCX, dy = e.y - kCY, r = sqrtf(dx * dx + dy * dy);
    float angle = fmodf(atan2f(dx, -dy) * 180 / PI + 360, 360);  // clockwise from 12
    switch (e.type) {
      case swd::Event::TouchDown:
        if (r > 120 && state_ != Running) {
          dragging_ = true;
          tween_.active = false;
          dragAngle_ = angle, lastTouch_ = angle;
          lastMinute_ = minuteAt(dragAngle_);
          caption("release to set");
          setPalette(false);
          showDrag();
          swd::Haptics::click();
        }
        return true;
      case swd::Event::TouchMove:
        if (dragging_) {
          float d = angle - lastTouch_;
          if (d > 180) d -= 360;
          if (d < -180) d += 360;
          // Clamp at 0 and 60 minutes instead of wrapping around.
          dragAngle_ = constrain(dragAngle_ + d, 0.0f, 360.0f);
          lastTouch_ = angle;
          showDrag();
        }
        return true;
      case swd::Event::TouchUp:
        if (dragging_) {
          dragging_ = false;
          int minutes = minuteAt(dragAngle_);
          setDuration(minutes * 60);
          animateArc(minutes * 6, 160);  // settle onto the whole minute
          enter(minutes ? Ready : Empty);
        }
        return true;
      case swd::Event::Tap:
        if (r < 120) primary();
        return true;
      case swd::Event::ButtonDown:
        if (e.button == swd::Event::A) primary();
        else rewind();
        return true;
      default:
        return false;
    }
  }

  void update(const swd::FrameInfo &f) override {
    const uint32_t nowMs = millis();
    if (state_ == Running) {
      float remaining = runRemaining_ - (esp_timer_get_time() - runStartUs_) / 1e6f;
      if (remaining <= 0) {
        finish();
      } else {
        setPalette(remaining <= 60);
        // The arc only moves ~0.1 degree per second: redraw when visible.
        float deg = remaining * 0.1f;
        if (fabsf(deg - shownDeg_) >= 0.05f) showArc(deg);
        showTime(int(ceilf(remaining)));
        // A slow breathing glow (6 s). It touches wide strips at the top and
        // bottom, so it changes at most every 250 ms, in coarse steps.
        if (nowMs - breathMs_ >= 250) {
          breathMs_ = nowMs;
          float b = 0.5f - 0.5f * cosf(f.time * 2 * PI / 6);
          breath_.setVisible(true);
          breath_.set(Shape::ring(kCX, kCY, kRingR + kRingW / 2 + 9, 2, warm_ ? 0xFF7A45 : 0x6366F1,
                                  uint8_t(int(20 + b * 70) / 10 * 10)));
        }
      }
    }
    if (tween_.active) {
      float t = min(1.0f, (nowMs - tween_.startMs) / float(tween_.ms));
      showArc(tween_.from + (tween_.to - tween_.from) * easeOutCubic(t));
      if (t >= 1) tween_.active = false;
    }
    if (burstLeft_ > 0) animateBurst(f.dt);
  }

  // Serial hook: jump to the end.
  void almostDone() {
    if (state_ == Running) runRemaining_ = 3, runStartUs_ = esp_timer_get_time();
  }

 private:
  struct Tween {
    bool active = false;
    float from = 0, to = 0;
    uint32_t startMs = 0, ms = 0;
  };
  struct Particle {
    swd::ShapeItem item;
    float x, y, vx, vy, r, life, age;
    uint32_t color;
  };

  static int minuteAt(float deg) { return constrain(int(roundf(deg / 6)), 0, 60); }

  // A three-stop palette's color at t.
  static uint32_t colorAt(const swd::ColorStop *s, float t) {
    int k = t <= s[1].offset ? 0 : 1;
    float f = constrain((t - s[k].offset) / (s[k + 1].offset - s[k].offset), 0.0f, 1.0f);
    uint32_t out = 0;
    for (int sh = 0; sh <= 16; sh += 8) {
      float a = s[k].color >> sh & 0xFF, b = s[k + 1].color >> sh & 0xFF;
      out |= uint32_t(a + (b - a) * f + 0.5f) << sh;
    }
    return out;
  }

  void enter(State s) {
    state_ = s;
    switch (s) {
      case Empty:
        caption("drag around the ring");
        digits_.setColor(kDim);
        break;
      case Ready:
        caption("tap to start");
        digits_.setColor(kText);
        break;
      case Running:
        caption("focus");
        digits_.setColor(kText);
        break;
      case Paused:
        caption("paused · tap to resume");
        digits_.setColor(kMuted);
        breath_.setVisible(false);  // invisible items cost nothing to render
        break;
      case Done:
        caption("done · tap to reset");
        digits_.setColor(kText);
        breath_.setVisible(false);  // invisible items cost nothing to render
        break;
    }
  }

  // Center tap / KEY A.
  void primary() {
    switch (state_) {
      case Ready:
        runRemaining_ = duration_, runStartUs_ = esp_timer_get_time();
        swd::Haptics::pattern({18, 70, 18});
        enter(Running);
        break;
      case Running:
        runRemaining_ -= (esp_timer_get_time() - runStartUs_) / 1e6f;
        swd::Haptics::click();
        enter(Paused);
        break;
      case Paused:
        runStartUs_ = esp_timer_get_time();
        swd::Haptics::click();
        enter(Running);
        break;
      case Done:
        rewind();
        break;
      case Empty:
        break;
    }
  }

  // KEY B: wind back up to the set time.
  void rewind() {
    if (!duration_) return;
    setPalette(false);
    animateArc(duration_ * 0.1f, 450);
    showTime(duration_);
    swd::Haptics::pattern({10, 40, 10, 40, 10});
    enter(Ready);
  }

  void finish() {
    showArc(0);
    showTime(0);
    enter(Done);
    swd::Haptics::pattern({70, 90, 70, 90, 160});
    // Burst: particles start along the ring and fly outward.
    for (int i = 0; i < kParticles; i++) {
      Particle &p = particles_[i];
      float a = (i + (esp_random() % 100) / 100.0f) * 360.0f / kParticles, speed = 70 + esp_random() % 140;
      polar(a, kRingR, p.x, p.y);
      float ox, oy;
      polar(a, 1, ox, oy);
      p.vx = (ox - kCX) * speed, p.vy = (oy - kCY) * speed;
      p.r = 3 + (esp_random() % 30) / 10.0f;
      p.life = 0.8f + (esp_random() % 70) / 100.0f, p.age = 0;
      static const uint32_t kColors[] = {0x22D3EE, 0x6366F1, 0xA855F7, 0xFFFFFF, 0xFFC371};
      p.color = kColors[i % 5];
      p.item.setVisible(true);
    }
    burstLeft_ = 1.5f;
  }

  void animateBurst(float dt) {
    burstLeft_ -= dt;
    for (auto &p : particles_) {
      if (!p.item.visible()) continue;
      p.age += dt;
      if (p.age >= p.life) {
        p.item.setVisible(false);
        continue;
      }
      p.vx *= 0.97f, p.vy *= 0.97f;
      p.x += p.vx * dt, p.y += p.vy * dt;
      float k = 1 - p.age / p.life;
      p.item.set(Shape::circle(p.x, p.y, p.r * (0.4f + 0.6f * k), p.color, uint8_t(255 * k)));
    }
  }

  void showDrag() {
    int minute = minuteAt(dragAngle_);
    if (minute != lastMinute_) {
      swd::Haptics::click(minute % 5 == 0 ? 200 : 130, minute % 5 == 0 ? 20 : 12);  // firmer on fives
      lastMinute_ = minute;
    }
    showArc(dragAngle_);
    showTime(minute * 60);
    digits_.setColor(minute ? kText : kDim);
  }

  void setDuration(int seconds) {
    duration_ = seconds;
    showTime(seconds);
  }

  void showTime(int seconds) {
    char text[8];
    snprintf(text, sizeof(text), "%02d:%02d", seconds / 60, seconds % 60);
    digits_.setText(text);
  }

  void caption(const char *text) { caption_.setText(text); }

  void animateArc(float to, uint32_t ms) {
    tween_ = {true, shownDeg_, to, millis(), ms};
  }

  void setPalette(bool warm) {
    if (warm == warm_ && paletteSet_) return;
    warm_ = warm, paletteSet_ = true;
    const swd::ColorStop *s = warm ? kWarm : kCool;
    // Diagonal across the ring, in screen coordinates (the arcs aren't transformed).
    swd::Paint p = swd::Paint::linear(kCX - kRingR, kCY - kRingR, kCX + kRingR, kCY + kRingR, {s[0], s[1], s[2]});
    for (auto &seg : segs_) seg.setStroke(kRingW, p, swd::Cap::Butt);
    tip_.setStroke(kRingW, p, swd::Cap::Round);
    // The gradient's color at 12 o'clock (t = 0.25 along the diagonal).
    startCap_.set(Shape::circle(kCX, kCY - kRingR, kRingW / 2, colorAt(s, 0.25f)));
    knobDot_.set(Shape::circle(knobDot_.get().x0, knobDot_.get().y0, 4.5f, s[1].color));
  }

  // The arc from 12 o'clock to `deg`: whole-minute segments switched on,
  // plus a tip for the rest (changes every frame while dragging).
  void showArc(float deg) {
    deg = constrain(deg, 0.0f, 360.0f);
    shownDeg_ = deg;
    float bodyDeg = floorf(deg / 6) * 6;
    if (deg - bodyDeg < 0.5f && bodyDeg >= 6) bodyDeg -= 6;  // keep the tip from vanishing into the cap
    for (int i = 0; i < 60; i++) segs_[i].setVisible((i + 1) * 6 <= bodyDeg);
    startCap_.setVisible(deg > 0.05f);
    tipPath_.clear();
    if (deg - bodyDeg > 0.05f) tipPath_.arc(kCX, kCY, kRingR, bodyDeg, deg);
    tip_.setPath(tipPath_);
    tip_.setVisible(deg - bodyDeg > 0.05f);
    for (int i = 1; i < 12; i++) ticks_[i].setVisible(deg >= i * 30 - 0.01f);
    float kx, ky;
    polar(deg, kRingR, kx, ky);
    knobShadow_.set(Shape::circle(kx + 2, ky + 3, 15, 0x000000, 100));
    knob_.set(Shape::circle(kx, ky, 13, 0xFFFFFF));
    knobDot_.set(Shape::circle(kx, ky, 4.5f, knobDot_.get().color));
  }

  static constexpr int kParticles = 28;
  State state_ = Ready;
  int duration_ = 0, lastMinute_ = 0;
  bool dragging_ = false, warm_ = false, paletteSet_ = false;
  float dragAngle_ = 0, lastTouch_ = 0, shownDeg_ = 0;
  float runRemaining_ = 0, burstLeft_ = 0;
  uint32_t breathMs_ = 0;
  int64_t runStartUs_ = 0;
  Tween tween_;
  swd::Path segPaths_[60], tipPath_;
  swd::PathItem segs_[60], tip_;
  swd::ShapeItem ticks_[12], breath_, knobShadow_, knob_, knobDot_, startCap_;
  Particle particles_[kParticles];
  swd::TextItem digits_, caption_;
};
static Focus focus;

void setup() {
  Serial.begin(115200);
  Serial.setTxTimeoutMs(0);
  auto cfg = M5.config();
  cfg.fallback_board = m5::board_t::board_M5StopWatch;
  M5.begin(cfg);
  if (!engine.begin()) Serial.println("error: engine init failed");
  if (!dial.begin()) Serial.println("error: dial allocation failed");
  drawDial();
  focus.begin();
  engine.add(dial);
  engine.add(focus);
  Serial.printf("ready, free internal=%u psram=%u\n", heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
}

void loop() {
  static uint32_t windowStart = millis();
  static swd::Engine::Counters last = {};
  static uint32_t worstDraw = 0, worstUpdate = 0;
  while (Serial.available()) {
    char ch = Serial.read();
    if (ch == 'a') engine.post(swd::Event::buttonDown(swd::Event::A));
    if (ch == 'b') engine.post(swd::Event::buttonDown(swd::Event::B));
    if (ch == 'x') focus.almostDone();
  }
  engine.frame();
  static char worstInfo[160] = "";
  if (engine.lastFrame().drawUs > worstDraw) {
    auto &st = engine.renderer().lastStats();
    snprintf(worstInfo, sizeof(worstInfo), "pieces=%u render=%u wait_render=%u wait_scan=%u dial=%u focus=%u full=%d",
             st.bands, st.render, st.waitRender, st.waitScan, engine.layerRenderUs(0), engine.layerRenderUs(1),
             engine.lastFrame().full);
  }
  worstDraw = max(worstDraw, engine.lastFrame().drawUs);
  worstUpdate = max(worstUpdate, engine.lastFrame().updateUs);

  uint32_t elapsed = millis() - windowStart;
  if (elapsed >= 2000) {
    auto c = engine.counters();
    Serial.printf("frames=%u draws=%u idle_sleeps=%u asleep=%.0f%% worst_update_us=%u worst_draw_us=%u torn=%u skipped=%u\n",
                  c.frames - last.frames, c.draws - last.draws, c.idleWaits - last.idleWaits,
                  (c.sleptUs - last.sleptUs) / (elapsed * 10.0f), worstUpdate, worstDraw,
                  engine.renderer().tornCount(), engine.skipped());
    if (worstDraw > 16000) Serial.printf("  worst draw: %s\n", worstInfo);
    last = c;
    windowStart = millis();
    worstDraw = worstUpdate = 0;
  }
}
