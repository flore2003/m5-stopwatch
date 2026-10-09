#include "Vector.h"

namespace swd {

// ---- Shape

Shape Shape::capsule(float ax, float ay, float bx, float by, float radius, uint32_t color, uint8_t alpha) {
  Shape s;
  s.kind = Capsule;
  s.x0 = ax, s.y0 = ay, s.x1 = bx, s.y1 = by, s.r = radius, s.color = color, s.alpha = alpha;
  return s;
}

Shape Shape::circle(float cx, float cy, float radius, uint32_t color, uint8_t alpha) {
  Shape s;
  s.kind = Circle;
  s.x0 = cx, s.y0 = cy, s.r = radius, s.color = color, s.alpha = alpha;
  return s;
}

Shape Shape::ring(float cx, float cy, float radius, float width, uint32_t color, uint8_t alpha) {
  Shape s;
  s.kind = Ring;
  s.x0 = cx, s.y0 = cy, s.r = radius, s.w = width / 2, s.color = color, s.alpha = alpha;
  return s;
}

Shape Shape::roundRect(float x, float y, float w, float h, float radius, uint32_t color, uint8_t alpha) {
  Shape s;
  s.kind = RoundRect;
  s.x0 = x, s.y0 = y, s.x1 = x + w, s.y1 = y + h, s.r = radius, s.color = color, s.alpha = alpha;
  return s;
}

Rect Shape::bounds() const {
  float l, t, rt, b;
  switch (kind) {
    case Capsule:
      l = min(x0, x1) - r, rt = max(x0, x1) + r, t = min(y0, y1) - r, b = max(y0, y1) + r;
      break;
    case Circle:
      l = x0 - r, rt = x0 + r, t = y0 - r, b = y0 + r;
      break;
    case Ring:
      l = x0 - r - w, rt = x0 + r + w, t = y0 - r - w, b = y0 + r + w;
      break;
    case RoundRect:
      l = x0, rt = x1, t = y0, b = y1;
      break;
    default:
      return Rect();
  }
  // One extra pixel for the anti-aliased edge.
  return {int(floorf(l)) - 1, int(floorf(t)) - 1, int(ceilf(rt)) + 2, int(ceilf(b)) + 2};
}

int Shape::bandExtents(Rect *out, int capacity) const {
  Rect b = bounds();
  if (b.empty()) return 0;
  int n = 0;
  for (int band = max(0, b.y0) / kBandRows; band * kBandRows < b.y1 && band < kBands && n < capacity; band++) {
    int by0 = max(b.y0, band * kBandRows), by1 = min(b.y1, (band + 1) * kBandRows);
    float m = r + 2;  // radius plus the anti-aliased edge and rounding
    switch (kind) {
      case Capsule: {
        // The part of the segment whose rows reach this band, widened.
        float dy = y1 - y0, t0 = 0, t1 = 1;
        if (fabsf(dy) > 1e-3f) {
          t0 = (by0 - m - y0) / dy, t1 = (by1 + m - y0) / dy;
          if (t0 > t1) std::swap(t0, t1);
          t0 = max(t0, 0.0f), t1 = min(t1, 1.0f);
          if (t0 > t1) continue;
        }
        float xa = x0 + (x1 - x0) * t0, xb = x0 + (x1 - x0) * t1;
        out[n++] = {int(floorf(min(xa, xb) - m)), by0, int(ceilf(max(xa, xb) + m)), by1};
        break;
      }
      case Circle:
      case Ring: {
        // Widest row of the band, and for a ring the hole it leaves.
        float ro = kind == Circle ? r + 2 : r + w + 2;
        float dy = (by0 <= y0 && y0 < by1) ? 0 : min(fabsf(by0 - y0), fabsf(by1 - y0));
        if (dy >= ro) continue;
        float outer = sqrtf(ro * ro - dy * dy);
        float ri = r - w - 2, far = max(fabsf(by0 - y0), fabsf(by1 - y0));
        if (kind == Ring && ri > 0 && far < ri) {
          float inner = sqrtf(ri * ri - far * far);
          out[n++] = {int(floorf(x0 - outer)), by0, int(ceilf(x0 - inner)), by1};
          if (n < capacity) out[n++] = {int(floorf(x0 + inner)), by0, int(ceilf(x0 + outer)), by1};
        } else {
          out[n++] = {int(floorf(x0 - outer)), by0, int(ceilf(x0 + outer)), by1};
        }
        break;
      }
      default:
        out[n++] = {b.x0, by0, b.x1, by1};
        break;
    }
  }
  return n;
}

namespace {

inline float clamp01(float v) { return v < 0 ? 0 : v > 1 ? 1 : v; }

// Per-call constants for blending one shape. Pixels are classified by
// squared distance first: only those in the one-pixel anti-aliased edge
// need a square root, the rest are fully inside or skipped.
struct Paint {
  uint32_t color;
  uint16_t solid;  // color as a byte-swapped RGB565 pixel
  uint8_t alpha;

  explicit Paint(const Shape &s)
      : color(s.color),
        solid(swap16(uint16_t(((s.color >> 8) & 0xF800) | ((s.color >> 5) & 0x07E0) | ((s.color >> 3) & 0x1F)))),
        alpha(s.alpha) {}

  inline void full(uint16_t &px) const { px = alpha == 255 ? solid : blend565(px, color, alpha); }
  // d: signed distance to the edge (negative inside), in pixels.
  inline void edge(uint16_t &px, float d) const {
    uint32_t a = uint32_t(clamp01(0.5f - d) * alpha + 0.5f);
    if (a) px = a >= 255 ? solid : blend565(px, color, a);
  }
};

// Disc-like shapes: inside when the squared distance is below in2, outside
// above out2, otherwise an edge pixel at distance sqrt(d2) - r.
inline void radialPixel(uint16_t &px, float d2, float r, float in2, float out2, const Paint &p) {
  if (d2 <= in2) p.full(px);
  else if (d2 < out2) p.edge(px, sqrtf(d2) - r);
}

void renderCapsule(const Shape &s, const Span &sp, const Rect &b) {
  const Paint p(s);
  const float dx = s.x1 - s.x0, dy = s.y1 - s.y0, len2 = dx * dx + dy * dy;
  const float inv = len2 > 0 ? 1 / len2 : 0;
  const float rin = max(s.r - 0.5f, 0.0f), in2 = rin * rin, out2 = (s.r + 0.5f) * (s.r + 0.5f);
  // Half-width of the run around the line on each row (wider for shallow lines).
  const float half = fabsf(dy) > 0.5f ? (s.r + 1.5f) * sqrtf(len2) / fabsf(dy) : 1e9f;
  for (int y = b.y0; y < b.y1; y++) {
    uint16_t *row = sp.dst + (y - sp.y) * sp.w - sp.x;  // indexed by screen x
    const float ey = y + 0.5f - s.y0;
    int xa = b.x0, xb = b.x1;
    if (half < 1e8f) {
      float xc = s.x0 + ey * dx / dy;
      xa = max(xa, int(floorf(xc - half)));
      xb = min(xb, int(ceilf(xc + half)) + 1);
    }
    // Walk the row: the projection onto the segment grows by dx per pixel.
    float ex = xa + 0.5f - s.x0, dot = ex * dx + ey * dy;
    for (int x = xa; x < xb; x++, ex += 1, dot += dx) {
      float t = clamp01(dot * inv);
      float qx = ex - t * dx, qy = ey - t * dy;
      radialPixel(row[x], qx * qx + qy * qy, s.r, in2, out2, p);
    }
  }
}

void renderCircle(const Shape &s, const Span &sp, const Rect &b) {
  const Paint p(s);
  const float rin = max(s.r - 0.5f, 0.0f), in2 = rin * rin, out2 = (s.r + 0.5f) * (s.r + 0.5f);
  for (int y = b.y0; y < b.y1; y++) {
    const float ey = y + 0.5f - s.y0;
    if (ey * ey >= out2) continue;
    float half = sqrtf(out2 - ey * ey) + 1;
    int xa = max(b.x0, int(floorf(s.x0 - half))), xb = min(b.x1, int(ceilf(s.x0 + half)));
    uint16_t *row = sp.dst + (y - sp.y) * sp.w - sp.x;
    float ex = xa + 0.5f - s.x0;
    for (int x = xa; x < xb; x++, ex += 1) radialPixel(row[x], ex * ex + ey * ey, s.r, in2, out2, p);
  }
}

void renderRing(const Shape &s, const Span &sp, const Rect &b) {
  const Paint p(s);
  const float ro = s.r + s.w, ri = s.r - s.w;
  const float outer2 = (ro + 0.5f) * (ro + 0.5f), solidOut2 = max(ro - 0.5f, 0.0f) * max(ro - 0.5f, 0.0f);
  const float inner2 = ri > 0.5f ? (ri - 0.5f) * (ri - 0.5f) : 0, solidIn2 = (ri + 0.5f) * (ri + 0.5f);
  for (int y = b.y0; y < b.y1; y++) {
    const float ey = y + 0.5f - s.y0, ey2 = ey * ey;
    if (ey2 >= outer2) continue;
    uint16_t *row = sp.dst + (y - sp.y) * sp.w - sp.x;
    // The left and right arcs crossing this row (one run if the row misses the hole).
    float outer = sqrtf(outer2 - ey2) + 1;
    float inner = ey2 < inner2 ? sqrtf(inner2 - ey2) - 1 : 0;
    int runs[2][2] = {{int(floorf(s.x0 - outer)), int(ceilf(s.x0 - inner))},
                      {int(floorf(s.x0 + inner)), int(ceilf(s.x0 + outer))}};
    if (inner <= 0) runs[0][1] = runs[1][0];
    for (auto &run : runs) {
      int xa = max(b.x0, run[0]), xb = min(b.x1, run[1]);
      float ex = xa + 0.5f - s.x0;
      for (int x = xa; x < xb; x++, ex += 1) {
        float d2 = ex * ex + ey2;
        if (d2 >= outer2 || d2 <= inner2) continue;
        if (d2 >= solidIn2 && d2 <= solidOut2) p.full(row[x]);
        else p.edge(row[x], fabsf(sqrtf(d2) - s.r) - s.w);
      }
    }
  }
}

void renderRoundRect(const Shape &s, const Span &sp, const Rect &b) {
  const Paint p(s);
  const float cx = (s.x0 + s.x1) / 2, cy = (s.y0 + s.y1) / 2;
  const float hx = (s.x1 - s.x0) / 2 - s.r, hy = (s.y1 - s.y0) / 2 - s.r;
  for (int y = b.y0; y < b.y1; y++) {
    uint16_t *row = sp.dst + (y - sp.y) * sp.w - sp.x;
    float qy = fabsf(y + 0.5f - cy) - hy;
    for (int x = b.x0; x < b.x1; x++) {
      float qx = fabsf(x + 0.5f - cx) - hx;
      float ox = max(qx, 0.0f), oy = max(qy, 0.0f);
      float d = (ox > 0 || oy > 0 ? sqrtf(ox * ox + oy * oy) : 0) + min(max(qx, qy), 0.0f) - s.r;
      if (d <= -0.5f) p.full(row[x]);
      else if (d < 0.5f) p.edge(row[x], d);
    }
  }
}

}  // namespace

void Shape::render(const Span &sp) const {
  Rect b = bounds().intersect(sp.rect());
  if (b.empty()) return;
  switch (kind) {
    case Capsule: renderCapsule(*this, sp, b); break;
    case Circle: renderCircle(*this, sp, b); break;
    case Ring: renderRing(*this, sp, b); break;
    case RoundRect: renderRoundRect(*this, sp, b); break;
    default: break;
  }
}

// ---- ShapeLayer

void ShapeLayer::invalidateShape(const Shape &s) {
  Rect r[kBands * 2];
  int n = s.bandExtents(r, kBands * 2);
  for (int i = 0; i < n; i++) invalidate(r[i]);
}

int ShapeLayer::add(const Shape &s) {
  if (count_ >= kMaxShapes) return -1;
  shapes_[count_] = s;
  invalidateShape(s);
  return count_++;
}

void ShapeLayer::set(int i, const Shape &s) {
  if (i < 0 || i >= count_) return;
  Shape &cur = shapes_[i];
  if (cur.kind == s.kind && cur.x0 == s.x0 && cur.y0 == s.y0 && cur.x1 == s.x1 && cur.y1 == s.y1 && cur.r == s.r &&
      cur.w == s.w && cur.color == s.color && cur.alpha == s.alpha) {
    return;  // unchanged: nothing to redraw
  }
  invalidateShape(cur);
  cur = s;
  invalidateShape(s);
}

void ShapeLayer::clear() {
  for (int i = 0; i < count_; i++) invalidateShape(shapes_[i]);
  count_ = 0;
}

Rect ShapeLayer::bounds() const {
  Rect r;
  for (int i = 0; i < count_; i++) r = r.unite(shapes_[i].bounds());
  return r;
}

void ShapeLayer::render(const Span &s) const {
  for (int i = 0; i < count_; i++) shapes_[i].render(s);
}

// ---- ImageLayer

bool ImageLayer::begin() {
  canvas_.setPsram(true);
  canvas_.setColorDepth(16);
  return canvas_.createSprite(kWidth, kHeight) != nullptr;
}

void ImageLayer::paint(const Layer &layer) {
  auto *buf = static_cast<uint16_t *>(canvas_.getBuffer());
  for (int y = 0; y < kHeight; y += 32) {
    Span s = {buf + y * kWidth, 0, y, kWidth, min(32, kHeight - y)};
    layer.render(s);
  }
  invalidate();
}

void ImageLayer::radialGradient(uint32_t inner, uint32_t outer, float radius) {
  auto *buf = static_cast<uint16_t *>(canvas_.getBuffer());
  float ir = inner >> 16 & 0xFF, ig = inner >> 8 & 0xFF, ib = inner & 0xFF;
  float orr = outer >> 16 & 0xFF, og = outer >> 8 & 0xFF, ob = outer & 0xFF;
  static const float kBayer[4] = {0.125f, 0.625f, 0.875f, 0.375f};
  for (int y = 0; y < kHeight; y++) {
    for (int x = 0; x < kWidth; x++) {
      float dx = x + 0.5f - kCenterX, dy = y + 0.5f - kCenterY;
      float t = clamp01(sqrtf(dx * dx + dy * dy) / radius);
      t = t * t * (3 - 2 * t);
      float d = kBayer[(y & 1) * 2 + (x & 1)];
      int r = min(31, int((ir + (orr - ir) * t) * 31 / 255 + d));
      int g = min(63, int((ig + (og - ig) * t) * 63 / 255 + d));
      int b = min(31, int((ib + (ob - ib) * t) * 31 / 255 + d));
      buf[y * kWidth + x] = swap16(r << 11 | g << 5 | b);
    }
  }
  invalidate();
}

void ImageLayer::render(const Span &s) const {
  auto *buf = static_cast<const uint16_t *>(const_cast<M5Canvas &>(canvas_).getBuffer());
  for (int r = 0; r < s.rows; r++) memcpy(s.dst + r * s.w, buf + (s.y + r) * kWidth + s.x, s.w * 2);
}

}  // namespace swd
