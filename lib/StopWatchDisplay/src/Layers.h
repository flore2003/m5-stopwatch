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
  void render(const Span &s) const override { Gradient::render(state_, s.dst, s.x, s.y, s.w, s.rows); }

 private:
  Gradient::State state_ = {};
  const GradientTheme *theme_ = &kGradientThemes[0];
  uint8_t interval_ = 1;
  uint32_t lastRefresh_ = 0;
  bool changed_ = true;
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

 private:
  void redraw(float fps);
  Engine &engine_;
  uint32_t windowStartRefresh_ = 0, windowFrames_ = 0;
};

}  // namespace swd
