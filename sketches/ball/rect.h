// Kept in a header: Arduino's generated prototypes would otherwise appear
// before the struct definition in ball.ino.
#pragma once

#include <algorithm>

struct Rect {
  int x0, y0, x1, y1;  // half-open: [x0, x1) x [y0, y1)
};

inline Rect unite(const Rect &a, const Rect &b) {
  return {std::min(a.x0, b.x0), std::min(a.y0, b.y0), std::max(a.x1, b.x1), std::max(a.y1, b.y1)};
}
