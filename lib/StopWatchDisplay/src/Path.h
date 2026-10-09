// Paths and transforms for swd::Engine, following SVG's model.
//
//   swd::Path hand;                       // defined once, local coordinates
//   hand.moveTo(0, 20).lineTo(-6, 0).lineTo(0, -180).lineTo(6, 0).close();
//   swd::PathItem item(hand);
//   item.setFill(0xFFFFFF);
//   layer.add(item);                      // a VectorLayer
//   item.setTransform(swd::Transform::rotate(deg).then(swd::Transform::translate(cx, cy)));
//
// A PathItem flattens its path's curves into line segments once, in local
// coordinates (within kFlatness px at the current scale), and only re-flattens
// when the path changes or the scale changes a lot; a new transform just
// moves the points. Fills use exact area coverage per pixel (the scanline method
// font renderers use), nonzero winding. Strokes use the distance to the
// nearest segment, with round caps and joins.
#pragma once

#include <esp_heap_caps.h>

#include <initializer_list>
#include <memory>
#include <new>
#include <vector>

#include "Vector.h"

namespace swd {

// Puts path geometry (commands, flattened segments, edges) in PSRAM: text
// alone can need a few KB per glyph, and internal RAM is needed for DMA.
template <typename T>
struct PsramAllocator {
  using value_type = T;
  PsramAllocator() = default;
  template <typename U>
  PsramAllocator(const PsramAllocator<U> &) {}
  T *allocate(size_t n) {
    void *p = heap_caps_malloc(n * sizeof(T), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!p) p = malloc(n * sizeof(T));  // no PSRAM: fall back to internal
    if (!p) throw std::bad_alloc();
    return static_cast<T *>(p);
  }
  void deallocate(T *p, size_t) { heap_caps_free(p); }
  template <typename U>
  bool operator==(const PsramAllocator<U> &) const { return true; }
  template <typename U>
  bool operator!=(const PsramAllocator<U> &) const { return false; }
};
template <typename T>
using PsramVector = std::vector<T, PsramAllocator<T>>;

// 2D affine transform, as in SVG: x' = a x + c y + e, y' = b x + d y + f.
struct Transform {
  float a = 1, b = 0, c = 0, d = 1, e = 0, f = 0;

  static Transform translate(float x, float y) { return {1, 0, 0, 1, x, y}; }
  static Transform scale(float sx, float sy) { return {sx, 0, 0, sy, 0, 0}; }
  static Transform scale(float s) { return scale(s, s); }
  // Clockwise in screen coordinates (y down), in degrees, as in SVG.
  static Transform rotate(float degrees);
  // Rotation about (cx, cy).
  static Transform rotate(float degrees, float cx, float cy);

  // This transform, then t (t applied after this one).
  Transform then(const Transform &t) const;
  void apply(float x, float y, float &ox, float &oy) const {
    ox = a * x + c * y + e;
    oy = b * x + d * y + f;
  }
  // Largest scale factor, for flattening tolerance.
  float maxScale() const;
  bool operator==(const Transform &o) const {
    return a == o.a && b == o.b && c == o.c && d == o.d && e == o.e && f == o.f;
  }
  bool operator!=(const Transform &o) const { return !(*this == o); }
};

class Path {
 public:
  Path &moveTo(float x, float y);
  Path &lineTo(float x, float y);
  Path &quadTo(float cx, float cy, float x, float y);
  Path &cubicTo(float c1x, float c1y, float c2x, float c2y, float x, float y);
  Path &close();

  // Shapes, as closed subpaths.
  Path &rect(float x, float y, float w, float h);
  Path &roundRect(float x, float y, float w, float h, float r);
  Path &circle(float cx, float cy, float r);
  // An open arc along a circle, from `startDeg` to `endDeg` (clockwise from
  // 12 o'clock, like watch angles). Starts a new subpath.
  Path &arc(float cx, float cy, float r, float startDeg, float endDeg);
  // A pie slice (closed).
  Path &pie(float cx, float cy, float r, float startDeg, float endDeg);

  void clear();
  bool empty() const { return verbs_.empty(); }

  enum Verb : uint8_t { Move, Line, Quad, Cubic, Close };
  const PsramVector<Verb> &verbs() const { return verbs_; }
  const PsramVector<float> &points() const { return points_; }  // x, y pairs

 private:
  void arcSegments(float cx, float cy, float r, float startDeg, float endDeg, bool connect);
  PsramVector<Verb> verbs_;
  PsramVector<float> points_;
};

class VectorLayer;

// Something a VectorLayer draws. Mutators call changed() around their
// update, so the old and new extents are both redrawn.
class VectorItem {
 public:
  virtual ~VectorItem() = default;
  virtual Rect bounds() const = 0;
  // Extent per band of kBandRows rows; defaults to the bounds.
  virtual int bandExtents(Rect *out, int capacity) const;
  // Render core: blend into the span.
  virtual void render(const Span &s) const = 0;

  void setVisible(bool visible);
  bool visible() const { return visible_; }

 protected:
  // Redraw this item's current area (call before and after a change).
  void invalidateArea();
  // Redraw specific screen areas (for items that change only in part).
  void invalidateRects(const Rect *rects, int n);

 private:
  friend class VectorLayer;
  VectorLayer *layer_ = nullptr;
  bool visible_ = true;
};

// A gradient color stop: position 0..1 along the gradient, 0xRRGGBB.
struct ColorStop {
  float offset;
  uint32_t color;
};

// What a fill or stroke is painted with: one color, or a linear or radial
// gradient in the item's local coordinates (it moves with the item's
// transform). Between stops colors are interpolated; beyond the ends the
// end colors continue (SVG's "pad"). Implicitly converts from a color.
struct Paint {
  enum Kind : uint8_t { Solid, Linear, Radial };
  static constexpr int kMaxStops = 6;

  Paint(uint32_t color = 0xFFFFFF, uint8_t alpha = 255) : color(color), alpha(alpha) {}
  // From (x0, y0) at offset 0 to (x1, y1) at offset 1.
  static Paint linear(float x0, float y0, float x1, float y1, std::initializer_list<ColorStop> stops,
                      uint8_t alpha = 255);
  // From the center (offset 0) out to radius r (offset 1).
  static Paint radial(float cx, float cy, float r, std::initializer_list<ColorStop> stops, uint8_t alpha = 255);

  Kind kind = Solid;
  uint32_t color;  // Solid
  uint8_t alpha;   // overall opacity
  float x0 = 0, y0 = 0, x1 = 0, y1 = 0, r = 0;
  ColorStop stops[kMaxStops] = {};
  uint8_t stopCount = 0;

  bool operator==(const Paint &o) const;
  bool operator!=(const Paint &o) const { return !(*this == o); }
};

enum class FillRule : uint8_t { NonZero, EvenOdd };
enum class Cap : uint8_t { Butt, Round, Square };
enum class Join : uint8_t { Miter, Round, Bevel };

// A filled and/or stroked path with a transform.
//
// Fills: nonzero or even-odd. Strokes are turned into outline polygons
// (one quad per segment plus joins and caps, as in SVG) and filled with the
// same exact-coverage filler. Stroke widths scale with the transform.
class PathItem : public VectorItem {
 public:
  static constexpr float kFlatness = 0.25f;  // max curve flattening error, px

  PathItem() = default;
  explicit PathItem(const Path &path) { setPath(path); }

  // The path is referenced, not copied: keep it alive and call setPath()
  // again after changing it.
  void setPath(const Path &path);
  void setTransform(const Transform &t);
  void setFill(const Paint &paint);
  void setFill(uint32_t color, uint8_t alpha) { setFill(Paint(color, alpha)); }
  void setFillRule(FillRule rule);
  void noFill();
  void setStroke(float width, const Paint &paint, Cap cap = Cap::Round, Join join = Join::Round,
                 float miterLimit = 4);
  void setStroke(float width, uint32_t color, uint8_t alpha) { setStroke(width, Paint(color, alpha)); }
  void noStroke();
  // Optional 256-entry curve applied to edge coverage (0..255 -> 0..255),
  // e.g. a gamma that strengthens thin light-on-dark text. Referenced, not
  // copied; nullptr for linear.
  void setCoverageCurve(const uint8_t *curve);

  Rect bounds() const override { return bounds_; }
  int bandExtents(Rect *out, int capacity) const override;
  void render(const Span &s) const override;

 private:
  struct Edge {
    float x0, y0, x1, y1;  // y0 <= y1
    int8_t dir;            // +1 down, -1 up
    float dxdy;            // x step per row
  };
  // A flattened segment in local coordinates.
  struct Segment {
    float x0, y0, x1, y1;
    uint8_t flags;
  };
  static constexpr uint8_t kStroked = 1;     // part of the outline (not the implicit closing edge of a fill)
  static constexpr uint8_t kStartsPath = 2;  // first segment of a subpath
  static constexpr uint8_t kCloses = 4;      // the explicit close() segment
  // A paint resolved for screen space: gradient parameters and color tables.
  struct PaintState {
    Paint paint;
    std::unique_ptr<uint32_t[]> lut;  // 256 x RGB888, then 256 x byte-swapped RGB565
    float g0 = 0, gx = 0, gy = 0;     // linear: t = g0 + gx * x + gy * y
    float cx = 0, cy = 0, invR = 0;   // radial: t = |p - c| / r
    void set(const Paint &p);
    void resolve(const Transform &t);  // gradient geometry -> screen space
  };

  void flatten();   // path -> local segments
  void rebuild();   // local segments + transform -> screen edges and bounds
  void buildStroke(const float *pts, int n, bool closed);
  void addPolygon(const float *xy, int n);  // to strokeEdges_, normalized orientation
  void renderEdges(const Span &s, const PsramVector<Edge> &edges, const PaintState &paint, bool evenOdd) const;
  static void pushEdge(PsramVector<Edge> &out, float x0, float y0, float x1, float y1);

  const Path *path_ = nullptr;
  Transform transform_;
  PsramVector<Segment> segments_;
  float flattenScale_ = 0;  // scale the segments were flattened for (0: stale)
  PaintState fill_, stroke_;
  FillRule fillRule_ = FillRule::NonZero;
  const uint8_t *curve_ = nullptr;
  bool hasFill_ = true, hasStroke_ = false;
  float strokeWidth_ = 0, miterLimit_ = 4;
  Cap cap_ = Cap::Round;
  Join join_ = Join::Round;
  PsramVector<Edge> fillEdges_;    // closed outlines, sorted by y0
  PsramVector<Edge> strokeEdges_;  // stroke outline polygons, sorted by y0
  Rect bounds_;
};

// One of the fast primitives (Shape) as a VectorItem.
class ShapeItem : public VectorItem {
 public:
  ShapeItem() = default;
  explicit ShapeItem(const Shape &s) : shape_(s) {}
  void set(const Shape &s);
  const Shape &get() const { return shape_; }

  Rect bounds() const override { return shape_.bounds(); }
  int bandExtents(Rect *out, int capacity) const override { return shape_.bandExtents(out, capacity); }
  void render(const Span &s) const override { shape_.render(s); }

 private:
  Shape shape_;
};

// Draws VectorItems in the order they were added (back to front). Items are
// referenced, not owned.
class VectorLayer : public Layer {
 public:
  static constexpr int kMaxItems = 64;
  bool add(VectorItem &item);

  Rect bounds() const override;
  void render(const Span &s) const override;

 private:
  friend class VectorItem;
  void invalidateItem(const VectorItem &item);
  void invalidateRect(const Rect &r) { invalidate(r); }
  VectorItem *items_[kMaxItems] = {};
  int count_ = 0;
};

}  // namespace swd
