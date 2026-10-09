// Tear-free rendering for the M5Stack StopWatch (C152) round AMOLED.
//
// What we measured on this panel (see the project README, sketches/scan_probe
// and sketches/ball):
// - The CO5300 refreshes at 59.68 Hz (16.76 ms) and reports each refresh on
//   its tearing-effect (TE) pin, GPIO38. M5GFX enables TE but never reads it.
// - After the TE edge the panel blanks for ~2.95 ms, then scans rows top to
//   bottom at ~29.7 us per row (13.8 ms for 466 rows). So row r is scanned
//   at TE + kScanStartUs + r * kScanRowUs.
// - A full 468x466 RGB565 frame takes ~22-24 ms on the QSPI bus, so full-screen
//   updates top out at 30 fps (every second refresh).
//
// Tear-free rule: write each row after the scan has passed it, and finish it
// before the scan comes round again (one refresh later). The scan then shows
// the old row this refresh and the new one the next, so everything written in
// one pass appears together, one refresh later. StripRenderer does this with
// followScan (on by default): each piece waits until the scan has passed it.
// A write that is slower than the scan (full-width rows, ~51 us/row) only
// waits for the first piece; ~30 ms per full frame is the limit (a PSRAM
// full-frame push, ~31 ms, tears).
//
// Pixel format everywhere: RGB565, byte-swapped (as the panel expects). Use
// swd::rgb565() to build colors.
#pragma once

#include <M5Unified.h>

namespace swd {

// Corrected panel geometry. Rows 466-467 of the controller are never shown.
constexpr int kWidth = 468;
constexpr int kHeight = 466;
constexpr int kCenterX = kWidth / 2;
constexpr int kCenterY = kHeight / 2;
constexpr int kRadius = 233;  // visible circle

// Panel scan timing relative to the TE edge (sketches/scan_probe measured the
// period and row time; the blanking was tuned by eye with sketches/ball).
constexpr uint32_t kRefreshUs = 16756;    // 59.68 Hz
constexpr uint32_t kScanStartUs = 2950;   // blanking before row 0
constexpr float kScanRowUs = 29.7f;       // top to bottom, 13.8 ms for 466 rows

// When the scan reaches the start of row r, in microseconds after TE.
constexpr uint32_t scanTimeUs(int row) { return kScanStartUs + uint32_t(row * kScanRowUs); }

constexpr uint16_t swap16(uint16_t v) { return uint16_t(v >> 8 | v << 8); }
constexpr uint16_t rgb565(uint8_t r, uint8_t g, uint8_t b) {
  return swap16(uint16_t((r & 0xF8) << 8 | (g & 0xFC) << 3 | b >> 3));
}

// Applies the panel geometry fix (468x466, offset_x 6, rotation 0). The
// default config leaves a black bar; this comes from ESPtember. Call right
// after M5.begin().
void setupDisplay(uint8_t brightness = 255);

// Counts panel refreshes via an interrupt on the TE pin.
class Vsync {
 public:
  static void begin(int pin = 38);
  static uint32_t count();
  // Busy-waits until count() reaches target (wrap-safe). False on timeout,
  // so a missing TE signal never hangs the caller.
  static bool waitFor(uint32_t target, uint32_t timeoutUs = 50000);
  static bool waitNext(uint32_t timeoutUs = 50000) { return waitFor(count() + 1, timeoutUs); }
  // micros() timestamp of the latest TE edge.
  static uint32_t lastEdgeUs();
  // Busy-waits until `us` microseconds after the latest TE edge.
  static void waitAfterEdge(uint32_t us);
};

// Starts frames on every Nth refresh. If a frame runs long, it resyncs to the
// next refresh instead of trying to catch up (and counts a skip).
class FramePacer {
 public:
  explicit FramePacer(uint8_t refreshesPerFrame = 2) : interval_(refreshesPerFrame) {}
  void setInterval(uint8_t refreshesPerFrame) { interval_ = refreshesPerFrame; }
  // Blocks until the next scheduled refresh edge.
  void wait();
  uint32_t skipped() const { return skipped_; }

 private:
  uint8_t interval_;
  uint32_t next_ = 0;
  uint32_t skipped_ = 0;
  bool started_ = false;
};

// Fills dst with pixels for screen columns [x, x + w) and rows [y, y + rows);
// dst has a stride of w pixels. x and w are always even. Runs on the render
// core, so anything it reads must not change while a draw is in progress.
using RenderFn = void (*)(uint16_t *dst, int x, int y, int w, int rows, void *ctx);

struct Region {
  int x, y, w, h;
};

// Renders the screen in horizontal strips: the second core renders strip
// i + 1 into one SRAM buffer while this core sends strip i from the other.
class StripRenderer {
 public:
  struct Config {
    int stripRows = 32;
    // Only send the part of each strip inside the visible circle (~84% of
    // the pixels for a full frame). Invisible corners keep old content.
    bool circleOnly = true;
    UBaseType_t taskPriority = configMAX_PRIORITIES - 2;
    // Chase the scan: before sending each piece, wait until the scan of the
    // refresh that started at the latest TE edge has passed the piece's last
    // row. Everything a draw sends then shows on the next refresh, whatever
    // its width or position. Start draws soon after a TE edge, and make sure
    // they end before the scan returns (about one refresh later).
    bool followScan = true;
    // Extra wait after the scan should have passed a piece (TE interrupt
    // latency, timing spread). With 100 us the ball test was tear-free.
    uint32_t scanMarginUs = 100;
  };

  bool begin(RenderFn fn, void *ctx = nullptr);
  bool begin(RenderFn fn, void *ctx, const Config &cfg);
  void setCircleOnly(bool on) { cfg_.circleOnly = on; }  // call between draws
  void setFollowScan(bool on) { cfg_.followScan = on; }
  void setScanMargin(uint32_t us) { cfg_.scanMarginUs = us; }
  uint32_t scanMargin() const { return cfg_.scanMarginUs; }
  bool circleOnly() const { return cfg_.circleOnly; }

  // Render and send a region. Each call blocks until the pixels are on the
  // panel and returns the time it took in microseconds. Don't wrap these in
  // your own startWrite()/endWrite(), or the transfer completes later.
  uint32_t drawFrame() { return drawRect(0, 0, kWidth, kHeight); }
  uint32_t drawRows(int y0, int y1) { return drawRect(0, y0, kWidth, y1 - y0); }
  uint32_t drawRect(int x, int y, int w, int h);
  // Several regions written together in row order (all pieces of one band of
  // stripRows, then the next band), so the whole update follows the scan.
  // Regions must not overlap.
  uint32_t drawRegions(const Region *regions, int count);

  // Timing of the last draw, in microseconds. If waitRender is a large part
  // of total, the render callback is the bottleneck, not the bus.
  struct Stats {
    uint32_t total;       // whole draw, until the pixels are on the panel
    uint32_t render;      // time spent in the render callback (render core)
    uint32_t waitRender;  // time this core waited for the render core
    uint32_t flush;       // time endWrite() waited for the last transfers
    uint32_t waitScan;    // time spent waiting for the scan (followScan)
    uint16_t bands;
  };
  const Stats &lastStats() const { return stats_; }

 private:
  struct Job {
    uint16_t *dst;
    int16_t x, y, w, rows;
  };
  static void taskEntry(void *self);
  void runJob(const Job &job);
  bool span(int x, int y, int w, int rows, int &sx, int &sw) const;

  Config cfg_;
  RenderFn fn_ = nullptr;
  void *ctx_ = nullptr;
  uint16_t *buf_[2] = {nullptr, nullptr};
  TaskHandle_t task_ = nullptr;
  TaskHandle_t caller_ = nullptr;
  volatile Job job_ = {};
  volatile uint32_t renderUs_ = 0;
  Stats stats_ = {};
};

// A small sprite composited over rendered pixels. Pixels equal to
// kTransparent are skipped, so shapes can be any outline.
class Overlay {
 public:
  // Key color in M5GFX's (unswapped) RGB565 form, for drawing into canvas().
  static constexpr uint16_t kTransparent = 0x0120;  // an unlikely dark green

  bool begin(int w, int h);
  // Draw with M5GFX into this. Avoid anti-aliased edges: they would blend
  // into the key color. Only change it between draws.
  M5Canvas &canvas() { return canvas_; }
  void clear() { canvas_.fillScreen(kTransparent); }
  void setPosition(int x, int y) { x_ = x; y_ = y; }
  void setVisible(bool visible) { visible_ = visible; }
  int x() const { return x_; }
  int y() const { return y_; }
  int width() const { return canvas_.width(); }
  int height() const { return canvas_.height(); }

  // Call from a RenderFn after drawing the background into dst.
  void composite(uint16_t *dst, int x, int y, int w, int rows) const;

 private:
  M5Canvas canvas_;
  int x_ = 0, y_ = 0;
  bool visible_ = true;
};

}  // namespace swd
