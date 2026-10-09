#include "Layers.h"

namespace swd {

// ---- GradientLayer

void GradientLayer::update(const FrameInfo &f) {
  if (!changed_ && f.refresh - lastRefresh_ < interval_) return;
  changed_ = false;
  lastRefresh_ = f.refresh;
  Gradient::update(state_, *theme_, f.time);
  invalidate();
}

// ---- SpriteLayer

bool SpriteLayer::begin(int w, int h) { return overlay_.begin(w, h); }

void SpriteLayer::moveTo(int x, int y) {
  if (x == overlay_.x() && y == overlay_.y()) return;
  invalidate();  // old area
  overlay_.setPosition(x, y);
  invalidate();  // new area
}

// ---- StatsLayer

bool StatsLayer::begin(int y) {
  if (!SpriteLayer::begin(150, 26)) return false;
  moveTo((kWidth - width()) / 2, y);
  redraw(0);
  return true;
}

void StatsLayer::update(const FrameInfo &f) {
  windowFrames_++;
  uint32_t refreshes = f.refresh - windowStartRefresh_;
  if (refreshes < 60) return;
  redraw(windowFrames_ * 1e6f / (refreshes * float(Vsync::periodUs())));
  windowStartRefresh_ = f.refresh;
  windowFrames_ = 0;
}

void StatsLayer::redraw(float fps) {
  char text[32];
  uint32_t torn = engine_.renderer().tornCount();
  snprintf(text, sizeof(text), "%.0f fps  %u torn", fps, torn);
  auto &c = canvas();
  clear();
  c.fillRoundRect(0, 0, c.width(), c.height(), c.height() / 2, torn ? c.color565(150, 20, 20) : TFT_BLACK);
  c.setTextColor(TFT_WHITE);
  c.setTextDatum(middle_center);
  c.setFont(&fonts::FreeSansBold9pt7b);
  c.drawString(text, c.width() / 2, c.height() / 2 + 1);
  changed();
}

}  // namespace swd
