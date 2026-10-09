// Animated full-screen gradient, tear-free at 30 fps, with an fps indicator.
//
// Rendering, pacing and the panel quirks live in lib/StopWatchDisplay; this
// sketch only supplies the gradient and the label.
//
// Tap the screen to switch color theme; KEY A toggles circle-only spans.
// Serial prints frame timing.
#include <M5Unified.h>
#include <StopWatchDisplay.h>

// ---- Gradient

struct Rgb { float r, g, b; };
struct Theme { Rgb stops[3]; };
static constexpr Rgb hex(uint32_t c) { return {float(c >> 16 & 0xFF), float(c >> 8 & 0xFF), float(c & 0xFF)}; }
static const Theme kThemes[] = {
  {{hex(0xFF5F6D), hex(0xFFC371), hex(0x845EC2)}},  // sunset
  {{hex(0x00C6FF), hex(0x0072FF), hex(0x7B2FF7)}},  // ocean
  {{hex(0x00F5A0), hex(0x00D9F5), hex(0x7A00FF)}},  // aurora
  {{hex(0x1A1A40), hex(0x7A0BC0), hex(0xFA58B6)}},  // dusk
};
static volatile int theme = 0;

// 2x2 ordered dithering hides RGB565 banding in slow gradients: four palettes,
// each rounded with a different threshold, chosen by pixel position.
static uint16_t palette[4][256];  // byte-swapped RGB565

// Per-frame gradient parameters, 16.16 fixed point palette index.
static int32_t gradDx, gradDy, gradOrigin;

static Rgb lerp(const Rgb &a, const Rgb &b, float t) {
  t = t * t * (3 - 2 * t);  // smoothstep for softer transitions
  return {a.r + (b.r - a.r) * t, a.g + (b.g - a.g) * t, a.b + (b.b - a.b) * t};
}

static void buildPalette(float time) {
  const Theme &th = kThemes[theme];
  // Let the middle stop drift so the colors flow, not just rotate.
  float mid = 0.5f + 0.18f * sinf(time * 0.7f);
  static const float kBayer[4] = {0.125f, 0.625f, 0.875f, 0.375f};
  for (int i = 0; i < 256; i++) {
    float p = i / 255.0f;
    Rgb c = p < mid ? lerp(th.stops[0], th.stops[1], p / mid)
                    : lerp(th.stops[1], th.stops[2], (p - mid) / (1 - mid));
    for (int d = 0; d < 4; d++) {
      // Scale to 5/6/5 bits, add the dither threshold, then truncate.
      int r = min(31, int(c.r * 31 / 255 + kBayer[d]));
      int g = min(63, int(c.g * 63 / 255 + kBayer[d]));
      int b = min(31, int(c.b * 31 / 255 + kBayer[d]));
      palette[d][i] = swd::swap16(r << 11 | g << 5 | b);
    }
  }
}

static void setupGradient(float time) {
  // Linear gradient through the screen center, slowly rotating (one turn per
  // ~25 s). The palette index runs 0..255 across the screen diagonal.
  float a = time * 0.25f;
  float scale = 255.0f / 470;  // ~diagonal of the visible circle
  gradDx = int32_t(cosf(a) * scale * 65536);
  gradDy = int32_t(sinf(a) * scale * 65536);
  gradOrigin = int32_t(127.5f * 65536) - gradDx * swd::kCenterX - gradDy * swd::kCenterY;
}

// ---- FPS label

static swd::Overlay label;

static void updateLabel(float fps) {
  char text[16];
  snprintf(text, sizeof(text), "%.0f fps", fps);
  auto &c = label.canvas();
  label.clear();
  c.fillRoundRect(0, 0, c.width(), c.height(), c.height() / 2, TFT_BLACK);
  c.setTextColor(TFT_WHITE);
  c.setTextDatum(middle_center);
  c.setFont(&fonts::FreeSansBold9pt7b);
  c.drawString(text, c.width() / 2, c.height() / 2 + 1);
}

// ---- Render callback (runs on the render core)

// The render core is the bottleneck at the start of each frame, so this loop
// is kept lean: rows whose palette index stays in range skip clamping, and
// pixel pairs are written as one 32-bit store (x0 and w are always even, so
// every row starts 4-byte aligned).
static void render(uint16_t *dst, int x0, int y0, int w, int rows, void *) {
  auto *p = reinterpret_cast<uint32_t *>(dst);
  for (int y = y0; y < y0 + rows; y++) {
    const uint16_t *even = palette[(y & 1) * 2], *odd = palette[(y & 1) * 2 + 1];
    int32_t v = gradOrigin + gradDy * y + gradDx * x0;
    int32_t vEnd = v + gradDx * (w - 1);
    const int32_t dx = gradDx;
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
  label.composite(dst, x0, y0, w, rows);
}

static swd::StripRenderer renderer;
static swd::FramePacer pacer(2);  // every second refresh: 30 fps
static volatile bool toggleCircle = false;

// Input runs on the render core at low priority, so touch I2C reads never
// delay the start of a frame.
static void inputTask(void *) {
  for (;;) {
    M5.update();
    if (M5.Touch.getDetail().wasPressed()) {
      theme = (theme + 1) % (sizeof(kThemes) / sizeof(kThemes[0]));
      Serial.printf("theme %d\n", theme);
    }
    if (M5.BtnA.wasPressed()) toggleCircle = true;
    vTaskDelay(pdMS_TO_TICKS(10));
  }
}

void setup() {
  Serial.begin(115200);
  Serial.setTxTimeoutMs(0);  // never block on a full USB buffer
  auto cfg = M5.config();
  cfg.fallback_board = m5::board_t::board_M5StopWatch;
  M5.begin(cfg);
  swd::setupDisplay();
  swd::Vsync::begin();

  if (!renderer.begin(render)) Serial.println("error: renderer init failed");
  label.begin(84, 26);
  label.setPosition((swd::kWidth - label.width()) / 2, 34);
  updateLabel(0);
  xTaskCreatePinnedToCore(inputTask, "input", 4096, nullptr, 1, nullptr, 1 - xPortGetCoreID());
  Serial.println("ready");
}

void loop() {
  static uint32_t windowStart = millis(), windowFrames = 0, worstUs = 0;

  if (toggleCircle) {
    toggleCircle = false;
    renderer.setCircleOnly(!renderer.circleOnly());
  }
  float time = millis() / 1000.0f;
  buildPalette(time);
  setupGradient(time);

  pacer.wait();
  uint32_t us = renderer.drawFrame();
  if (us > worstUs) worstUs = us;
  windowFrames++;

  uint32_t elapsed = millis() - windowStart;
  if (elapsed >= 1000) {
    float fps = windowFrames * 1000.0f / elapsed;
    updateLabel(fps);  // safe: the renderer is idle between frames
    auto &st = renderer.lastStats();
    Serial.printf("fps=%.1f frame_worst_us=%u skipped=%u circle_only=%d | last: render=%u wait_render=%u flush=%u bands=%u\n",
                  fps, worstUs, pacer.skipped(), renderer.circleOnly(), st.render, st.waitRender, st.flush, st.bands);
    windowStart = millis();
    windowFrames = 0;
    worstUs = 0;
  }
}
