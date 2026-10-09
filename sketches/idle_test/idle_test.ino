// Idle and input test for swd::Engine.
//
// A static background and a square that jumps to wherever you tap. Nothing
// animates, so between taps the engine sleeps until the next event instead of
// waking every refresh. Serial prints, every 2 s: frames, draws, idle sleeps,
// events and the share of time the loop task was asleep.
// Sending 't' over serial simulates a tap at a random spot.
#include <M5Unified.h>
#include <Layers.h>

static swd::Engine engine;
static swd::SolidLayer background(swd::rgb565(20, 24, 40));

class Marker : public swd::SpriteLayer {
 public:
  bool begin() {
    if (!SpriteLayer::begin(40, 40)) return false;
    canvas().fillRoundRect(0, 0, 40, 40, 8, TFT_ORANGE);
    canvas().fillRoundRect(8, 8, 24, 24, 4, TFT_WHITE);
    moveTo(swd::kCenterX - 20, swd::kCenterY - 20);
    return true;
  }
  bool onEvent(const swd::Event &e) override {
    if (e.type != swd::Event::TouchDown && e.type != swd::Event::TouchMove) return false;
    moveTo(e.x - 20, e.y - 20);
    Serial.printf("touch %d,%d\n", e.x, e.y);
    return true;
  }
};
static Marker marker;

void setup() {
  Serial.begin(115200);
  Serial.setTxTimeoutMs(0);
  auto cfg = M5.config();
  cfg.fallback_board = m5::board_t::board_M5StopWatch;
  M5.begin(cfg);
  if (!engine.begin()) Serial.println("error: engine init failed");
  marker.begin();
  engine.add(background);
  engine.add(marker);
  Serial.println("ready");
}

void loop() {
  static uint32_t windowStart = millis();
  static swd::Engine::Counters last = {};
  while (Serial.available()) {
    if (Serial.read() == 't') {
      swd::Event e = {swd::Event::TouchDown};
      e.x = 120 + esp_random() % 228;
      e.y = 120 + esp_random() % 226;
      engine.post(e);
    }
  }

  engine.frame();

  uint32_t elapsed = millis() - windowStart;
  if (elapsed >= 2000) {
    auto c = engine.counters();
    Serial.printf("frames=%u draws=%u idle_sleeps=%u events=%u asleep=%.0f%% torn=%u\n", c.frames - last.frames,
                  c.draws - last.draws, c.idleWaits - last.idleWaits, c.events - last.events,
                  (c.sleptUs - last.sleptUs) / (elapsed * 10.0f), engine.renderer().tornCount());
    last = c;
    windowStart = millis();
  }
}
