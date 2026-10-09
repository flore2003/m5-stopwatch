// Ready-made layers for swd::Engine.
#pragma once

#include "Engine.h"
#include "GradientBackground.h"

namespace swd {

// Full-screen animated gradient (see GradientBackground.h). With interval N
// it advances every N refreshes; between those it stays put, and areas that
// other layers redraw show it unchanged.
class GradientLayer : public Layer {
 public:
  void setTheme(const GradientTheme &theme) {
    theme_ = &theme;
    changed_ = true;
  }
  void setInterval(uint8_t refreshes) { interval_ = refreshes ? refreshes : 1; }

  void update(const FrameInfo &f) override;
  bool animating() const override { return true; }
  void render(const Span &s) const override { Gradient::render(state_, s.dst, s.x, s.y, s.w, s.rows); }

 private:
  Gradient::State state_ = {};
  const GradientTheme *theme_ = &kGradientThemes[0];
  uint8_t interval_ = 1;
  uint32_t lastRefresh_ = 0;
  bool changed_ = true;
};

// A single color, e.g. a static background.
class SolidLayer : public Layer {
 public:
  explicit SolidLayer(uint16_t color = rgb565(0, 0, 0)) : color_(color) {}
  void setColor(uint16_t color) {
    color_ = color;
    invalidate();
  }
  void render(const Span &s) const override;

 private:
  uint16_t color_;
};

// A sprite drawn with M5GFX into an SRAM canvas and composited over the
// layers below. Pixels of kTransparent are see-through; avoid anti-aliased
// edges (they would blend into that key color).
class SpriteLayer : public Layer {
 public:
  static constexpr uint16_t kTransparent = Overlay::kTransparent;

  bool begin(int w, int h);
  // Draw into this, then call changed().
  M5Canvas &canvas() { return overlay_.canvas(); }
  void clear() { overlay_.clear(); }
  void changed() { invalidate(); }
  // Moves the sprite; redraws both where it was and where it is.
  void moveTo(int x, int y);
  int x() const { return overlay_.x(); }
  int y() const { return overlay_.y(); }
  int width() const { return overlay_.width(); }
  int height() const { return overlay_.height(); }

  Rect bounds() const override { return Rect::xywh(x(), y(), width(), height()); }
  void render(const Span &s) const override { overlay_.composite(s.dst, s.x, s.y, s.w, s.rows); }

 private:
  Overlay overlay_;
};

// A small pill showing fps and the tear count, updated once a second.
class StatsLayer : public SpriteLayer {
 public:
  explicit StatsLayer(Engine &engine) : engine_(engine) {}
  // Centered horizontally at row y.
  bool begin(int y = 34);
  void update(const FrameInfo &f) override;
  // Keeps the engine awake to count frames; hide it to let a scene idle.
  bool animating() const override { return true; }
  // Short extra text after the numbers (e.g. a mode); shown on the next redraw.
  void setNote(const char *note) {
    note_ = note;
    noteChanged_ = true;
  }

 private:
  void redraw(float fps);
  Engine &engine_;
  const char *note_ = "";
  bool noteChanged_ = false;
  float fps_ = 0;
  uint32_t windowStartRefresh_ = 0, windowFrames_ = 0;
};

}  // namespace swd
