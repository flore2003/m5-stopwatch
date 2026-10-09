#include "StopWatchDisplay.h"

#include <esp_heap_caps.h>

namespace swd {

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
static void IRAM_ATTR onTE() { teCount = teCount + 1; }

void Vsync::begin(int pin) {
  pinMode(pin, INPUT);
  attachInterrupt(digitalPinToInterrupt(pin), onTE, RISING);
}

uint32_t Vsync::count() { return teCount; }

bool Vsync::waitFor(uint32_t target, uint32_t timeoutUs) {
  uint32_t t0 = micros();
  while (int32_t(teCount - target) < 0) {
    if (micros() - t0 > timeoutUs) return false;
  }
  return true;
}

// ---- FramePacer

void FramePacer::wait() {
  uint32_t now = Vsync::count();
  if (!started_ || int32_t(now - next_) >= 0) {
    if (started_) skipped_++;
    started_ = true;
    next_ = now + 1;
  }
  Vsync::waitFor(next_);
  next_ += interval_;
}

// ---- StripRenderer

bool StripRenderer::begin(RenderFn fn, void *ctx) { return begin(fn, ctx, Config()); }

bool StripRenderer::begin(RenderFn fn, void *ctx, const Config &cfg) {
  cfg_ = cfg;
  fn_ = fn;
  ctx_ = ctx;
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
  uint32_t t0 = micros();
  int yEnd = min(kHeight, y + h);
  y = max(0, y);
  if (!fn_ || y >= yEnd || w <= 0) return 0;
  caller_ = xTaskGetCurrentTaskHandle();

  // Collect the bands first so the render core can always be one ahead.
  constexpr int kMaxBands = 64;
  Job bands[kMaxBands];
  int n = 0;
  for (int by = y; by < yEnd && n < kMaxBands; by += cfg_.stripRows) {
    int rows = min(cfg_.stripRows, yEnd - by), sx, sw;
    if (span(x, by, w, rows, sx, sw)) bands[n++] = {nullptr, int16_t(sx), int16_t(by), int16_t(sw), int16_t(rows)};
  }
  stats_ = {};
  renderUs_ = 0;
  if (n == 0) return stats_.total = micros() - t0;

  M5.Display.startWrite();
  bands[0].dst = buf_[0];
  runJob(bands[0]);
  uint32_t w0 = micros();
  ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
  stats_.waitRender += micros() - w0;
  for (int i = 0; i < n; i++) {
    if (i + 1 < n) {
      bands[i + 1].dst = buf_[(i + 1) & 1];
      runJob(bands[i + 1]);  // renders into the other buffer
    }
    const Job &b = bands[i];
    // Returns once the transfer is queued; the next setWindow waits for it.
    M5.Display.pushImageDMA(b.x, b.y, b.w, b.rows, reinterpret_cast<const lgfx::swap565_t *>(b.dst));
    M5.Display.waitDMA();
    if (i + 1 < n) {
      w0 = micros();
      ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
      stats_.waitRender += micros() - w0;
    }
  }
  uint32_t f0 = micros();
  M5.Display.endWrite();  // the last transfers only complete here
  stats_.flush = micros() - f0;
  stats_.render = renderUs_;
  stats_.bands = n;
  return stats_.total = micros() - t0;
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
