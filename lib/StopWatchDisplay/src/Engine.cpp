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
  setupDisplay();
  Vsync::begin();
  events_ = xQueueCreate(32, sizeof(Event));
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

// Keeps the dirty list small and its rects non-overlapping (drawRegions
// requires that): a new rect is merged with every rect it touches.
void Engine::addDirty(const Rect &in) {
  Rect r = in.intersect(Rect::screen());
  if (r.empty()) return;
  bool merged = true;
  while (merged) {
    merged = false;
    for (int i = 0; i < dirtyCount_; i++) {
      if (dirty_[i].intersects(r)) {
        r = r.unite(dirty_[i]);
        dirty_[i] = dirty_[--dirtyCount_];
        merged = true;
        break;
      }
    }
  }
  if (dirtyCount_ < kMaxDirty) {
    dirty_[dirtyCount_++] = r;
  } else {
    // Out of slots: fold everything into one rect.
    for (int i = 0; i < dirtyCount_; i++) r = r.unite(dirty_[i]);
    dirty_[0] = r;
    dirtyCount_ = 1;
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
    vTaskDelay(pdMS_TO_TICKS(10));
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
  if (dirtyCount_ == 0 && !anyAnimating() && uxQueueMessagesWaiting(events_) == 0) {
    // Nothing to do: sleep until an event (or the timeout), then resync.
    Event e;
    xQueuePeek(events_, &e, pdMS_TO_TICKS(cfg_.idleTimeoutMs));
    counters_.idleWaits++;
    pacer_.reset();
    resumed = true;
  }
  pacer_.wait();
  counters_.sleptUs += micros() - s0;

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
  if (dirtyCount_ > 0) {
    int area = 0;
    for (int i = 0; i < dirtyCount_; i++) area += dirty_[i].area();
    Region regions[kMaxDirty];
    int n = 0;
    if (area > cfg_.fullFrameThreshold * kWidth * kHeight) {
      regions[n++] = {0, 0, kWidth, kHeight};
      last_.full = true;
    } else {
      for (int i = 0; i < dirtyCount_; i++) {
        const Rect &r = dirty_[i];
        regions[n++] = {r.x0, r.y0, r.x1 - r.x0, r.y1 - r.y0};
      }
    }
    dirtyCount_ = 0;
    last_.drawUs = renderer_.drawRegions(regions, n);
    counters_.draws++;
    last_.regions = n;
    last_.torn = renderer_.lastStats().torn;
    last_.slackUs = renderer_.lastStats().slackUs;
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
    if (l->visible_ && l->bounds().intersects(sr)) l->render(s);
  }
}

}  // namespace swd
