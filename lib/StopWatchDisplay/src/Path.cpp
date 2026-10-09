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

// ---- Paint

Paint Paint::linear(float x0, float y0, float x1, float y1, std::initializer_list<ColorStop> stops, uint8_t alpha) {
  Paint p(stops.size() ? stops.begin()->color : 0xFFFFFF, alpha);
  p.kind = Linear;
  p.x0 = x0, p.y0 = y0, p.x1 = x1, p.y1 = y1;
  for (const ColorStop &st : stops) {
    if (p.stopCount < kMaxStops) p.stops[p.stopCount++] = st;
  }
  return p;
}

Paint Paint::radial(float cx, float cy, float r, std::initializer_list<ColorStop> stops, uint8_t alpha) {
  Paint p = linear(cx, cy, cx, cy, stops, alpha);
  p.kind = Radial;
  p.r = r;
  return p;
}

bool Paint::operator==(const Paint &o) const {
  if (kind != o.kind || alpha != o.alpha) return false;
  if (kind == Solid) return color == o.color;
  if (x0 != o.x0 || y0 != o.y0 || x1 != o.x1 || y1 != o.y1 || r != o.r || stopCount != o.stopCount) return false;
  for (int i = 0; i < stopCount; i++) {
    if (stops[i].offset != o.stops[i].offset || stops[i].color != o.stops[i].color) return false;
  }
  return true;
}

namespace {

inline uint16_t pack565(uint32_t rgb) {
  return swap16(uint16_t(((rgb >> 8) & 0xF800) | ((rgb >> 5) & 0x07E0) | ((rgb >> 3) & 0x1F)));
}

}  // namespace

void PathItem::PaintState::set(const Paint &p) {
  paint = p;
  if (p.kind == Paint::Solid || p.stopCount == 0) {
    lut.reset();
    return;
  }
  if (!lut) lut.reset(new uint32_t[512]);
  // 256 interpolated colors (RGB888), then the same as panel pixels.
  for (int i = 0; i < 256; i++) {
    float t = i / 255.0f;
    const ColorStop *a = &p.stops[0], *b = a;
    for (int k = 0; k < p.stopCount; k++) {
      if (p.stops[k].offset <= t) a = &p.stops[k];
      if (p.stops[k].offset >= t) {
        b = &p.stops[k];
        break;
      }
      b = &p.stops[k];
    }
    float span = b->offset - a->offset, f = span > 0 ? constrain((t - a->offset) / span, 0.0f, 1.0f) : 0;
    uint32_t rgb = 0;
    for (int sh = 0; sh <= 16; sh += 8) {
      float ca = a->color >> sh & 0xFF, cb = b->color >> sh & 0xFF;
      rgb |= uint32_t(ca + (cb - ca) * f + 0.5f) << sh;
    }
    lut[i] = rgb;
    lut[256 + i] = pack565(rgb);
  }
}

void PathItem::PaintState::resolve(const Transform &t) {
  if (paint.kind == Paint::Linear) {
    float ax, ay, bx, by;
    t.apply(paint.x0, paint.y0, ax, ay);
    t.apply(paint.x1, paint.y1, bx, by);
    float dx = bx - ax, dy = by - ay, len2 = dx * dx + dy * dy;
    gx = len2 > 0 ? dx / len2 : 0, gy = len2 > 0 ? dy / len2 : 0;
    g0 = -(ax * gx + ay * gy);
  } else if (paint.kind == Paint::Radial) {
    t.apply(paint.x0, paint.y0, cx, cy);
    float r = paint.r * t.maxScale();  // circles stay circles under uniform scale
    invR = r > 0 ? 1 / r : 0;
  }
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

void PathItem::setFill(const Paint &paint) {
  if (hasFill_ && fill_.paint == paint) return;
  invalidateArea();
  bool geometry = !hasFill_;
  fill_.set(paint);
  fill_.resolve(transform_);
  hasFill_ = true;
  if (geometry) rebuild();  // a paint change alone keeps the edges
  invalidateArea();
}

void PathItem::setFillRule(FillRule rule) {
  if (rule == fillRule_) return;
  fillRule_ = rule;
  invalidateArea();
}

void PathItem::noFill() {
  if (!hasFill_) return;
  invalidateArea();
  hasFill_ = false;
  rebuild();
  invalidateArea();
}

void PathItem::setStroke(float width, const Paint &paint, Cap cap, Join join, float miterLimit) {
  bool geometry = !hasStroke_ || strokeWidth_ != width || cap_ != cap || join_ != join || miterLimit_ != miterLimit;
  if (!geometry && stroke_.paint == paint) return;
  invalidateArea();
  stroke_.set(paint);
  stroke_.resolve(transform_);
  strokeWidth_ = width, cap_ = cap, join_ = join, miterLimit_ = miterLimit, hasStroke_ = width > 0;
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

void PathItem::setCoverageCurve(const uint8_t *curve) {
  if (curve == curve_) return;
  curve_ = curve;
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
  bool open = false, first = false;
  auto emit = [&](float x0, float y0, float x1, float y1, uint8_t flags) {
    if (first) flags |= kStartsPath, first = false;
    segments_.push_back({x0, y0, x1, y1, flags});
  };
  auto closeFill = [&]() {
    // Fills are implicitly closed (as in SVG); strokes only on close().
    if (open && (curX != startX || curY != startY)) emit(curX, curY, startX, startY, 0);
  };
  size_t p = 0;
  for (Path::Verb v : verbs) {
    switch (v) {
      case Path::Move:
        closeFill();
        startX = curX = pts[p], startY = curY = pts[p + 1];
        p += 2;
        open = true, first = true;
        break;
      case Path::Line:
        emit(curX, curY, pts[p], pts[p + 1], kStroked);
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
          emit(px, py, nx, ny, kStroked);
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
          emit(px, py, nx, ny, kStroked);
          px = nx, py = ny;
        }
        curX = x, curY = y;
        p += 6;
        break;
      }
      case Path::Close:
        if (curX != startX || curY != startY) emit(curX, curY, startX, startY, kStroked | kCloses);
        else if (!segments_.empty()) segments_.back().flags |= kCloses;
        curX = startX, curY = startY;
        open = false;
        break;
    }
  }
  closeFill();
}

void PathItem::pushEdge(PsramVector<Edge> &out, float x0, float y0, float x1, float y1) {
  if (y0 == y1) return;
  if (y0 < y1) out.push_back({x0, y0, x1, y1, 1, (x1 - x0) / (y1 - y0)});
  else out.push_back({x1, y1, x0, y0, -1, (x0 - x1) / (y0 - y1)});
}

// Adds a stroke piece as a closed polygon. All pieces get the same
// orientation, so where they overlap their windings add up instead of
// cancelling, and the nonzero fill gives their union.
void PathItem::addPolygon(const float *xy, int n) {
  float area = 0;
  for (int i = 0; i < n; i++) {
    int j = (i + 1) % n;
    area += xy[2 * i] * xy[2 * j + 1] - xy[2 * j] * xy[2 * i + 1];
  }
  for (int i = 0; i < n; i++) {
    int a = area >= 0 ? i : n - 1 - i, b = area >= 0 ? (i + 1) % n : (2 * n - 2 - i) % n;
    pushEdge(strokeEdges_, xy[2 * a], xy[2 * a + 1], xy[2 * b], xy[2 * b + 1]);
  }
}

// Stroke outline for one flattened polyline in screen space: a quad per
// segment, joins between segments, caps at open ends.
void PathItem::buildStroke(const float *p, int n, bool closed) {
  const float h = strokeWidth_ * transform_.maxScale() / 2;
  if (h <= 0 || n < 2) return;
  // Arc around (cx, cy) from angle a0 to a1 (radians, shorter way), as a fan.
  auto arcFan = [&](float cx, float cy, float a0, float a1) {
    float d = a1 - a0;
    while (d > PI) d -= 2 * PI;
    while (d < -PI) d += 2 * PI;
    int steps = constrain(int(ceilf(fabsf(d) * sqrtf(h) * 1.2f)), 1, 32);
    float poly[2 * 34];
    int k = 0;
    poly[k++] = cx, poly[k++] = cy;
    for (int i = 0; i <= steps; i++) {
      float a = a0 + d * i / steps;
      poly[k++] = cx + cosf(a) * h, poly[k++] = cy + sinf(a) * h;
    }
    addPolygon(poly, k / 2);
  };
  auto disc = [&](float cx, float cy) {
    arcFan(cx, cy, 0, PI * 0.999f);
    arcFan(cx, cy, PI, 2 * PI * 0.9995f);
  };

  const int segs = closed ? n : n - 1;
  for (int i = 0; i < segs; i++) {
    const float *a = p + 2 * i, *b = p + 2 * ((i + 1) % n);
    float dx = b[0] - a[0], dy = b[1] - a[1], len = sqrtf(dx * dx + dy * dy);
    if (len < 1e-4f) continue;
    float nx = -dy / len * h, ny = dx / len * h;
    float quad[8] = {a[0] + nx, a[1] + ny, b[0] + nx, b[1] + ny, b[0] - nx, b[1] - ny, a[0] - nx, a[1] - ny};
    addPolygon(quad, 4);
  }

  // Joins at interior vertices (all vertices of a closed polyline).
  for (int i = closed ? 0 : 1; i < (closed ? n : n - 1); i++) {
    const float *pv = p + 2 * ((i + n - 1) % n), *v = p + 2 * i, *nv = p + 2 * ((i + 1) % n);
    float ax = v[0] - pv[0], ay = v[1] - pv[1], bx = nv[0] - v[0], by = nv[1] - v[1];
    float la = sqrtf(ax * ax + ay * ay), lb = sqrtf(bx * bx + by * by);
    if (la < 1e-4f || lb < 1e-4f) continue;
    ax /= la, ay /= la, bx /= lb, by /= lb;
    float cross = ax * by - ay * bx, dot = ax * bx + ay * by;
    if (fabsf(cross) < 1e-6f && dot > 0) continue;  // straight on
    // The outer side of the turn is where the two offset edges open a gap.
    float side = cross > 0 ? -1 : 1;
    float nax = -ay * side, nay = ax * side, nbx = -by * side, nby = bx * side;
    float angle = acosf(constrain(dot, -1.0f, 1.0f));
    Join join = join_;
    if (h * angle < 0.5f) join = Join::Bevel;  // tiny gap: a bevel is indistinguishable
    if (join == Join::Miter && sqrtf(2 / max(1 + dot, 1e-6f)) > miterLimit_) join = Join::Bevel;
    if (join == Join::Round) {
      arcFan(v[0], v[1], atan2f(nay, nax), atan2f(nby, nbx));
    } else if (join == Join::Miter) {
      float k = h / (1 + dot);
      float poly[8] = {v[0], v[1], v[0] + nax * h, v[1] + nay * h, v[0] + (nax + nbx) * k, v[1] + (nay + nby) * k,
                       v[0] + nbx * h, v[1] + nby * h};
      addPolygon(poly, 4);
    } else {
      float tri[6] = {v[0], v[1], v[0] + nax * h, v[1] + nay * h, v[0] + nbx * h, v[1] + nby * h};
      addPolygon(tri, 3);
    }
  }

  // Caps at the ends of an open polyline.
  if (!closed && cap_ != Cap::Butt) {
    for (int end = 0; end < 2; end++) {
      const float *e = end ? p + 2 * (n - 1) : p, *in = end ? p + 2 * (n - 2) : p + 2;
      float dx = e[0] - in[0], dy = e[1] - in[1], len = sqrtf(dx * dx + dy * dy);
      if (len < 1e-4f) continue;
      dx /= len, dy /= len;  // pointing out of the line
      if (cap_ == Cap::Round) {
        disc(e[0], e[1]);
      } else {
        float nx = -dy * h, ny = dx * h, ox = dx * h, oy = dy * h;
        float quad[8] = {e[0] + nx, e[1] + ny, e[0] + nx + ox, e[1] + ny + oy,
                         e[0] - nx + ox, e[1] - ny + oy, e[0] - nx, e[1] - ny};
        addPolygon(quad, 4);
      }
    }
  }
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
  fill_.resolve(transform_);
  stroke_.resolve(transform_);

  std::vector<float> poly;  // current stroke polyline, screen space
  bool polyClosed = false;
  auto flushStroke = [&]() {
    if (poly.size() >= 4) buildStroke(poly.data(), int(poly.size() / 2), polyClosed);
    poly.clear();
    polyClosed = false;
  };
  for (const Segment &sg : segments_) {
    float x0, y0, x1, y1;
    transform_.apply(sg.x0, sg.y0, x0, y0);
    transform_.apply(sg.x1, sg.y1, x1, y1);
    if (hasFill_) pushEdge(fillEdges_, x0, y0, x1, y1);
    if (!hasStroke_ || !(sg.flags & kStroked)) continue;
    if (sg.flags & kStartsPath) flushStroke();
    if (poly.empty()) poly.insert(poly.end(), {x0, y0});
    if (sg.flags & kCloses) {
      polyClosed = true;  // the closing point equals the first: don't repeat it
      if (x1 != poly[0] || y1 != poly[1]) poly.insert(poly.end(), {x1, y1});
      flushStroke();
    } else {
      poly.insert(poly.end(), {x1, y1});
    }
  }
  flushStroke();

  auto byTop = [](const Edge &a, const Edge &b) { return a.y0 < b.y0; };
  std::sort(fillEdges_.begin(), fillEdges_.end(), byTop);
  std::sort(strokeEdges_.begin(), strokeEdges_.end(), byTop);
  float minX = 1e9f, minY = 1e9f, maxX = -1e9f, maxY = -1e9f;
  for (const auto *edges : {&fillEdges_, &strokeEdges_}) {
    for (const Edge &e : *edges) {
      minX = min(minX, min(e.x0, e.x1)), maxX = max(maxX, max(e.x0, e.x1));
      minY = min(minY, e.y0), maxY = max(maxY, e.y1);
    }
  }
  if (minX > maxX) return;
  bounds_ = {int(floorf(minX)) - 1, int(floorf(minY)) - 1, int(ceilf(maxX)) + 2, int(ceilf(maxY)) + 2};
}

int PathItem::bandExtents(Rect *out, int capacity) const {
  if (bounds_.empty()) return 0;
  // Per band, the x-range of the edges' parts that reach it: fills and stroke
  // outlines both lie between their edges.
  const float m = 1.5f;
  int n = 0;
  for (int band = max(0, bounds_.y0) / kBandRows; band * kBandRows < bounds_.y1 && band < kBands && n < capacity;
       band++) {
    float by0 = band * kBandRows - m, by1 = (band + 1) * kBandRows + m, lo = 1e9f, hi = -1e9f;
    for (const auto *edges : {&fillEdges_, &strokeEdges_}) {
      for (const Edge &e : *edges) {
        if (e.y1 < by0 || e.y0 > by1) continue;
        float t0 = max(by0, e.y0), t1 = min(by1, e.y1);
        float xa = e.x0 + (t0 - e.y0) * e.dxdy, xb = e.x0 + (t1 - e.y0) * e.dxdy;
        lo = min(lo, min(xa, xb)), hi = max(hi, max(xa, xb));
      }
    }
    if (lo > hi) continue;
    out[n++] = {int(floorf(lo - m)), max(bounds_.y0, band * kBandRows), int(ceilf(hi + m)) + 1,
                min(bounds_.y1, (band + 1) * kBandRows)};
  }
  return n;
}

void PathItem::render(const Span &s) const {
  if (hasFill_) renderEdges(s, fillEdges_, fill_, fillRule_ == FillRule::EvenOdd);
  if (hasStroke_) renderEdges(s, strokeEdges_, stroke_, false);
}

namespace {

// Area-coverage accumulator for one row (render core only; also used by
// ImageLayer::paint on the main core, which never overlaps a draw).
float gAccum[kWidth + 2];

// Adds one edge's signed area to the accumulator for the row band
// [rowTop, rowTop + 1), with x already relative to the span start and the
// accumulator width w. Coverage of a pixel is then the running sum.
// (The method used by font-rs and stb_truetype.) Widens [lo, hi) to the
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

// Fills the area enclosed by `edges` with `paint`: area coverage per pixel
// from the accumulated edge contributions, then nonzero (|winding| capped at
// 1) or even-odd (winding folded into 0..1).
void PathItem::renderEdges(const Span &s, const PsramVector<Edge> &edges, const PaintState &ps, bool evenOdd) const {
  Rect b = bounds_.intersect(s.rect());
  if (b.empty() || edges.empty()) return;
  const Paint &paint = ps.paint;
  const bool gradient = ps.lut != nullptr;
  const uint16_t solid = pack565(paint.color);
  const int w = b.x1 - b.x0;
  for (int y = b.y0; y < b.y1; y++) {
    const float top = y;
    // Only the columns this row's edge pieces touch get accumulator
    // entries; coverage between them comes from the running sum.
    int lo = w + 2, hi = 0;
    bool any = false;
    for (const Edge &e : edges) {
      if (e.y0 >= top + 1) break;  // sorted by y0
      if (e.y1 <= top) continue;
      if (!any) {
        memset(gAccum, 0, (w + 2) * sizeof(float));
        any = true;
      }
      accumulateEdge(e.x0 - b.x0, e.y0, e.y1, e.dxdy, e.dir, top, w, lo, hi);
    }
    if (!any || lo >= hi) continue;
    uint16_t *row = s.dst + (y - s.y) * s.w + (b.x0 - s.x);
    const float py = y + 0.5f;
    // Linear gradients step by gx per pixel; radial ones need a distance.
    float t = ps.g0 + ps.gx * (b.x0 + lo + 0.5f) + ps.gy * py;
    float acc = 0;
    for (int x = lo; x < min(hi, w); x++, t += ps.gx) {
      acc += gAccum[x];
      float cov = fabsf(acc);
      if (evenOdd) {
        cov = fmodf(cov, 2.0f);
        if (cov > 1) cov = 2 - cov;
      } else if (cov > 1) {
        cov = 1;
      }
      uint32_t a = uint32_t(cov * 255 + 0.5f);
      if (curve_) a = curve_[a];
      a = (a * paint.alpha + 127) / 255;
      if (!a) continue;
      if (!gradient) {
        row[x] = a >= 255 ? solid : blend565(row[x], paint.color, a);
        continue;
      }
      float tt = t;
      if (paint.kind == Paint::Radial) {
        float dx = b.x0 + x + 0.5f - ps.cx, dy = py - ps.cy;
        tt = sqrtf(dx * dx + dy * dy) * ps.invR;
      }
      int i = tt <= 0 ? 0 : tt >= 1 ? 255 : int(tt * 255 + 0.5f);
      row[x] = a >= 255 ? uint16_t(ps.lut[256 + i]) : blend565(row[x], ps.lut[i], a);
    }
  }
}

}  // namespace swd
