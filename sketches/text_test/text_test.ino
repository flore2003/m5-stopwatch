// Live vector text test for swd::TextItem: small-size quality comparison.
//
// The same words at 10-24 px in two columns: left plain (gamma 1), right
// tuned (starts at the TextItem defaults: gamma 2, no snapping). KEY A toggles
// snapping on the right column, KEY B cycles the right column's gamma. A
// tabular counter at the top keeps the engine drawing.
// Serial: 'a' = KEY A, 'b' = KEY B; prints timing.
#include <M5Unified.h>
#include <Layers.h>
#include <Text.h>

static swd::Engine engine;
static swd::SolidLayer background(swd::rgb565(14, 17, 28));

static const float kGammas[] = {1.0f, 1.3f, 1.6f, 2.0f};
static const int kSizes[] = {10, 12, 14, 16, 20, 24};
static constexpr int kRows = sizeof(kSizes) / sizeof(kSizes[0]);

class TextLayer : public swd::VectorLayer {
 public:
  int gammaIdx = 3;    // 2.0, the TextItem default
  bool snap = false;   // the TextItem default

  void begin() {
    counter_.setFont(swd::fonts::InterSemiBold);
    counter_.setSize(44);
    counter_.setAlign(swd::TextItem::Center);
    counter_.setTabularNumbers(true);
    counter_.setPosition(swd::kCenterX, 96);
    add(counter_);

    header(plainHead_, "plain", swd::kCenterX - 12, swd::TextItem::Right);
    header(tunedHead_, "tuned", swd::kCenterX + 12, swd::TextItem::Left);
    // Baselines at fractional y on purpose, as text often lands in real
    // layouts.
    float y = 168;
    for (int i = 0; i < kRows; i++) {
      y += kSizes[i] + 9.4f;
      char label[24];
      snprintf(label, sizeof(label), "%d Hamburg", kSizes[i]);
      for (int side = 0; side < 2; side++) {
        swd::TextItem &t = rows_[i][side];
        t.setFont(swd::fonts::InterRegular);
        t.setSize(kSizes[i]);
        t.setColor(0xE4E7EE);
        t.setAlign(side ? swd::TextItem::Left : swd::TextItem::Right);
        t.setPosition(swd::kCenterX + (side ? 12 : -12), y);
        t.setText(label);
        if (!side) t.setGamma(1);  // left column: plain, linear coverage
        add(t);
      }
    }
    status_.setFont(swd::fonts::InterRegular);
    status_.setSize(15);
    status_.setAlign(swd::TextItem::Center);
    status_.setColor(0xFF6A2B);
    status_.setPosition(swd::kCenterX, 410);
    add(status_);
    apply();
  }

  // Settings -> right column, and the status line.
  void apply() {
    for (int i = 0; i < kRows; i++) {
      rows_[i][1].setGamma(kGammas[gammaIdx]);
      rows_[i][1].setSnapping(snap);
    }
    char label[48];
    snprintf(label, sizeof(label), "right: gamma %.1f · snap %s", kGammas[gammaIdx], snap ? "on" : "off");
    status_.setText(label);
  }

  bool animating() const override { return true; }

  void update(const swd::FrameInfo &f) override {
    char label[16];
    snprintf(label, sizeof(label), "%05u", f.frame);
    counter_.setText(label);
  }

 private:
  void header(swd::TextItem &t, const char *label, float x, swd::TextItem::Align align) {
    t.setFont(swd::fonts::InterSemiBold);
    t.setSize(14);
    t.setColor(0x8C95A8);
    t.setAlign(align);
    t.setPosition(x, 160);
    t.setText(label);
    add(t);
  }

  swd::TextItem counter_, plainHead_, tunedHead_, status_, rows_[kRows][2];
};
static TextLayer text;

static void onEvent(const swd::Event &e) {
  if (e.type != swd::Event::ButtonDown) return;
  if (e.button == swd::Event::A) text.snap = !text.snap;
  else text.gammaIdx = (text.gammaIdx + 1) % 4;
  text.apply();
}

void setup() {
  Serial.begin(115200);
  Serial.setTxTimeoutMs(0);
  auto cfg = M5.config();
  cfg.fallback_board = m5::board_t::board_M5StopWatch;
  M5.begin(cfg);
  if (!engine.begin()) Serial.println("error: engine init failed");
  text.begin();
  engine.add(background);
  engine.add(text);
  engine.onEvent(onEvent);
  Serial.printf("ready, free internal=%u (dma %u) psram=%u\n", heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                heap_caps_get_free_size(MALLOC_CAP_DMA), heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
}

void loop() {
  static uint32_t windowStart = millis(), frames = 0, worstUs = 0, tornStart = 0, worstUpdate = 0;
  while (Serial.available()) {
    char ch = Serial.read();
    if (ch == 'a') engine.post(swd::Event::buttonDown(swd::Event::A));
    if (ch == 'b') engine.post(swd::Event::buttonDown(swd::Event::B));
  }
  engine.frame();
  frames++;
  worstUs = max(worstUs, engine.lastFrame().drawUs);
  worstUpdate = max(worstUpdate, engine.lastFrame().updateUs);

  uint32_t elapsed = millis() - windowStart;
  if (elapsed >= 2000) {
    uint32_t torn = engine.renderer().tornCount();
    Serial.printf("fps=%.1f worst_update_us=%u worst_draw_us=%u torn=%u skipped=%u | gamma=%.1f snap=%d\n",
                  frames * 1000.0f / elapsed, worstUpdate, worstUs, torn - tornStart, engine.skipped(),
                  kGammas[text.gammaIdx], text.snap);
    windowStart = millis();
    frames = worstUs = worstUpdate = 0;
    tornStart = torn;
  }
}
