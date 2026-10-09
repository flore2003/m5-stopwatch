#include "StopWatchDisplay.h"

#include <esp_heap_caps.h>
#include <lgfx/v1/panel/Panel_AMOLED.hpp>

namespace swd {

// ---- Direct panel output
//
// On the StopWatch, M5GFX draws into a PSRAM framebuffer and, on endWrite(),
// copies the bounding box of everything changed to the panel line by line.
// Pushing our strips through it costs two copies (SRAM -> PSRAM -> panel,
// ~12 + ~12 ms per full frame). We send the strips to the real panel driver
// instead: one DMA transfer per strip, straight from SRAM. Don't draw with
// M5.Display while a StripRenderer is in use: its framebuffer no longer
// matches the screen, and its next flush would overwrite what we sent.
namespace {

struct FramebufferAccess : lgfx::Panel_AMOLED_Framebuffer {
  // _panel is protected; this is the usual way to reach it from outside.
  static lgfx::Panel_AMOLED *panelOf(lgfx::IPanel *fb) {
    return static_cast<FramebufferAccess *>(static_cast<lgfx::Panel_AMOLED_Framebuffer *>(fb))->_panel;
  }
};

lgfx::Panel_AMOLED *directPanel() {
  static lgfx::Panel_AMOLED *panel = nullptr;
  if (!panel && M5.getBoard() == m5::board_t::board_M5StopWatch) {
    panel = FramebufferAccess::panelOf(M5.Display.getPanel());
  }
  return panel;
}

}  // namespace

void setupDisplay(uint8_t brightness) {
  auto *panel = M5.Display.getPanel();
  auto pc = panel->config();
  pc.panel_width = kWidth;
  pc.panel_height = kHeight;
  pc.offset_x = 6;
  pc.offset_y = 0;
  panel->config(pc);
  M5.Display.setRotation(0);
  M5.Display.setBrightness(brightness);
}

// ---- Vsync

static volatile uint32_t teCount = 0;
static volatile uint32_t teEdgeUs = 0;
static volatile uint32_t tePeriod16 = 0;  // average period, 1/16 us
static SemaphoreHandle_t teSem = nullptr;  // given on every edge, for blocking waits
static void IRAM_ATTR onTE() {
  uint32_t t = micros(), d = t - teEdgeUs;
  // Average plausible periods only (a missed edge would double it).
  if (teCount > 2 && d > kRefreshUs * 3 / 4 && d < kRefreshUs * 5 / 4) {
    uint32_t p = tePeriod16 ? tePeriod16 : kRefreshUs * 16;
    tePeriod16 = p + (int32_t(d * 16 - p) >> 5);
  }
  teEdgeUs = t;
  teCount = teCount + 1;
  BaseType_t woken = pdFALSE;
  if (teSem) xSemaphoreGiveFromISR(teSem, &woken);
  if (woken) portYIELD_FROM_ISR();
}

void Vsync::begin(int pin) {
  if (!teSem) teSem = xSemaphoreCreateBinary();
  pinMode(pin, INPUT);
  attachInterrupt(digitalPinToInterrupt(pin), onTE, RISING);
}

uint32_t Vsync::count() { return teCount; }

uint32_t Vsync::lastEdgeUs() { return teEdgeUs; }

uint32_t Vsync::periodUs() { return tePeriod16 ? (tePeriod16 + 8) / 16 : kRefreshUs; }

void Vsync::waitAfterEdge(uint32_t us) {
  uint32_t edge = teEdgeUs;
  while (micros() - edge < us) {
  }
}

// Blocks (instead of spinning) until the target edge: the TE interrupt gives
// a semaphore, so the waiting core is free in the meantime. Waking takes a
// few tens of microseconds, well inside the ~3 ms blanking after TE.
bool Vsync::waitFor(uint32_t target, uint32_t timeoutUs) {
  uint32_t t0 = micros();
  while (int32_t(teCount - target) < 0) {
    uint32_t elapsed = micros() - t0;
    if (elapsed > timeoutUs) return false;
    if (teSem) {
      xSemaphoreTake(teSem, pdMS_TO_TICKS((timeoutUs - elapsed) / 1000) + 1);
    }
  }
  return true;
}

// ---- FramePacer

void FramePacer::reset() { started_ = false; }

void FramePacer::wait() {
  uint32_t now = Vsync::count();
  if (!started_) {
    started_ = true;
    next_ = now + 1;
  } else if (int32_t(now - next_) > 0 || (now == next_ && micros() - Vsync::lastEdgeUs() > kScanStartUs)) {
    // Missed the target refresh. Starting during the blanking after its TE
    // edge is still fine for draws that chase the scan; later costs a skip.
    skipped_++;
    next_ = now + 1;
  }
  Vsync::waitFor(next_);
  next_ += interval_;
}

// ---- StripRenderer

bool StripRenderer::begin(RenderFn fn, void *ctx) { return begin(fn, ctx, Config()); }

bool StripRenderer::begin(RenderFn fn, void *ctx, const Config &cfg) {
  cfg_ = cfg;
  // One strip must fit a single SPI transaction (32 KiB); longer writes make
  // the bus driver sleep a tick between chunks.
  cfg_.stripRows = constrain(cfg_.stripRows & ~1, 2, 32768 / (kWidth * 2) & ~1);
  fn_ = fn;
  ctx_ = ctx;
  if (!directPanel()) return false;
  for (auto &b : buf_) {
    b = static_cast<uint16_t *>(
        heap_caps_malloc(kWidth * cfg_.stripRows * 2, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL));
    if (!b) return false;
  }
  // Render on the core this one isn't using.
  return xTaskCreatePinnedToCore(taskEntry, "strips", 4096, this, cfg_.taskPriority, &task_,
                                 1 - xPortGetCoreID()) == pdPASS;
}

void StripRenderer::taskEntry(void *self) {
  auto *r = static_cast<StripRenderer *>(self);
  for (;;) {
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
    Job job = {r->job_.dst, r->job_.x, r->job_.y, r->job_.w, r->job_.rows};
    uint32_t t0 = micros();
    r->fn_(job.dst, job.x, job.y, job.w, job.rows, r->ctx_);
    r->renderUs_ = r->renderUs_ + (micros() - t0);
    xTaskNotifyGive(r->caller_);
  }
}

void StripRenderer::runJob(const Job &job) {
  job_.dst = job.dst;
  job_.x = job.x;
  job_.y = job.y;
  job_.w = job.w;
  job_.rows = job.rows;
  xTaskNotifyGive(task_);
}

// Columns to send for a band of rows: the requested range, clipped to the
// visible circle's widest row in the band if circleOnly. The panel needs an
// even start column and width.
bool StripRenderer::span(int x, int y, int w, int rows, int &sx, int &sw) const {
  int x0 = x, x1 = x + w;
  if (cfg_.circleOnly) {
    int y1 = y + rows - 1;
    int dy = (y <= kCenterY && kCenterY <= y1) ? 0 : min(abs(y - kCenterY), abs(y1 - kCenterY));
    if (dy >= kRadius) return false;
    int half = int(ceilf(sqrtf(float(kRadius * kRadius - dy * dy)))) + 2;  // +2 px margin
    x0 = max(x0, kCenterX - half);
    x1 = min(x1, kCenterX + half);
  }
  x0 = max(0, x0) & ~1;
  x1 = min(kWidth, (x1 + 1) & ~1);
  if (x1 <= x0) return false;
  sx = x0;
  sw = x1 - x0;
  return true;
}

uint32_t StripRenderer::drawRect(int x, int y, int w, int h) {
  Region r = {x, y, w, h};
  return drawRegions(&r, 1);
}

uint32_t StripRenderer::drawRegions(const Region *regions, int count) {
  uint32_t t0 = micros();
  stats_ = {};
  renderUs_ = 0;
  if (!fn_) return 0;
  caller_ = xTaskGetCurrentTaskHandle();

  // Cut every region into bands on a shared grid of stripRows, then order the
  // pieces by band, so the whole update is written top to bottom.
  int yMin = kHeight, yMax = 0;
  for (int i = 0; i < count; i++) {
    yMin = min(yMin, max(0, regions[i].y));
    yMax = max(yMax, min(kHeight, regions[i].y + regions[i].h));
  }
  constexpr int kMaxJobs = 128;
  Job jobs[kMaxJobs];
  int n = 0;
  for (int by = yMin - yMin % cfg_.stripRows; by < yMax; by += cfg_.stripRows) {
    for (int i = 0; i < count && n < kMaxJobs; i++) {
      const Region &r = regions[i];
      // The CO5300 wants row windows that start on an even row and cover an
      // even number of rows (M5GFX's own flush rounds the same way).
      int y0 = max(by, max(0, r.y) & ~1);
      int y1 = min(by + cfg_.stripRows, min(kHeight, (r.y + r.h + 1) & ~1)), sx, sw;
      if (y0 >= y1 || r.w <= 0) continue;
      if (span(r.x, y0, r.w, y1 - y0, sx, sw)) jobs[n++] = {nullptr, int16_t(sx), int16_t(y0), int16_t(sw), int16_t(y1 - y0)};
    }
  }
  if (n == 0) return stats_.total = micros() - t0;
  const uint32_t edge = Vsync::lastEdgeUs();

  auto *panel = directPanel();
  auto *bus = panel->getBus();
  Piece pieces[kMaxJobs];
  panel->beginTransaction();
  jobs[0].dst = buf_[0];
  runJob(jobs[0]);
  uint32_t w0 = micros();
  ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
  stats_.waitRender += micros() - w0;
  for (int i = 0; i < n; i++) {
    const Job &b = jobs[i];
    if (cfg_.followScan) {
      uint32_t passed = scanTimeUs(b.y + b.rows) + cfg_.scanMarginUs, s0 = micros();
      while (micros() - edge < passed) {
      }
      stats_.waitScan += micros() - s0;
    }
    // setWindow waits for the previous strip's DMA to finish, so the buffer
    // the render core fills next (the one before this) is free again.
    panel->setWindow(b.x, b.y, b.x + b.w - 1, b.y + b.rows - 1);
    panel->start_qspi();
    pieces[i] = {b.y, b.rows, micros(), 0};
    bus->writeBytes(reinterpret_cast<const uint8_t *>(b.dst), b.w * b.rows * 2, true, true);
    bool rendering = i + 1 < n;
    if (rendering) {
      jobs[i + 1].dst = buf_[(i + 1) & 1];
      runJob(jobs[i + 1]);  // renders while this strip's DMA runs
    }
    // Wait for both the render core and this strip's DMA, noting when the
    // DMA finished (for the tear check).
    w0 = micros();
    uint32_t renderDone = w0;
    for (;;) {
      if (!pieces[i].end && !bus->busy()) pieces[i].end = micros();
      if (rendering && ulTaskNotifyTake(pdTRUE, 0)) {
        rendering = false;
        renderDone = micros();
      }
      if (!rendering && pieces[i].end) break;
    }
    if (i + 1 < n) stats_.waitRender += renderDone - w0;
    else stats_.flush = pieces[i].end - w0;
  }
  panel->endTransaction();
  checkTearing(pieces, n, edge, Vsync::periodUs(), stats_);
  draws_++;
  if (stats_.torn) torn_++;
  stats_.render = renderUs_;
  stats_.bands = n;
  return stats_.total = micros() - t0;
}

void StripRenderer::checkTearing(const Piece *pieces, int n, uint32_t edgeUs, uint32_t periodUs, Stats &out) {
  out.torn = false;
  out.slackUs = INT32_MAX;
  out.refresh = -1;
  bool first = true;
  for (int i = 0; i < n; i++) {
    const Piece &p = pieces[i];
    int32_t s0 = int32_t(p.start - edgeUs), s1 = int32_t(p.end - edgeUs);
    int shown = INT8_MIN;
    // Compare the write (start..end, top to bottom) with the scan of this
    // piece's rows in the refreshes around it. The write and a scan can only
    // cross if one is ahead at the piece's top row and behind at its bottom.
    for (int k = -1; k <= 3; k++) {
      int32_t a = k * int32_t(periodUs) + int32_t(scanTimeUs(p.y));
      int32_t b = k * int32_t(periodUs) + int32_t(scanTimeUs(p.y + p.rows));
      int32_t d0 = s0 - a, d1 = s1 - b;
      if ((d0 > 0) != (d1 > 0)) {
        out.torn = true;
        out.slackUs = min(out.slackUs, int32_t(0));
      } else {
        out.slackUs = min(out.slackUs, min(abs(d0), abs(d1)));
      }
      // First refresh whose scan reaches the piece only after it's written.
      if (shown == INT8_MIN && d0 <= 0 && d1 <= 0) shown = k;
    }
    if (first) {
      out.refresh = shown;
      first = false;
    } else if (shown != out.refresh) {
      out.torn = true;  // parts of one draw appear in different refreshes
    }
  }
}

// ---- Overlay

bool Overlay::begin(int w, int h) {
  canvas_.setPsram(false);
  canvas_.setColorDepth(16);
  if (!canvas_.createSprite(w, h)) return false;
  clear();
  return true;
}

void Overlay::composite(uint16_t *dst, int x, int y, int w, int rows) const {
  if (!visible_) return;
  int ox0 = max(x, x_), ox1 = min(x + w, x_ + width());
  int oy0 = max(y, y_), oy1 = min(y + rows, y_ + height());
  if (ox0 >= ox1 || oy0 >= oy1) return;
  constexpr uint16_t key = swap16(kTransparent);  // as stored in the sprite
  auto *src = static_cast<const uint16_t *>(const_cast<M5Canvas &>(canvas_).getBuffer());
  for (int sy = oy0; sy < oy1; sy++) {
    const uint16_t *s = src + (sy - y_) * width() + (ox0 - x_);
    uint16_t *d = dst + (sy - y) * w + (ox0 - x);
    for (int sx = ox0; sx < ox1; sx++, s++, d++) {
      if (*s != key) *d = *s;
    }
  }
}

}  // namespace swd
