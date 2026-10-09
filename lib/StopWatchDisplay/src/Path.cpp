#include "Path.h"

#include <algorithm>

namespace swd {

// ---- Transform

Transform Transform::rotate(float degrees) {
  float r = degrees * PI / 180, s = sinf(r), c = cosf(r);
  return {c, s, -s, c, 0, 0};
}

Transform Transform::rotate(float degrees, float cx, float cy) {
  return translate(-cx, -cy).then(rotate(degrees)).then(translate(cx, cy));
}

Transform Transform::then(const Transform &t) const {
  return {t.a * a + t.c * b,     t.b * a + t.d * b,     t.a * c + t.c * d,
          t.b * c + t.d * d,     t.a * e + t.c * f + t.e, t.b * e + t.d * f + t.f};
}

float Transform::maxScale() const { return max(sqrtf(a * a + b * b), sqrtf(c * c + d * d)); }

// ---- Path

Path &Path::moveTo(float x, float y) {
  verbs_.push_back(Move);
  points_.insert(points_.end(), {x, y});
  return *this;
}

Path &Path::lineTo(float x, float y) {
  verbs_.push_back(Line);
  points_.insert(points_.end(), {x, y});
  return *this;
}

Path &Path::quadTo(float cx, float cy, float x, float y) {
  verbs_.push_back(Quad);
  points_.insert(points_.end(), {cx, cy, x, y});
  return *this;
}

Path &Path::cubicTo(float c1x, float c1y, float c2x, float c2y, float x, float y) {
  verbs_.push_back(Cubic);
  points_.insert(points_.end(), {c1x, c1y, c2x, c2y, x, y});
  return *this;
}

Path &Path::close() {
  verbs_.push_back(Close);
  return *this;
}

void Path::clear() {
  verbs_.clear();
  points_.clear();
}

Path &Path::rect(float x, float y, float w, float h) {
  return moveTo(x, y).lineTo(x + w, y).lineTo(x + w, y + h).lineTo(x, y + h).close();
}

Path &Path::roundRect(float x, float y, float w, float h, float r) {
  r = min(r, min(w, h) / 2);
  const float k = r * 0.5523f;  // cubic approximation of a quarter circle
  moveTo(x + r, y).lineTo(x + w - r, y);
  cubicTo(x + w - r + k, y, x + w, y + r - k, x + w, y + r).lineTo(x + w, y + h - r);
  cubicTo(x + w, y + h - r + k, x + w - r + k, y + h, x + w - r, y + h).lineTo(x + r, y + h);
  cubicTo(x + r - k, y + h, x, y + h - r + k, x, y + h - r).lineTo(x, y + r);
  return cubicTo(x, y + r - k, x + r - k, y, x + r, y).close();
}

Path &Path::circle(float cx, float cy, float r) {
  arcSegments(cx, cy, r, 0, 360, false);
  return close();
}

Path &Path::arc(float cx, float cy, float r, float startDeg, float endDeg) {
  arcSegments(cx, cy, r, startDeg, endDeg, false);
  return *this;
}

Path &Path::pie(float cx, float cy, float r, float startDeg, float endDeg) {
  moveTo(cx, cy);
  arcSegments(cx, cy, r, startDeg, endDeg, true);
  return close();
}

// Cubic segments of at most 90 degrees along a circle. Angles are clockwise
// from 12 o'clock.
void Path::arcSegments(float cx, float cy, float r, float startDeg, float endDeg, bool connect) {
  int n = max(1, int(ceilf(fabsf(endDeg - startDeg) / 90)));
  float step = (endDeg - startDeg) / n * PI / 180, a0 = startDeg * PI / 180;
  float k = 4.0f / 3 * tanf(step / 4) * r;
  auto pt = [&](float a, float &x, float &y) { x = cx + sinf(a) * r, y = cy - cosf(a) * r; };
  float x, y;
  pt(a0, x, y);
  if (connect) lineTo(x, y);
  else moveTo(x, y);
  for (int i = 0; i < n; i++) {
    float a1 = a0 + step, x1, y1;
    pt(a1, x1, y1);
    // Tangents (clockwise from 12): d/da (sin a, -cos a) = (cos a, sin a).
    cubicTo(x + cosf(a0) * k, y + sinf(a0) * k, x1 - cosf(a1) * k, y1 - sinf(a1) * k, x1, y1);
    a0 = a1, x = x1, y = y1;
  }
}

// ---- VectorItem / VectorLayer

int VectorItem::bandExtents(Rect *out, int capacity) const {
  Rect b = bounds();
  if (b.empty()) return 0;
  int n = 0;
  for (int band = max(0, b.y0) / kBandRows; band * kBandRows < b.y1 && band < kBands && n < capacity; band++) {
    out[n++] = {b.x0, max(b.y0, band * kBandRows), b.x1, min(b.y1, (band + 1) * kBandRows)};
  }
  return n;
}

void VectorItem::invalidateArea() {
  if (layer_ && visible_) layer_->invalidateItem(*this);
}

void VectorItem::invalidateRects(const Rect *rects, int n) {
  if (!layer_ || !visible_) return;
  for (int i = 0; i < n; i++) layer_->invalidateRect(rects[i]);
}

void VectorItem::setVisible(bool visible) {
  if (visible == visible_) return;
  if (visible_) invalidateArea();
  visible_ = visible;
  invalidateArea();
}

bool VectorLayer::add(VectorItem &item) {
  if (count_ >= kMaxItems) return false;
  item.layer_ = this;
  items_[count_++] = &item;
  item.invalidateArea();
  return true;
}

void VectorLayer::invalidateItem(const VectorItem &item) {
  Rect r[kBands * 2];
  int n = item.bandExtents(r, kBands * 2);
  for (int i = 0; i < n; i++) invalidate(r[i]);
}

Rect VectorLayer::bounds() const {
  Rect r;
  for (int i = 0; i < count_; i++) {
    if (items_[i]->visible_) r = r.unite(items_[i]->bounds());
  }
  return r;
}

void VectorLayer::render(const Span &s) const {
  Rect sr = s.rect();
  for (int i = 0; i < count_; i++) {
    const VectorItem *it = items_[i];
    if (it->visible_ && it->bounds().intersects(sr)) it->render(s);
  }
}

void ShapeItem::set(const Shape &s) {
  invalidateArea();
  shape_ = s;
  invalidateArea();
}

// ---- PathItem

void PathItem::setPath(const Path &path) {
  invalidateArea();
  path_ = &path;
  flattenScale_ = 0;
  rebuild();
  invalidateArea();
}

void PathItem::setTransform(const Transform &t) {
  if (t == transform_ && path_) return;
  invalidateArea();
  transform_ = t;
  rebuild();
  invalidateArea();
}

void PathItem::setFill(uint32_t color, uint8_t alpha) {
  if (hasFill_ && fill_ == color && fillAlpha_ == alpha) return;
  invalidateArea();
  bool geometry = !hasFill_;
  fill_ = color, fillAlpha_ = alpha, hasFill_ = true;
  if (geometry) rebuild();  // a color change alone keeps the edges
  invalidateArea();
}

void PathItem::setCoverageCurve(const uint8_t *curve) {
  if (curve == curve_) return;
  curve_ = curve;
  invalidateArea();
}

void PathItem::noFill() {
  if (!hasFill_) return;
  invalidateArea();
  hasFill_ = false;
  rebuild();
  invalidateArea();
}

void PathItem::setStroke(float width, uint32_t color, uint8_t alpha) {
  invalidateArea();
  bool geometry = !hasStroke_ || strokeHalf_ != width / 2;
  strokeHalf_ = width / 2, stroke_ = color, strokeAlpha_ = alpha, hasStroke_ = width > 0;
  if (geometry) rebuild();
  invalidateArea();
}

void PathItem::noStroke() {
  if (!hasStroke_) return;
  invalidateArea();
  hasStroke_ = false;
  rebuild();
  invalidateArea();
}

// Flatten the path into local line segments, fine enough for the current
// scale (tolerance kFlatness in screen pixels).
void PathItem::flatten() {
  segments_.clear();
  flattenScale_ = max(transform_.maxScale(), 1e-3f);
  if (!path_) return;
  const auto &verbs = path_->verbs();
  const auto &pts = path_->points();
  const float tol = kFlatness / flattenScale_;

  float startX = 0, startY = 0, curX = 0, curY = 0;
  bool open = false;
  auto emit = [&](float x0, float y0, float x1, float y1, bool stroked) {
    segments_.push_back({x0, y0, x1, y1, stroked});
  };
  auto closeFill = [&]() {
    // Fills are implicitly closed (as in SVG); strokes only on close().
    if (open && (curX != startX || curY != startY)) emit(curX, curY, startX, startY, false);
  };
  size_t p = 0;
  for (Path::Verb v : verbs) {
    switch (v) {
      case Path::Move:
        closeFill();
        startX = curX = pts[p], startY = curY = pts[p + 1];
        p += 2;
        open = true;
        break;
      case Path::Line:
        emit(curX, curY, pts[p], pts[p + 1], true);
        curX = pts[p], curY = pts[p + 1];
        p += 2;
        break;
      case Path::Quad: {
        float cx = pts[p], cy = pts[p + 1], x = pts[p + 2], y = pts[p + 3];
        float ddx = curX - 2 * cx + x, ddy = curY - 2 * cy + y;
        int n = constrain(int(ceilf(sqrtf(sqrtf(ddx * ddx + ddy * ddy) / (4 * tol)))), 1, 64);
        float px = curX, py = curY;
        for (int i = 1; i <= n; i++) {
          float t = float(i) / n, u = 1 - t;
          float nx = u * u * curX + 2 * u * t * cx + t * t * x, ny = u * u * curY + 2 * u * t * cy + t * t * y;
          emit(px, py, nx, ny, true);
          px = nx, py = ny;
        }
        curX = x, curY = y;
        p += 4;
        break;
      }
      case Path::Cubic: {
        float c1x = pts[p], c1y = pts[p + 1], c2x = pts[p + 2], c2y = pts[p + 3], x = pts[p + 4], y = pts[p + 5];
        float d1x = curX - 2 * c1x + c2x, d1y = curY - 2 * c1y + c2y;
        float d2x = c1x - 2 * c2x + x, d2y = c1y - 2 * c2y + y;
        float dd = max(sqrtf(d1x * d1x + d1y * d1y), sqrtf(d2x * d2x + d2y * d2y));
        int n = constrain(int(ceilf(sqrtf(0.75f * dd / tol))), 1, 64);
        float px = curX, py = curY;
        for (int i = 1; i <= n; i++) {
          float t = float(i) / n, u = 1 - t;
          float a = u * u * u, b = 3 * u * u * t, c = 3 * u * t * t, d = t * t * t;
          float nx = a * curX + b * c1x + c * c2x + d * x, ny = a * curY + b * c1y + c * c2y + d * y;
          emit(px, py, nx, ny, true);
          px = nx, py = ny;
        }
        curX = x, curY = y;
        p += 6;
        break;
      }
      case Path::Close:
        if (curX != startX || curY != startY) emit(curX, curY, startX, startY, true);
        curX = startX, curY = startY;
        open = false;
        break;
    }
  }
  closeFill();
}

// Transform the local segments into screen-space edges. Re-flattens first
// if the scale moved far from the one the segments were made for.
void PathItem::rebuild() {
  fillEdges_.clear();
  strokeEdges_.clear();
  bounds_ = Rect();
  if (!path_) return;
  float scale = transform_.maxScale();
  if (flattenScale_ == 0 || scale > flattenScale_ * 1.25f || scale < flattenScale_ * 0.5f) flatten();

  float minX = 1e9f, minY = 1e9f, maxX = -1e9f, maxY = -1e9f;
  for (const Segment &sg : segments_) {
    float x0, y0, x1, y1;
    transform_.apply(sg.x0, sg.y0, x0, y0);
    transform_.apply(sg.x1, sg.y1, x1, y1);
    minX = min(minX, min(x0, x1)), maxX = max(maxX, max(x0, x1));
    minY = min(minY, min(y0, y1)), maxY = max(maxY, max(y0, y1));
    if (hasStroke_ && sg.stroked) strokeEdges_.push_back({x0, y0, x1, y1, 0});
    if (hasFill_ && y0 != y1) {
      if (y0 < y1) fillEdges_.push_back({x0, y0, x1, y1, 1, (x1 - x0) / (y1 - y0)});
      else fillEdges_.push_back({x1, y1, x0, y0, -1, (x0 - x1) / (y0 - y1)});
    }
  }
  std::sort(fillEdges_.begin(), fillEdges_.end(), [](const Edge &a, const Edge &b) { return a.y0 < b.y0; });
  if (minX > maxX) return;
  float m = hasStroke_ ? strokeHalf_ + 1 : 1;
  bounds_ = {int(floorf(minX - m)), int(floorf(minY - m)), int(ceilf(maxX + m)) + 1, int(ceilf(maxY + m)) + 1};
}

int PathItem::bandExtents(Rect *out, int capacity) const {
  if (bounds_.empty()) return 0;
  // Per band, the x-range of the edges' parts that reach it: a fill lies
  // between its edges, and strokes are widened by their half width.
  const float m = (hasStroke_ ? strokeHalf_ : 0) + 1.5f;
  int n = 0;
  for (int band = max(0, bounds_.y0) / kBandRows; band * kBandRows < bounds_.y1 && band < kBands && n < capacity;
       band++) {
    float by0 = band * kBandRows - m, by1 = (band + 1) * kBandRows + m, lo = 1e9f, hi = -1e9f;
    auto take = [&](const Edge &e) {
      float ey0 = min(e.y0, e.y1), ey1 = max(e.y0, e.y1);
      if (ey1 < by0 || ey0 > by1) return;
      float xa = e.x0, xb = e.x1;
      if (e.y1 != e.y0) {
        float t0 = constrain((by0 - e.y0) / (e.y1 - e.y0), 0.0f, 1.0f), t1 = constrain((by1 - e.y0) / (e.y1 - e.y0), 0.0f, 1.0f);
        xa = e.x0 + (e.x1 - e.x0) * t0, xb = e.x0 + (e.x1 - e.x0) * t1;
      }
      lo = min(lo, min(xa, xb)), hi = max(hi, max(xa, xb));
    };
    for (const Edge &e : fillEdges_) take(e);
    for (const Edge &e : strokeEdges_) take(e);
    if (lo > hi) continue;
    out[n++] = {int(floorf(lo - m)), max(bounds_.y0, band * kBandRows), int(ceilf(hi + m)) + 1,
                min(bounds_.y1, (band + 1) * kBandRows)};
  }
  return n;
}

void PathItem::render(const Span &s) const {
  if (hasFill_) renderFill(s);
  if (hasStroke_) renderStroke(s);
}

namespace {

// Area-coverage accumulator for one row (render core only; also used by
// ImageLayer::paint on the main core, which never overlaps a draw).
float gAccum[kWidth + 2];

// Adds one edge's signed area to the accumulator for the row band
// [rowTop, rowTop + 1), with x already relative to the span start (offset
// xOff) and the accumulator width w. Coverage of a pixel is then the running
// sum. (The method used by font-rs and stb_truetype.) Widens [lo, hi) to the
// columns this row's piece of the edge touches. Hot: no divisions and no
// floorf (x is clamped to >= 0, where a cast is a floor).
inline void accumulateEdge(float x0, float y0, float y1, float dxdy, int8_t dir, float rowTop, int w, int &touchLo,
                           int &touchHi) {
  float top = max(y0, rowTop), bot = min(y1, rowTop + 1);
  if (bot <= top) return;
  float xa = x0 + (top - y0) * dxdy, xb = xa + (bot - top) * dxdy;
  float d = (bot - top) * dir;
  // Left of the span: the whole contribution lands in column 0; right of it: none.
  const float fw = float(w);
  xa = xa < 0 ? 0 : xa > fw ? fw : xa;
  xb = xb < 0 ? 0 : xb > fw ? fw : xb;
  float lo = min(xa, xb), hi = max(xa, xb);
  int loI = int(lo);
  float loF = float(loI);
  int hiI = int(hi);
  if (float(hiI) < hi) hiI++;  // ceil
  touchLo = min(touchLo, loI), touchHi = max(touchHi, hiI + 1);
  if (hiI <= loI + 1) {
    // Within one pixel: split by the average x.
    float xm = 0.5f * (xa + xb) - loF;
    gAccum[loI] += d - d * xm;
    gAccum[loI + 1] += d * xm;
  } else {
    float inv = 1 / (hi - lo), lf = lo - loF;
    float a0 = 0.5f * inv * (1 - lf) * (1 - lf);
    float hf = hi - float(hiI) + 1;
    float am = 0.5f * inv * hf * hf;
    gAccum[loI] += d * a0;
    if (hiI == loI + 2) {
      gAccum[loI + 1] += d * (1 - a0 - am);
    } else {
      float a1 = inv * (1.5f - lf);
      gAccum[loI + 1] += d * (a1 - a0);
      for (int i = loI + 2; i < hiI - 1; i++) gAccum[i] += d * inv;
      float a2 = a1 + (hiI - loI - 3) * inv;
      gAccum[hiI - 1] += d * (1 - a2 - am);
    }
    gAccum[hiI] += d * am;
  }
}

}  // namespace

void PathItem::renderFill(const Span &s) const {
  Rect b = bounds_.intersect(s.rect());
  if (b.empty() || fillEdges_.empty()) return;
  const uint16_t solid =
      swap16(uint16_t(((fill_ >> 8) & 0xF800) | ((fill_ >> 5) & 0x07E0) | ((fill_ >> 3) & 0x1F)));
  const int w = b.x1 - b.x0;
  for (int y = b.y0; y < b.y1; y++) {
    const float top = y;
    // Only the columns this row's edge pieces touch get accumulator
    // entries; coverage between them comes from the running sum.
    int lo = w + 2, hi = 0;
    bool any = false;
    for (const Edge &e : fillEdges_) {
      if (e.y0 >= top + 1) break;  // sorted by y0
      if (e.y1 <= top) continue;
      if (!any) {
        memset(gAccum, 0, (w + 2) * sizeof(float));
        any = true;
      }
      accumulateEdge(e.x0 - b.x0, e.y0, e.y1, e.dxdy, e.dir, top, w, lo, hi);
    }
    if (!any || lo >= hi) continue;
    // Coverage is the running sum; past the rightmost touched column it's
    // back to 0 (every edge's contribution has been summed by then).
    uint16_t *row = s.dst + (y - s.y) * s.w + (b.x0 - s.x);
    float acc = 0;
    for (int x = lo; x < min(hi, w); x++) {
      acc += gAccum[x];
      uint32_t a = uint32_t(min(fabsf(acc), 1.0f) * 255 + 0.5f);
      if (curve_) a = curve_[a];
      a = (a * fillAlpha_ + 127) / 255;
      if (a >= 255) row[x] = solid;
      else if (a) row[x] = blend565(row[x], fill_, a);
    }
  }
}

void PathItem::renderStroke(const Span &s) const {
  Rect b = bounds_.intersect(s.rect());
  if (b.empty() || strokeEdges_.empty()) return;
  const float r = strokeHalf_, in2 = max(r - 0.5f, 0.0f) * max(r - 0.5f, 0.0f), out2 = (r + 0.5f) * (r + 0.5f);
  const uint16_t solid =
      swap16(uint16_t(((stroke_ >> 8) & 0xF800) | ((stroke_ >> 5) & 0x07E0) | ((stroke_ >> 3) & 0x1F)));
  for (int y = b.y0; y < b.y1; y++) {
    const float py = y + 0.5f;
    // Segments near this row, and the x-range they can affect.
    const Edge *near[64];
    int n = 0;
    float lo = 1e9f, hi = -1e9f;
    for (const Edge &e : strokeEdges_) {
      if (min(e.y0, e.y1) - r - 1 > py || max(e.y0, e.y1) + r + 1 < py) continue;
      if (n < 64) near[n++] = &e;
      lo = min(lo, min(e.x0, e.x1)), hi = max(hi, max(e.x0, e.x1));
    }
    if (!n) continue;
    int xa = max(b.x0, int(floorf(lo - r - 1))), xb = min(b.x1, int(ceilf(hi + r + 1)) + 1);
    uint16_t *row = s.dst + (y - s.y) * s.w - s.x;
    for (int x = xa; x < xb; x++) {
      float px = x + 0.5f, best = 1e9f;
      for (int i = 0; i < n; i++) {
        const Edge &e = *near[i];
        float dx = e.x1 - e.x0, dy = e.y1 - e.y0, ex = px - e.x0, ey = py - e.y0;
        float len2 = dx * dx + dy * dy;
        float t = len2 > 0 ? constrain((ex * dx + ey * dy) / len2, 0.0f, 1.0f) : 0;
        float qx = ex - t * dx, qy = ey - t * dy;
        best = min(best, qx * qx + qy * qy);
      }
      if (best >= out2) continue;
      if (best <= in2) {
        row[x] = strokeAlpha_ == 255 ? solid : blend565(row[x], stroke_, strokeAlpha_);
      } else {
        uint32_t a = uint32_t(constrain(r + 0.5f - sqrtf(best), 0.0f, 1.0f) * strokeAlpha_ + 0.5f);
        if (a) row[x] = blend565(row[x], stroke_, a);
      }
    }
  }
}

}  // namespace swd
