// Vector feature sheet: fill rules, stroke caps and joins, gradients.
//
// Row 1: a five-pointed star as one self-crossing path, nonzero (solid) and
//        even-odd (hollow center).
// Row 2: thick lines with butt, round and square caps over a thin guide
//        that marks the exact end points.
// Row 3: zig-zags with miter, round and bevel joins.
// Row 4: a linear-gradient rounded rect, a radial-gradient disc, and a
//        rotating gradient bar (gradients move with the transform).
// Serial prints timing.
#include <M5Unified.h>
#include <Layers.h>
#include <Text.h>

static swd::Engine engine;
static swd::SolidLayer background(swd::rgb565(14, 17, 28));

static constexpr uint32_t kInk = 0xE4E7EE, kAccent = 0xFF6A2B, kMuted = 0x5A6478;

class Sheet : public swd::VectorLayer {
 public:
  void begin() {
    // Row 1: stars, centered at y = 105.
    for (int i = 0; i < 5; i++) {
      float a = (i * 144 - 90) * PI / 180;
      float x = cosf(a) * 38, y = sinf(a) * 38;
      if (i == 0) star_.moveTo(x, y);
      else star_.lineTo(x, y);
    }
    star_.close();
    for (int i = 0; i < 2; i++) {
      stars_[i].setPath(star_);
      stars_[i].setFill(kAccent);
      stars_[i].setFillRule(i ? swd::FillRule::EvenOdd : swd::FillRule::NonZero);
      stars_[i].setTransform(swd::Transform::translate(swd::kCenterX + (i ? 60 : -60), 105));
      add(stars_[i]);
      label(starLabels_[i], i ? "even-odd" : "nonzero", swd::kCenterX + (i ? 60 : -60), 162);
    }

    // Row 2: caps. Guides first (drawn below the thick lines).
    static const swd::Cap kCaps[] = {swd::Cap::Butt, swd::Cap::Round, swd::Cap::Square};
    static const char *kCapNames[] = {"butt", "round", "square"};
    capLine_.moveTo(0, 0).lineTo(0, 46);
    for (int i = 0; i < 3; i++) {
      float x = swd::kCenterX + (i - 1) * 70;
      capGuides_[i].setPath(capLine_);
      capGuides_[i].noFill();
      capGuides_[i].setStroke(1, kMuted, swd::Cap::Butt);
      capGuides_[i].setTransform(swd::Transform::translate(x, 190));
      add(capGuides_[i]);
      caps_[i].setPath(capLine_);
      caps_[i].noFill();
      caps_[i].setStroke(16, kInk, kCaps[i]);
      caps_[i].setTransform(swd::Transform::translate(x, 190));
      add(caps_[i]);
      label(capLabels_[i], kCapNames[i], x, 268);
    }

    // Row 3: joins on a zig-zag.
    static const swd::Join kJoins[] = {swd::Join::Miter, swd::Join::Round, swd::Join::Bevel};
    static const char *kJoinNames[] = {"miter", "round", "bevel"};
    zigzag_.moveTo(-24, 14).lineTo(-8, -14).lineTo(8, 14).lineTo(24, -14);
    for (int i = 0; i < 3; i++) {
      float x = swd::kCenterX + (i - 1) * 70;
      joins_[i].setPath(zigzag_);
      joins_[i].noFill();
      joins_[i].setStroke(9, kAccent, swd::Cap::Butt, kJoins[i]);
      joins_[i].setTransform(swd::Transform::translate(x, 300));
      add(joins_[i]);
      label(joinLabels_[i], kJoinNames[i], x, 334);
    }

    // Row 4: gradients.
    rect_.roundRect(-34, -22, 68, 44, 10);
    linear_.setPath(rect_);
    linear_.setFill(swd::Paint::linear(-34, 0, 34, 0, {{0, 0x00C6FF}, {0.5f, 0x7B2FF7}, {1, 0xFF5F6D}}));
    linear_.setTransform(swd::Transform::translate(swd::kCenterX - 80, 380));
    add(linear_);
    disc_.circle(0, 0, 26);
    radial_.setPath(disc_);
    radial_.setFill(swd::Paint::radial(-8, -8, 34, {{0, 0xFFFFFF}, {0.35f, 0xFFC371}, {1, 0x845EC2}}));
    radial_.setTransform(swd::Transform::translate(swd::kCenterX, 380));
    add(radial_);
    bar_.roundRect(-30, -7, 60, 14, 7);
    spinner_.setPath(bar_);
    spinner_.setFill(swd::Paint::linear(-30, 0, 30, 0, {{0, 0x00F5A0}, {1, 0x7A00FF}}));
    spinner_.setStroke(1.5f, 0xE4E7EE);
    add(spinner_);
  }

  bool animating() const override { return true; }

  void update(const swd::FrameInfo &f) override {
    spinner_.setTransform(swd::Transform::rotate(f.time * 45).then(swd::Transform::translate(swd::kCenterX + 80, 380)));
  }

 private:
  void label(swd::TextItem &t, const char *text, float x, float y) {
    t.setFont(swd::fonts::InterRegular);
    t.setSize(13);
    t.setColor(0x8C95A8);
    t.setAlign(swd::TextItem::Center);
    t.setPosition(x, y);
    t.setText(text);
    add(t);
  }

  swd::Path star_, capLine_, zigzag_, rect_, disc_, bar_;
  swd::PathItem stars_[2], capGuides_[3], caps_[3], joins_[3], linear_, radial_, spinner_;
  swd::TextItem starLabels_[2], capLabels_[3], joinLabels_[3];
};
static Sheet sheet;

void setup() {
  Serial.begin(115200);
  Serial.setTxTimeoutMs(0);
  auto cfg = M5.config();
  cfg.fallback_board = m5::board_t::board_M5StopWatch;
  M5.begin(cfg);
  if (!engine.begin()) Serial.println("error: engine init failed");
  sheet.begin();
  engine.add(background);
  engine.add(sheet);
  Serial.println("ready");
}

void loop() {
  static uint32_t windowStart = millis(), frames = 0, worstUs = 0, worstUpdate = 0;
  engine.frame();
  frames++;
  worstUs = max(worstUs, engine.lastFrame().drawUs);
  worstUpdate = max(worstUpdate, engine.lastFrame().updateUs);
  uint32_t elapsed = millis() - windowStart;
  if (elapsed >= 2000) {
    Serial.printf("fps=%.1f worst_update_us=%u worst_draw_us=%u torn=%u skipped=%u | layer us=%u\n",
                  frames * 1000.0f / elapsed, worstUpdate, worstUs, engine.renderer().tornCount(), engine.skipped(),
                  engine.layerRenderUs(1));
    windowStart = millis();
    frames = worstUs = worstUpdate = 0;
  }
}
