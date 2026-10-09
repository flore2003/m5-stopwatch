// Paths and transforms for swd::Engine, following SVG's model.
//
//   swd::Path hand;                       // defined once, local coordinates
//   hand.moveTo(0, 20).lineTo(-6, 0).lineTo(0, -180).lineTo(6, 0).close();
//   swd::PathItem item(hand);
//   item.setFill(0xFFFFFF);
//   layer.add(item);                      // a VectorLayer
//   item.setTransform(swd::Transform::rotate(deg).then(swd::Transform::translate(cx, cy)));
//
// A PathItem transforms its path and flattens curves into line segments in
// screen space (within kFlatness px) whenever its path or transform
// changes. Fills use exact area coverage per pixel (the scanline method
// font renderers use), nonzero winding. Strokes use the distance to the
// nearest segment, with round caps and joins.
#pragma once

#include <vector>

#include "Vector.h"

namespace swd {

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
  const std::vector<Verb> &verbs() const { return verbs_; }
  const std::vector<float> &points() const { return points_; }  // x, y pairs

 private:
  void arcSegments(float cx, float cy, float r, float startDeg, float endDeg, bool connect);
  std::vector<Verb> verbs_;
  std::vector<float> points_;
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

 private:
  friend class VectorLayer;
  VectorLayer *layer_ = nullptr;
  bool visible_ = true;
};

// A filled and/or stroked path with a transform.
class PathItem : public VectorItem {
 public:
  static constexpr float kFlatness = 0.25f;  // max curve flattening error, px

  PathItem() = default;
  explicit PathItem(const Path &path) { setPath(path); }

  // The path is referenced, not copied: keep it alive and call setPath()
  // again after changing it.
  void setPath(const Path &path);
  void setTransform(const Transform &t);
  void setFill(uint32_t color, uint8_t alpha = 255);
  void noFill();
  void setStroke(float width, uint32_t color, uint8_t alpha = 255);
  void noStroke();

  Rect bounds() const override { return bounds_; }
  int bandExtents(Rect *out, int capacity) const override;
  void render(const Span &s) const override;

 private:
  struct Edge {
    float x0, y0, x1, y1;  // y0 <= y1 for fills; original order for strokes
    int8_t dir;            // +1 down, -1 up (fills)
  };
  void rebuild();
  void renderFill(const Span &s) const;
  void renderStroke(const Span &s) const;

  const Path *path_ = nullptr;
  Transform transform_;
  uint32_t fill_ = 0xFFFFFF, stroke_ = 0xFFFFFF;
  uint8_t fillAlpha_ = 255, strokeAlpha_ = 255;
  bool hasFill_ = true, hasStroke_ = false;
  float strokeHalf_ = 0;
  std::vector<Edge> fillEdges_;    // closed outlines, sorted by y0
  std::vector<Edge> strokeEdges_;  // polylines as drawn
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
  VectorItem *items_[kMaxItems] = {};
  int count_ = 0;
};

}  // namespace swd
