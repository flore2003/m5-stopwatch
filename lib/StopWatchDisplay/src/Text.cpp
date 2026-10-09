#include "Text.h"

namespace swd {

namespace {

const GlyphData *findGlyph(const FontData &f, uint32_t cp) {
  int lo = 0, hi = f.glyphCount - 1;
  while (lo <= hi) {
    int mid = (lo + hi) / 2;
    if (f.glyphs[mid].codepoint == cp) return &f.glyphs[mid];
    if (f.glyphs[mid].codepoint < cp) lo = mid + 1;
    else hi = mid - 1;
  }
  return nullptr;
}

// Next codepoint from UTF-8 (invalid bytes come out as '?').
uint32_t nextCodepoint(const char *&p) {
  uint8_t c = *p++;
  if (c < 0x80) return c;
  int extra = c >= 0xF0 ? 3 : c >= 0xE0 ? 2 : c >= 0xC0 ? 1 : -1;
  if (extra < 0) return '?';
  uint32_t cp = c & (0x3F >> extra);
  while (extra-- > 0) {
    if ((*p & 0xC0) != 0x80) return '?';
    cp = cp << 6 | (*p++ & 0x3F);
  }
  return cp;
}

}  // namespace

namespace {

// The glyph for cp, preferring its tabular form if asked; falls back to '?'.
const GlyphData *lookup(const FontData &f, uint32_t &cp, bool tabular) {
  if (tabular && cp < 0x100) {
    if (const GlyphData *g = findGlyph(f, 0xE000 + cp)) {
      cp = 0xE000 + cp;
      return g;
    }
  }
  if (const GlyphData *g = findGlyph(f, cp)) return g;
  cp = '?';
  return findGlyph(f, '?');
}

}  // namespace

void TextItem::change(void (TextItem::*apply)()) {
  invalidateArea();
  (this->*apply)();
  invalidateArea();
}

void TextItem::setFont(const FontData &font) {
  if (font_ == &font) return;
  font_ = &font;
  change(&TextItem::relayout);
}

void TextItem::setSize(float px) {
  if (size_ == px) return;
  size_ = px;
  change(&TextItem::relayout);
}

void TextItem::setText(const char *utf8) {
  if (strncmp(text_, utf8, sizeof(text_)) == 0) return;
  strncpy(text_, utf8, sizeof(text_) - 1);
  text_[sizeof(text_) - 1] = 0;
  layout(false);  // invalidates only the glyphs that change
}

void TextItem::setColor(uint32_t color, uint8_t alpha) {
  if (color_ == color && alpha_ == alpha) return;
  color_ = color, alpha_ = alpha;
  for (int i = 0; i < count_; i++) slots_[i]->item.setFill(color, alpha);
  invalidateArea();
}

void TextItem::invalidateGlyph(int i) {
  Rect r[kBands * 4];
  invalidateRects(r, slots_[i]->item.bandExtents(r, kBands * 4));
}

void TextItem::setAlign(Align a) {
  if (align_ == a) return;
  align_ = a;
  change(&TextItem::relayout);
}

void TextItem::setTabularNumbers(bool on) {
  if (tabular_ == on) return;
  tabular_ = on;
  change(&TextItem::relayout);
}

void TextItem::setGamma(float gamma) {
  if (gamma == gamma_) return;
  gamma_ = gamma;
  for (int i = 0; i < 256; i++) curve_[i] = uint8_t(powf(i / 255.0f, 1 / gamma) * 255 + 0.5f);
  for (int i = 0; i < count_; i++) slots_[i]->item.setCoverageCurve(gamma == 1 ? nullptr : curve_);
  invalidateArea();
}

void TextItem::setSnapping(bool on) {
  if (snap_ == on) return;
  snap_ = on;
  change(&TextItem::relayout);
}

// With snapping, the baseline lands on a pixel boundary.
Transform TextItem::glyphTransform() const {
  Transform t = transform_;
  if (snapping()) {
    float baseline = t.d * baselineY_ + t.f;
    t.f += roundf(baseline) - baseline;
  }
  return t;
}

void TextItem::setBaseline(Baseline b) {
  if (baseline_ == b) return;
  baseline_ = b;
  change(&TextItem::relayout);
}

void TextItem::setTransform(const Transform &t) {
  if (t == transform_) return;
  bool wasSnapping = snapping();
  transform_ = t;
  // The snapped size depends on the vertical scale; relayout if it changed.
  if (snapping() != wasSnapping || (snapping() && fabsf(t.d) != snapForD_)) change(&TextItem::relayout);
  else change(&TextItem::transform);
}

// Lays out the text left to right with the anchor at (0, 0): one path per
// visible glyph, in pixels, y down.
void TextItem::layout(bool force) {
  if (!font_) {
    for (int i = 0; i < count_; i++) invalidateGlyph(i);
    count_ = 0, width_ = 0;
    return transform();
  }
  const FontData &f = *font_;

  float width = 0;
  for (const char *p = text_; *p;) {
    uint32_t cp = nextCodepoint(p);
    if (const GlyphData *g = lookup(f, cp, tabular_)) width += g->advance;
  }
  width_ = width;
  // Snapping: pick the size whose x-height is a whole number of pixels.
  float s = scale();
  if (snapping()) {
    float d = fabsf(transform_.d);
    float xh = f.xHeight * s * d;
    if (xh >= 1) s *= roundf(xh) / xh;
    snapForD_ = d;
  }
  effectiveScale_ = s;
  float pen = align_ == Left ? 0 : align_ == Center ? -width / 2 : -width;
  const float yShift = baseline_ == Middle ? f.capHeight / 2.0f : 0;  // font units, y up
  baselineY_ = yShift * s;  // local y of the baseline
  const Transform gt = glyphTransform();

  int n = 0;
  for (const char *p = text_; *p && n < kMaxGlyphs;) {
    uint32_t cp = nextCodepoint(p);
    const GlyphData *g = lookup(f, cp, tabular_);
    if (!g) continue;
    if (g->count) {
      // Same character at the same place: keep it (nothing to redraw).
      if (force || n >= count_ || slots_[n]->codepoint != cp || slots_[n]->pen != pen) {
        if (!force && n < count_) invalidateGlyph(n);
        Slot &sl = slot(n);
        Path &path = sl.path;
        path.clear();
        const int16_t *d = f.data + g->offset, *end = d + g->count;
        auto X = [&](int16_t v) { return (pen + v) * s; };
        auto Y = [&](int16_t v) { return (yShift - v) * s; };
        while (d < end) {
          switch (*d++) {
            case 0: path.moveTo(X(d[0]), Y(d[1])), d += 2; break;
            case 1: path.lineTo(X(d[0]), Y(d[1])), d += 2; break;
            case 2: path.quadTo(X(d[0]), Y(d[1]), X(d[2]), Y(d[3])), d += 4; break;
            case 3: path.cubicTo(X(d[0]), Y(d[1]), X(d[2]), Y(d[3]), X(d[4]), Y(d[5])), d += 6; break;
            default: path.close(); break;
          }
        }
        PathItem &item = sl.item;
        item.setFill(color_, alpha_);
        item.setCoverageCurve(gamma_ == 1 ? nullptr : curve_);
        item.setTransform(gt);
        item.setPath(path);
        sl.codepoint = cp, sl.pen = pen;
        if (!force) invalidateGlyph(n);
      }
      n++;
    }
    pen += g->advance;
  }
  if (!force) {
    for (int i = n; i < count_; i++) invalidateGlyph(i);  // glyphs that went away
  }
  count_ = n;
  bounds_ = Rect();
  for (int i = 0; i < count_; i++) bounds_ = bounds_.unite(slots_[i]->item.bounds());
}

void TextItem::transform() {
  bounds_ = Rect();
  const Transform gt = glyphTransform();
  for (int i = 0; i < count_; i++) {
    slots_[i]->item.setTransform(gt);
    bounds_ = bounds_.unite(slots_[i]->item.bounds());
  }
}

// Glyph extents merged per band, so a line of text costs one interval per band.
int TextItem::bandExtents(Rect *out, int capacity) const {
  Rect merged[kBands];
  for (int i = 0; i < count_; i++) {
    Rect r[kBands * 4];
    int n = slots_[i]->item.bandExtents(r, kBands * 4);
    for (int k = 0; k < n; k++) {
      int band = r[k].y0 / kBandRows;
      if (band >= 0 && band < kBands) merged[band] = merged[band].unite(r[k]);
    }
  }
  int n = 0;
  for (int b = 0; b < kBands && n < capacity; b++) {
    if (!merged[b].empty()) out[n++] = merged[b];
  }
  return n;
}

void TextItem::render(const Span &s) const {
  Rect sr = s.rect();
  for (int i = 0; i < count_; i++) {
    if (slots_[i]->item.bounds().intersects(sr)) slots_[i]->item.render(s);
  }
}

}  // namespace swd
