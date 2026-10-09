#include "Engine.h"

namespace swd {

Rect Rect::unite(const Rect &o) const {
  if (empty()) return o;
  if (o.empty()) return *this;
  return {min(x0, o.x0), min(y0, o.y0), max(x1, o.x1), max(y1, o.y1)};
}

Rect Rect::intersect(const Rect &o) const {
  Rect r = {max(x0, o.x0), max(y0, o.y0), min(x1, o.x1), min(y1, o.y1)};
  return r.empty() ? Rect() : r;
}

// ---- Haptics

namespace {

struct HapticPattern {
  uint8_t strength, count;
  uint16_t steps[Haptics::kMaxSteps];
};
QueueHandle_t hapticQueue = nullptr;

}  // namespace

void Haptics::begin() {
  if (!hapticQueue) hapticQueue = xQueueCreate(1, sizeof(HapticPattern));
}

void Haptics::pattern(std::initializer_list<uint16_t> steps, uint8_t strength) {
  if (!hapticQueue) return;
  HapticPattern p = {strength, 0, {}};
  for (uint16_t ms : steps) {
    if (p.count < kMaxSteps) p.steps[p.count++] = ms;
  }
  xQueueOverwrite(hapticQueue, &p);
}

void Haptics::service() {
  static HapticPattern playing = {};
  static int step = -1;
  static uint32_t stepEnd = 0;
  static bool motorOn = false;
  auto motor = [&](bool on) {
    if (on == motorOn) return;  // the write goes over I2C: only on changes
    motorOn = on;
    M5.Power.setVibration(on ? playing.strength : 0);
  };
  HapticPattern next;
  if (hapticQueue && xQueueReceive(hapticQueue, &next, 0) == pdTRUE) {
    playing = next;
    step = 0;
    stepEnd = millis() + playing.steps[0];
    motor(true);
  }
  while (step >= 0 && int32_t(millis() - stepEnd) >= 0) {
    if (++step >= playing.count) {
      step = -1;
      motor(false);
      break;
    }
    stepEnd += playing.steps[step];
    motor(step % 2 == 0);  // even steps on, odd steps off
  }
}

// ---- Layer

void Layer::invalidate(const Rect &r) {
  if (engine_) engine_->addDirty(r);
}

void Layer::setVisible(bool visible) {
  if (visible == visible_) return;
  visible_ = visible;
  invalidate();
}

// ---- Engine

bool Engine::begin() { return begin(Config()); }

bool Engine::begin(const Config &cfg) {
  cfg_ = cfg;
  // Dark until the first full frame is on the panel: it takes longer than
  // a refresh, so it would otherwise show up half drawn.
  setupDisplay(0);
  Vsync::begin();
  events_ = xQueueCreate(32, sizeof(Event));
  Haptics::begin();
  if (!events_ || !renderer_.begin(renderSpan, this, cfg_.renderer)) return false;
  // Input polling shares the render core at low priority, so its I2C reads
  // never delay the start of a frame on this core.
  if (cfg_.input) xTaskCreatePinnedToCore(inputTask, "input", 4096, this, 1, nullptr, 1 - xPortGetCoreID());
  startRefresh_ = lastRefresh_ = Vsync::count();
  addDirty(Rect::screen());  // first frame draws everything
  return true;
}

void Engine::add(Layer &layer) {
  if (layerCount_ >= kMaxLayers) return;
  layer.engine_ = this;
  layers_[layerCount_++] = &layer;
  layer.invalidate();
}

// Dirty areas are kept per band as sorted, non-overlapping x-intervals
// (drawRegions needs non-overlapping regions). Intervals closer than kGap
// are joined: one wider transfer is cheaper than two.
void Engine::addDirty(const Rect &in) {
  constexpr int kGap = 16;
  Rect r = in.intersect(Rect::screen());
  if (r.empty()) return;
  dirty_ = true;
  for (int b = r.y0 / kBandRows; b * kBandRows < r.y1 && b < kBands; b++) {
    Band &band = bands_[b];
    int by0 = max(r.y0, b * kBandRows), by1 = min(r.y1, (b + 1) * kBandRows);
    if (band.n == 0) {
      band.y0 = by0, band.y1 = by1;
    } else {
      band.y0 = min<int>(band.y0, by0), band.y1 = max<int>(band.y1, by1);
    }
    int x0 = r.x0, x1 = r.x1;
    // Absorb every interval that overlaps or nearly touches [x0, x1).
    int k = 0;
    for (int i = 0; i < band.n; i++) {
      if (band.x1[i] + kGap >= x0 && x1 + kGap >= band.x0[i]) {
        x0 = min<int>(x0, band.x0[i]), x1 = max<int>(x1, band.x1[i]);
      } else {
        band.x0[k] = band.x0[i], band.x1[k] = band.x1[i], k++;
      }
    }
    band.n = k;
    if (band.n == kMaxIntervals) {
      // Full: fold everything into one interval.
      for (int i = 0; i < band.n; i++) x0 = min<int>(x0, band.x0[i]), x1 = max<int>(x1, band.x1[i]);
      band.n = 0;
    }
    // Insert sorted by x.
    int pos = band.n;
    while (pos > 0 && band.x0[pos - 1] > x0) {
      band.x0[pos] = band.x0[pos - 1], band.x1[pos] = band.x1[pos - 1];
      pos--;
    }
    band.x0[pos] = x0, band.x1[pos] = x1;
    band.n++;
  }
}

bool Engine::post(const Event &e) { return events_ && xQueueSend(events_, &e, 0) == pdTRUE; }

void Engine::inputTask(void *self) {
  auto *e = static_cast<Engine *>(self);
  bool touching = false;
  int16_t lastX = 0, lastY = 0;
  for (;;) {
    M5.update();
    m5::Button_Class *buttons[2] = {&M5.BtnA, &M5.BtnB};
    for (uint8_t b = 0; b < 2; b++) {
      Event ev = {Event::ButtonDown};
      ev.button = b;
      if (buttons[b]->wasPressed()) e->post(ev);
      if (buttons[b]->wasHold()) {
        ev.type = Event::ButtonHold;
        e->post(ev);
      }
      if (buttons[b]->wasReleased()) {
        ev.type = Event::ButtonUp;
        e->post(ev);
      }
    }
    auto t = M5.Touch.getDetail();
    Event ev = {Event::TouchDown};
    ev.x = t.x;
    ev.y = t.y;
    if (t.wasPressed()) {
      touching = true;
      e->post(ev);
    } else if (touching && t.isPressed() && (t.x != lastX || t.y != lastY)) {
      ev.type = Event::TouchMove;
      e->post(ev);
    }
    if (t.wasClicked()) {
      ev.type = Event::Tap;
      e->post(ev);
    }
    if (t.wasReleased()) {
      touching = false;
      ev.type = Event::TouchUp;
      e->post(ev);
    }
    lastX = t.x;
    lastY = t.y;
    Haptics::service();
    vTaskDelay(pdMS_TO_TICKS(5));
  }
}

void Engine::dispatch(const Event &e) {
  counters_.events++;
  for (int i = layerCount_ - 1; i >= 0; i--) {
    if (layers_[i]->visible_ && layers_[i]->onEvent(e)) return;
  }
  if (handler_) handler_(e);
}

bool Engine::anyAnimating() const {
  for (int i = 0; i < layerCount_; i++) {
    if (layers_[i]->visible_ && layers_[i]->animating()) return true;
  }
  return false;
}

void Engine::frame() {
  counters_.frames++;
  uint32_t s0 = micros();
  bool resumed = false;
  if (!dirty_ && !anyAnimating() && uxQueueMessagesWaiting(events_) == 0) {
    // Nothing to do: sleep until an event (or the timeout), then resync.
    Event e;
    xQueuePeek(events_, &e, pdMS_TO_TICKS(cfg_.idleTimeoutMs));
    counters_.idleWaits++;
    pacer_.reset();
    resumed = true;
  }
  pacer_.wait();
  counters_.sleptUs += micros() - s0;

  const uint32_t u0 = micros();
  Event e;
  while (xQueueReceive(events_, &e, 0) == pdTRUE) dispatch(e);

  uint32_t refresh = Vsync::count();
  float period = Vsync::periodUs() / 1e6f;
  info_.refresh = refresh - startRefresh_;
  info_.time = info_.refresh * period;
  // After idling, count the first frame as one refresh so nothing jumps.
  info_.dt = (resumed ? 1 : refresh - lastRefresh_) * period;
  lastRefresh_ = refresh;

  for (int i = 0; i < layerCount_; i++) {
    if (layers_[i]->visible_) layers_[i]->update(info_);
  }

  last_ = {};
  last_.updateUs = micros() - u0;
  if (dirty_) {
    int area = 0;
    for (const Band &b : bands_) {
      for (int i = 0; i < b.n; i++) area += (b.x1[i] - b.x0[i]) * (b.y1 - b.y0);
    }
    Region regions[kBands * kMaxIntervals];
    int n = 0;
    if (area > cfg_.fullFrameThreshold * kWidth * kHeight) {
      regions[n++] = {0, 0, kWidth, kHeight};
      last_.full = true;
    } else {
      for (const Band &b : bands_) {
        for (int i = 0; i < b.n; i++) regions[n++] = {b.x0[i], b.y0, b.x1[i] - b.x0[i], b.y1 - b.y0};
      }
    }
    for (Band &b : bands_) b.n = 0;
    dirty_ = false;
    memset(layerUs_, 0, sizeof(layerUs_));
    last_.drawUs = renderer_.drawRegions(regions, n);
    last_.regions = n;
    last_.torn = renderer_.lastStats().torn;
    last_.slackUs = renderer_.lastStats().slackUs;
    if (counters_.draws++ == 0) {
      Vsync::waitNext();  // let the panel show the finished frame first
      Vsync::waitNext();
      M5.Display.setBrightness(cfg_.brightness);
    }
  }
  info_.frame++;
}

// Render core: composite all visible layers that overlap the span, back to
// front.
void Engine::renderSpan(uint16_t *dst, int x, int y, int w, int rows, void *self) {
  auto *e = static_cast<Engine *>(self);
  Span s = {dst, x, y, w, rows};
  Rect sr = s.rect();
  for (int i = 0; i < e->layerCount_; i++) {
    const Layer *l = e->layers_[i];
    if (l->visible_ && l->bounds().intersects(sr)) {
      uint32_t t0 = micros();
      l->render(s);
      e->layerUs_[i] += micros() - t0;
    }
  }
}

}  // namespace swd
