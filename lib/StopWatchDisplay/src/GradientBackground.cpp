#include "GradientBackground.h"

#include <Arduino.h>

#include "StopWatchDisplay.h"

namespace swd {

const GradientTheme kGradientThemes[] = {
    {{0xFF5F6D, 0xFFC371, 0x845EC2}},  // sunset
    {{0x00C6FF, 0x0072FF, 0x7B2FF7}},  // ocean
    {{0x00F5A0, 0x00D9F5, 0x7A00FF}},  // aurora
    {{0x1A1A40, 0x7A0BC0, 0xFA58B6}},  // dusk
};
const int kGradientThemeCount = sizeof(kGradientThemes) / sizeof(kGradientThemes[0]);

namespace {

struct Rgb {
  float r, g, b;
};

Rgb toRgb(uint32_t c) { return {float(c >> 16 & 0xFF), float(c >> 8 & 0xFF), float(c & 0xFF)}; }

Rgb lerp(const Rgb &a, const Rgb &b, float t) {
  t = t * t * (3 - 2 * t);  // smoothstep for softer transitions
  return {a.r + (b.r - a.r) * t, a.g + (b.g - a.g) * t, a.b + (b.b - a.b) * t};
}

}  // namespace

void Gradient::update(State &s, const GradientTheme &theme, float time) {
  Rgb stops[3] = {toRgb(theme.stops[0]), toRgb(theme.stops[1]), toRgb(theme.stops[2])};
  // Let the middle stop drift so the colors flow, not just rotate.
  float mid = 0.5f + 0.18f * sinf(time * 0.7f);
  static const float kBayer[4] = {0.125f, 0.625f, 0.875f, 0.375f};
  for (int i = 0; i < 256; i++) {
    float p = i / 255.0f;
    Rgb c = p < mid ? lerp(stops[0], stops[1], p / mid) : lerp(stops[1], stops[2], (p - mid) / (1 - mid));
    for (int d = 0; d < 4; d++) {
      // Scale to 5/6/5 bits, add the dither threshold, then truncate.
      int r = min(31, int(c.r * 31 / 255 + kBayer[d]));
      int g = min(63, int(c.g * 63 / 255 + kBayer[d]));
      int b = min(31, int(c.b * 31 / 255 + kBayer[d]));
      s.palette[d][i] = swap16(r << 11 | g << 5 | b);
    }
  }

  // Linear gradient through the screen center, slowly rotating. The palette
  // index runs 0..255 across the visible circle's diameter.
  float a = time * 0.25f;
  float scale = 255.0f / 470;
  s.dx = int32_t(cosf(a) * scale * 65536);
  s.dy = int32_t(sinf(a) * scale * 65536);
  s.origin = int32_t(127.5f * 65536) - s.dx * kCenterX - s.dy * kCenterY;
}

// The render core is the bottleneck at the start of each frame, so this loop
// is kept lean: rows whose palette index stays in range skip clamping, and
// pixel pairs are written as one 32-bit store (x0 and w are even, so every
// row starts 4-byte aligned). This took the full frame from ~15.5 to ~8.5 ms.
void Gradient::render(const State &s, uint16_t *dst, int x0, int y0, int w, int rows, int stride) {
  if (stride <= 0) stride = w;
  const int32_t dx = s.dx;
  for (int y = y0; y < y0 + rows; y++) {
    auto *p = reinterpret_cast<uint32_t *>(dst + (y - y0) * stride);
    const uint16_t *even = s.palette[(y & 1) * 2], *odd = s.palette[(y & 1) * 2 + 1];
    int32_t v = s.origin + s.dy * y + dx * x0;
    int32_t vEnd = v + dx * (w - 1);
    if (min(v, vEnd) >= 0 && max(v, vEnd) < (256 << 16)) {
      for (int x = 0; x < w; x += 2) {
        uint32_t a = even[v >> 16];
        v += dx;
        uint32_t b = odd[v >> 16];
        v += dx;
        *p++ = a | b << 16;
      }
    } else {
      for (int x = 0; x < w; x += 2) {
        uint32_t a = even[constrain(v >> 16, 0, 255)];
        v += dx;
        uint32_t b = odd[constrain(v >> 16, 0, 255)];
        v += dx;
        *p++ = a | b << 16;
      }
    }
  }
}

}  // namespace swd
