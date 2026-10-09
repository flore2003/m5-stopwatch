// Live vector text test for swd::TextItem.
//
// A counter that changes every frame (72 px), a line rotating around the
// center, a size ladder (12-28 px) for legibility, and a title. Serial
// prints frame timing and per-layer render time.
#include <M5Unified.h>
#include <Layers.h>
#include <Text.h>

static swd::Engine engine;
static swd::SolidLayer background(swd::rgb565(14, 17, 28));

class TextLayer : public swd::VectorLayer {
 public:
  void begin() {
    title_.setFont(swd::fonts::InterSemiBold);
    title_.setSize(26);
    title_.setAlign(swd::TextItem::Center);
    title_.setColor(0x8C95A8);
    title_.setPosition(swd::kCenterX, 92);
    title_.setText("Live vector text");

    counter_.setFont(swd::fonts::InterSemiBold);
    counter_.setSize(72);
    counter_.setAlign(swd::TextItem::Center);
    counter_.setBaseline(swd::TextItem::Middle);
    counter_.setPosition(swd::kCenterX, swd::kCenterY - 10);

    orbit_.setFont(swd::fonts::InterRegular);
    orbit_.setSize(22);
    orbit_.setAlign(swd::TextItem::Center);
    orbit_.setColor(0xFF6A2B);
    orbit_.setText("rotating • 60 fps • any angle");

    static const int kSizes[] = {12, 16, 20, 28};
    for (int i = 0; i < 4; i++) {
      char text[24];
      snprintf(text, sizeof(text), "%d px Quick brown fox", kSizes[i]);
      ladder_[i].setFont(swd::fonts::InterRegular);
      ladder_[i].setSize(kSizes[i]);
      ladder_[i].setAlign(swd::TextItem::Center);
      ladder_[i].setColor(0xD8DCE6);
      ladder_[i].setPosition(swd::kCenterX, 300 + i * 30 + kSizes[i] / 2);
      ladder_[i].setText(text);
    }
    for (swd::VectorItem *it : std::initializer_list<swd::VectorItem *>{&title_, &counter_, &orbit_, &ladder_[0],
                                                                        &ladder_[1], &ladder_[2], &ladder_[3]}) {
      add(*it);
    }
  }

  bool animating() const override { return true; }
  bool counting = true;
  uint32_t counterUs = 0, orbitUs = 0;
  void toggleOrbit() { orbit_.setVisible(!orbit_.visible()); }

  void update(const swd::FrameInfo &f) override {
    char text[16];
    snprintf(text, sizeof(text), "%05u", f.frame);
    uint32_t t0 = micros();
    if (counting) counter_.setText(text);
    counterUs = max(counterUs, micros() - t0);
    // Orbit text: rotated around the screen center, 150 px out.
    float deg = f.time * 30;
    t0 = micros();
    orbit_.setTransform(swd::Transform::translate(0, -150)
                            .then(swd::Transform::rotate(deg))
                            .then(swd::Transform::translate(swd::kCenterX, swd::kCenterY)));
    orbitUs = max(orbitUs, micros() - t0);
  }

 private:
  swd::TextItem title_, counter_, orbit_, ladder_[4];
};
static TextLayer text;

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
  Serial.println("ready");
}

void loop() {
  static uint32_t windowStart = millis(), frames = 0, worstUs = 0, tornStart = 0, worstUpdate = 0;
  while (Serial.available()) {
    char ch = Serial.read();
    if (ch == 'o') text.toggleOrbit();
    if (ch == 'c') text.counting = !text.counting;
  }
  engine.frame();
  frames++;
  worstUs = max(worstUs, engine.lastFrame().drawUs);
  worstUpdate = max(worstUpdate, engine.lastFrame().updateUs);

  uint32_t elapsed = millis() - windowStart;
  if (elapsed >= 2000) {
    auto &st = engine.renderer().lastStats();
    uint32_t torn = engine.renderer().tornCount();
    Serial.printf("fps=%.1f worst_update_us=%u worst_draw_us=%u torn=%u skipped=%u regions=%u | last: render=%u wait_render=%u | text layer us=%u\n",
                  frames * 1000.0f / elapsed, worstUpdate, worstUs, torn - tornStart, engine.skipped(),
                  engine.lastFrame().regions, st.render, st.waitRender, engine.layerRenderUs(1));
    Serial.printf("  worst update parts: counter setText=%u us, orbit setTransform=%u us\n", text.counterUs, text.orbitUs);
    text.counterUs = text.orbitUs = 0;
    windowStart = millis();
    frames = worstUs = worstUpdate = 0;
    tornStart = torn;
  }
}
