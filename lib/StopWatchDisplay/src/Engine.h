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
// Each frame() waits for the next refresh, calls update() on every layer,
// and redraws only what the layers invalidated. Layers change their state in
// update() (main core) and call invalidate() for every area whose pixels
// change; render() runs on the render core and must only read state. A layer
// that animates more slowly (e.g. a background at 30 fps) simply invalidates
// less often, and areas redrawn for other layers in between render it with
// the state that is already on screen.
#pragma once

#include "StopWatchDisplay.h"

namespace swd {

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

  // Main core, once per frame, before anything renders.
  virtual void update(const FrameInfo &) {}
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
  };

  struct FrameStats {
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

  // One frame: wait for the next refresh, update layers, draw what's dirty.
  void frame();

  const FrameStats &lastFrame() const { return last_; }
  const FrameInfo &info() const { return info_; }
  StripRenderer &renderer() { return renderer_; }
  uint32_t skipped() const { return pacer_.skipped(); }

 private:
  friend class Layer;
  static constexpr int kMaxLayers = 16;
  static constexpr int kMaxDirty = 16;

  void addDirty(const Rect &r);
  static void renderSpan(uint16_t *dst, int x, int y, int w, int rows, void *self);

  Config cfg_;
  StripRenderer renderer_;
  FramePacer pacer_{1};
  Layer *layers_[kMaxLayers] = {};
  int layerCount_ = 0;
  Rect dirty_[kMaxDirty];
  int dirtyCount_ = 0;
  uint32_t startRefresh_ = 0, lastRefresh_ = 0;
  FrameInfo info_ = {};
  FrameStats last_ = {};
};

}  // namespace swd
