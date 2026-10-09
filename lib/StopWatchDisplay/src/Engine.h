// A small render engine on top of StripRenderer: layers, dirty regions and a
// per-refresh loop.
//
//   swd::Engine engine;
//   swd::GradientLayer background;
//   MyBall ball;                       // a SpriteLayer subclass
//   engine.begin();
//   engine.add(background);            // back to front
//   engine.add(ball);
//   loop() { engine.frame(); }
//
// Each frame() waits for the next refresh, delivers input events, calls
// update() on every layer, and redraws only what the layers invalidated.
// Layers change their state in onEvent()/update() (main core) and call
// invalidate() for every area whose pixels change; render() runs on the
// render core and must only read state. A layer that animates more slowly
// (e.g. a background at 30 fps) simply invalidates less often, and areas
// redrawn for other layers in between render it with the state that is
// already on screen.
//
// Threading: draws run synchronously inside frame(), so any code on the
// loop task (setup(), loop(), event handlers, update()) runs between draws
// and may change layers freely. Other tasks must not touch layers; they can
// post() events instead.
//
// Idle: when no layer is animating(), nothing is invalidated and no events
// are waiting, frame() sleeps until an event arrives (or idleTimeoutMs
// passes) instead of waking on every refresh.
#pragma once

#include <functional>

#include "StopWatchDisplay.h"

namespace swd {

// Dirty areas are tracked per band of this many rows (the renderer's strip
// height), as x-intervals, so thin diagonal shapes don't dirty their whole
// bounding box.
constexpr int kBandRows = 32;
constexpr int kBands = (kHeight + kBandRows - 1) / kBandRows;

struct Rect {
  int x0 = 0, y0 = 0, x1 = 0, y1 = 0;  // [x0, x1) x [y0, y1)

  static Rect xywh(int x, int y, int w, int h) { return {x, y, x + w, y + h}; }
  static Rect screen() { return {0, 0, kWidth, kHeight}; }
  bool empty() const { return x1 <= x0 || y1 <= y0; }
  int area() const { return empty() ? 0 : (x1 - x0) * (y1 - y0); }
  bool intersects(const Rect &o) const { return x0 < o.x1 && o.x0 < x1 && y0 < o.y1 && o.y0 < y1; }
  Rect unite(const Rect &o) const;
  Rect intersect(const Rect &o) const;
};

// A region of the screen to render: pixels for columns [x, x + w) and rows
// [y, y + rows), row stride w. x and w are always even.
struct Span {
  uint16_t *dst;
  int x, y, w, rows;
  Rect rect() const { return Rect::xywh(x, y, w, rows); }
  uint16_t *at(int px, int py) const { return dst + (py - y) * w + (px - x); }
};

struct Event {
  enum Type : uint8_t {
    TouchDown,   // x, y
    TouchMove,   // x, y (while pressed)
    TouchUp,     // x, y
    Tap,         // x, y: short press and release in place
    ButtonDown,  // button
    ButtonUp,    // button
    ButtonHold,  // button: held for ~500 ms
    User,        // value: for post() from app code
  };
  enum Button : uint8_t { A = 0, B = 1 };  // A: yellow, upper left; B: blue, upper right

  Type type;
  uint8_t button = 0;
  int16_t x = 0, y = 0;
  int32_t value = 0;

  static Event user(int32_t value) {
    Event e = {User};
    e.value = value;
    return e;
  }
  static Event buttonDown(uint8_t button) {
    Event e = {ButtonDown};
    e.button = button;
    return e;
  }
};

struct FrameInfo {
  uint32_t frame;    // frames drawn so far
  uint32_t refresh;  // panel refreshes since begin()
  float time;        // seconds since begin(), in whole refreshes
  float dt;          // seconds since the previous frame (one refresh at 60 fps)
};

class Engine;

class Layer {
 public:
  virtual ~Layer() = default;

  // Main core, before update(), front-most layer first. Return true to
  // consume the event (layers below and the app handler won't see it).
  virtual bool onEvent(const Event &) { return false; }
  // Main core, once per frame, before anything renders.
  virtual void update(const FrameInfo &) {}
  // True while the layer needs update() every refresh (it's moving). When no
  // layer is animating, the engine idles until the next event.
  virtual bool animating() const { return false; }
  // Render core. Draw this layer's pixels inside s (only where bounds()
  // overlaps it). Layers are drawn back to front; the bottom layer must cover
  // the whole screen.
  virtual void render(const Span &s) const = 0;
  // Where this layer can draw; spans outside it skip the layer.
  virtual Rect bounds() const { return Rect::screen(); }

  void setVisible(bool visible);
  bool visible() const { return visible_; }

 protected:
  // Mark an area (or the whole layer) for redraw this frame.
  void invalidate(const Rect &r);
  void invalidate() { invalidate(bounds()); }

 private:
  friend class Engine;
  Engine *engine_ = nullptr;
  bool visible_ = true;
};

class Engine {
 public:
  struct Config {
    StripRenderer::Config renderer;
    // Dirty areas covering more than this share of the screen are drawn as
    // one full frame (fewer, larger transfers).
    float fullFrameThreshold = 0.5f;
    // Read buttons and touch in an engine task and deliver them as events.
    bool input = true;
    // Longest frame() sleeps while idle, so loop() still runs now and then.
    uint32_t idleTimeoutMs = 1000;
  };

  // Since begin().
  struct Counters {
    uint32_t frames;     // frame() calls
    uint32_t draws;      // frames that drew something
    uint32_t idleWaits;  // frames that slept until an event or timeout
    uint32_t events;     // events delivered
    uint64_t sleptUs;    // time the loop task spent blocked (vsync + idle)
  };

  struct FrameStats {
    uint32_t updateUs;    // events + layer update()s (main core, before the draw)
    uint32_t drawUs;      // the draw (0 if nothing was dirty)
    uint16_t regions;     // regions drawn (0: nothing dirty)
    bool full;            // drawn as a full frame
    bool torn;            // StripRenderer's tear check
    int32_t slackUs;
  };

  // Call after M5.begin(). Sets up the display, vsync and the renderer.
  bool begin();
  bool begin(const Config &cfg);
  void add(Layer &layer);  // back to front

  // Events nobody consumed end up here (main core).
  void onEvent(std::function<void(const Event &)> handler) { handler_ = std::move(handler); }
  // Queue an event for the next frame. Safe from any task.
  bool post(const Event &e);

  // One frame: wait for the next refresh, update layers, draw what's dirty.
  void frame();

  const FrameStats &lastFrame() const { return last_; }
  const FrameInfo &info() const { return info_; }
  StripRenderer &renderer() { return renderer_; }
  uint32_t skipped() const { return pacer_.skipped(); }
  const Counters &counters() const { return counters_; }
  // Render-core time spent in layer i (in add() order) during the last draw.
  uint32_t layerRenderUs(int i) const { return i >= 0 && i < layerCount_ ? layerUs_[i] : 0; }
  int layerCount() const { return layerCount_; }

 private:
  friend class Layer;
  static constexpr int kMaxLayers = 16;
  static constexpr int kMaxIntervals = 6;  // per band; more get merged

  struct Band {
    int16_t y0, y1;  // dirty rows within the band
    uint8_t n;
    int16_t x0[kMaxIntervals], x1[kMaxIntervals];
  };

  void addDirty(const Rect &r);
  void dispatch(const Event &e);
  bool anyAnimating() const;
  static void renderSpan(uint16_t *dst, int x, int y, int w, int rows, void *self);
  static void inputTask(void *self);

  Config cfg_;
  StripRenderer renderer_;
  FramePacer pacer_{1};
  Layer *layers_[kMaxLayers] = {};
  int layerCount_ = 0;
  Band bands_[kBands] = {};
  bool dirty_ = false;
  uint32_t startRefresh_ = 0, lastRefresh_ = 0;
  FrameInfo info_ = {};
  FrameStats last_ = {};
  Counters counters_ = {};
  uint32_t layerUs_[kMaxLayers] = {};
  QueueHandle_t events_ = nullptr;
  std::function<void(const Event &)> handler_;
};

}  // namespace swd
