// Animated linear gradient background for swd::StripRenderer.
//
// The gradient's look at a moment in time is a State value (palette + angle),
// so a renderer can keep several: e.g. one per screen half when halves are
// redrawn on alternating refreshes, so patching a small area reproduces
// exactly what is already on screen.
#pragma once

#include <stdint.h>

namespace swd {

struct GradientTheme {
  uint32_t stops[3];  // 0xRRGGBB, from one end of the gradient to the other
};

// Some ready-made themes.
extern const GradientTheme kGradientThemes[];
extern const int kGradientThemeCount;

class Gradient {
 public:
  struct State {
    // 2x2 ordered dithering hides RGB565 banding: four palettes, each rounded
    // with a different threshold, chosen by pixel position. Byte-swapped.
    uint16_t palette[4][256];
    int32_t dx, dy, origin;  // 16.16 fixed point palette index
  };

  // Computes the gradient at timeSeconds: it rotates once every ~25 s and its
  // middle color stop drifts. Takes ~0.5 ms; call on the main core between
  // draws, never while a draw might be reading the state.
  static void update(State &s, const GradientTheme &theme, float timeSeconds);

  // Renders a region (x0 and w even, as StripRenderer guarantees). stride is
  // the distance between rows in dst, in pixels (defaults to w), so a region
  // can be split into side-by-side parts that use different states.
  static void render(const State &s, uint16_t *dst, int x0, int y0, int w, int rows, int stride = 0);
};

}  // namespace swd
