// Live vector text for swd::Engine.
//
//   swd::TextItem clock;
//   clock.setFont(swd::fonts::InterSemiBold);
//   clock.setSize(64);
//   clock.setAlign(swd::TextItem::Center);
//   clock.setPosition(233, 260);           // baseline anchor
//   clock.setText("12:34");                // any time; only the change redraws
//   layer.add(clock);                      // a VectorLayer
//
// Glyphs are outlines (converted from a TTF by tools/font2cpp.py), filled by
// the same exact-coverage path filler as other shapes, so text can have any
// size, color or transform (including rotation) and change every frame.
// Each glyph is its own path: rendering skips glyphs outside the strip, and
// setText() only rebuilds and redraws glyphs whose character or position
// changed (a counter going 00123 -> 00124 redraws one digit). No hinting and
// no kerning yet.
//
// Codepoints U+E000 + c hold the font's tabular (equal-width) form of ASCII
// character c, where it has one (see setTabularNumbers()).
#pragma once

#include "Path.h"

namespace swd {

struct GlyphData {
  uint16_t codepoint;
  uint16_t advance;  // font units
  uint32_t offset;   // into FontData::data
  uint16_t count;    // int16 values
};

// Outline font data: per glyph a stream of ops (0 move x y, 1 line x y,
// 2 quad cx cy x y, 3 cubic c1x c1y c2x c2y x y, 4 close) in font units, y up.
struct FontData {
  const char *name;
  uint16_t unitsPerEm;
  int16_t ascender, descender, capHeight, xHeight;
  uint16_t glyphCount;
  const GlyphData *glyphs;  // sorted by codepoint
  const int16_t *data;
};

namespace fonts {
extern const FontData InterRegular;
extern const FontData InterSemiBold;
}  // namespace fonts

class TextItem : public VectorItem {
 public:
  enum Align : uint8_t { Left, Center, Right };
  enum Baseline : uint8_t { Alphabetic, Middle };  // Middle: centered on the cap height

  static constexpr int kMaxGlyphs = 48;

  TextItem() = default;
  TextItem(const FontData &font, float size) : font_(&font), size_(size) {}

  void setFont(const FontData &font);
  void setSize(float px);  // em size in pixels
  void setText(const char *utf8);
  void setColor(uint32_t color, uint8_t alpha = 255);
  void setAlign(Align a);
  void setBaseline(Baseline b);
  // Equal-width digits (the font's tabular figures, if it has them), so
  // changing numbers don't shift: use for counters and clocks.
  void setTabularNumbers(bool on);
  // Where the anchor (aligned point on the baseline) goes; replaces the
  // transform with a translation.
  void setPosition(float x, float y) { setTransform(Transform::translate(x, y)); }
  // Applied to the laid-out text, whose anchor is at (0, 0).
  void setTransform(const Transform &t);

  const char *text() const { return text_; }
  float width() const { return width_ * scale(); }  // advance width in pixels

  Rect bounds() const override { return bounds_; }
  int bandExtents(Rect *out, int capacity) const override;
  void render(const Span &s) const override;

 private:
  float scale() const { return font_ ? size_ / font_->unitsPerEm : 0; }
  // Text, font, size, alignment -> glyph paths. With force, every glyph is
  // rebuilt; otherwise only glyphs whose character or position changed,
  // and only their areas are redrawn.
  void layout(bool force);
  void relayout() { layout(true); }
  void transform();  // glyph paths + transform -> screen edges
  void change(void (TextItem::*apply)());
  void invalidateGlyph(int i);

  const FontData *font_ = nullptr;
  float size_ = 24;
  char text_[kMaxGlyphs * 4 + 1] = "";
  uint32_t color_ = 0xFFFFFF;
  uint8_t alpha_ = 255;
  Align align_ = Left;
  Baseline baseline_ = Alphabetic;
  bool tabular_ = false;
  Transform transform_;
  float width_ = 0;  // font units

  Path paths_[kMaxGlyphs];
  PathItem items_[kMaxGlyphs];
  uint32_t codepoints_[kMaxGlyphs] = {};
  float pens_[kMaxGlyphs] = {};  // glyph origin x in font units
  int count_ = 0;
  Rect bounds_;
};

}  // namespace swd
