// Vector shapes for swd::Engine: anti-aliased, with alpha, any angle.
//
// The engine asks every layer to render arbitrary strips of the screen, so
// shapes are evaluated per pixel from a signed distance function: coverage
// comes straight from the distance to the edge (smooth edges, no
// rasterizing ahead of time, rotation for free). Cost scales with the area a
// shape covers, so this suits hands, ticks, rings and indicators; for static
// artwork, draw shapes once into an ImageLayer and let it serve the pixels.
#pragma once

#include "Engine.h"

namespace swd {

// Blends an RGB888 color with alpha 0..255 over a byte-swapped RGB565 pixel.
inline uint16_t blend565(uint16_t dst, uint32_t rgb, uint32_t alpha) {
  uint16_t d = swap16(dst);
  uint32_t dr = (d >> 8) & 0xF8, dg = (d >> 3) & 0xFC, db = (d << 3) & 0xF8;
  uint32_t sr = rgb >> 16 & 0xFF, sg = rgb >> 8 & 0xFF, sb = rgb & 0xFF;
  uint32_t a = alpha + (alpha >> 7);  // 0..256
  dr += (int32_t(sr - dr) * int32_t(a)) >> 8;
  dg += (int32_t(sg - dg) * int32_t(a)) >> 8;
  db += (int32_t(sb - db) * int32_t(a)) >> 8;
  return swap16(uint16_t((dr & 0xF8) << 8 | (dg & 0xFC) << 3 | db >> 3));
}

struct Shape {
  enum Kind : uint8_t { None, Capsule, Circle, Ring, RoundRect };

  Kind kind = None;
  float x0 = 0, y0 = 0, x1 = 0, y1 = 0;  // capsule: segment; circle/ring: center (x0, y0); round rect: corners
  float r = 0;                           // capsule/circle radius, ring radius, corner radius
  float w = 0;                           // ring: half the stroke width
  uint32_t color = 0xFFFFFF;             // 0xRRGGBB
  uint8_t alpha = 255;

  // A line from (ax, ay) to (bx, by) with round ends, `radius` thick on each side.
  static Shape capsule(float ax, float ay, float bx, float by, float radius, uint32_t color, uint8_t alpha = 255);
  static Shape circle(float cx, float cy, float radius, uint32_t color, uint8_t alpha = 255);
  // A circle outline: centerline radius, stroke width.
  static Shape ring(float cx, float cy, float radius, float width, uint32_t color, uint8_t alpha = 255);
  static Shape roundRect(float x, float y, float w, float h, float radius, uint32_t color, uint8_t alpha = 255);

  // Pixels this shape can touch, including its anti-aliased edge.
  Rect bounds() const;
  // Tighter: the shape's extent within each band of kBandRows rows (two per
  // band for a ring's left and right arcs). Returns how many it wrote.
  int bandExtents(Rect *out, int capacity) const;
  // Blend this shape into a span.
  void render(const Span &s) const;
};

// A list of shapes, drawn in order. Changing a shape redraws both its old
// and its new area.
class ShapeLayer : public Layer {
 public:
  static constexpr int kMaxShapes = 48;

  int add(const Shape &s);  // returns its index, or -1 if full
  void set(int i, const Shape &s);
  const Shape &get(int i) const { return shapes_[i]; }
  int count() const { return count_; }
  void clear();

  Rect bounds() const override;
  void render(const Span &s) const override;

 private:
  void invalidateShape(const Shape &s);
  Shape shapes_[kMaxShapes];
  int count_ = 0;
};

// An opaque full-screen raster held in PSRAM, e.g. a static background drawn
// once from shapes and text. Rendering copies its rows.
class ImageLayer : public Layer {
 public:
  bool begin();
  // Draw with M5GFX into this (anti-aliased drawing and smooth fonts work
  // here), then call changed().
  M5Canvas &canvas() { return canvas_; }
  void changed() { invalidate(); }
  void changed(const Rect &r) { invalidate(r); }
  // Paint another layer (e.g. a ShapeLayer that isn't added to the engine)
  // into this image, over what's there.
  void paint(const Layer &layer);
  // Fills the image with a radial gradient from `inner` (center) to `outer`
  // (at radius `radius`), dithered to hide RGB565 banding.
  void radialGradient(uint32_t inner, uint32_t outer, float radius = kRadius);

  void render(const Span &s) const override;

 private:
  M5Canvas canvas_;
};

}  // namespace swd
