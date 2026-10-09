// Analog watch face with a smoothly sweeping second hand, on swd::Engine.
//
// Everything is vector: the dial (ticks, batons, ring) is drawn once from
// shapes into a cached ImageLayer, plus the date in an anti-aliased font.
// The hands are paths (leaf-shaped Bezier outlines, a stroked lollipop ring)
// defined once pointing up and rotated by a transform every frame. Only
// the hands' old and new areas are redrawn each refresh.
//
// Time: the RTC (RX8130) counts whole seconds in UTC (as the init() badge
// installer sets it), so at boot we wait for its next tick and start the
// ESP32 clock on it; the hands then read microsecond time. The time zone is
// a POSIX TZ string kept in flash. Over serial:
//   T<unix seconds>   set the clock (UTC), e.g. T$(date +%s)
//   Z<posix tz>       set the time zone, e.g. Z$(tail -n1 /etc/localtime) on a Mac
//
// KEY A toggles the stats pill (fps and tear count).
#include <M5Unified.h>
#include <Layers.h>
#include <Path.h>
#include <Preferences.h>
#include <sys/time.h>

using swd::Shape;

static constexpr float kCX = swd::kCenterX, kCY = swd::kCenterY;
static constexpr uint32_t kWhite = 0xF2F4F8, kAccent = 0xFF6A2B, kTick = 0x7D879C, kShadow = 0x000000;
static constexpr uint32_t kDialInner = 0x222B3F, kDialOuter = 0x05070C;

static swd::Engine engine;
static swd::ImageLayer dial;
static swd::StatsLayer stats(engine);

// Point at `radius` from the center, `deg` clockwise from 12 o'clock.
static void polar(float deg, float radius, float &x, float &y) {
  float a = deg * PI / 180;
  x = kCX + sinf(a) * radius;
  y = kCY - cosf(a) * radius;
}

// ---- Time

static bool clockValid = false;

static Preferences prefs;

static void applyTimeZone(const String &tz) {
  setenv("TZ", tz.length() ? tz.c_str() : "UTC0", 1);
  tzset();
}

// Local seconds since midnight (with microseconds) and the calendar date.
static double now(tm &out) {
  timeval tv;
  gettimeofday(&tv, nullptr);
  time_t t = tv.tv_sec;
  localtime_r(&t, &out);
  return out.tm_hour * 3600.0 + out.tm_min * 60.0 + out.tm_sec + tv.tv_usec / 1e6;
}

static void setClock(time_t t) {
  timeval tv = {t, 0};
  settimeofday(&tv, nullptr);
}

// Start the ESP32 clock on an RTC second boundary.
static bool syncFromRtc() {
  setenv("TZ", "UTC0", 1);  // the RTC holds UTC; mktime below must not shift it
  tzset();
  m5::rtc_datetime_t dt;
  if (!M5.Rtc.getDateTime(&dt) || dt.date.year < 2024) return false;
  int8_t start = dt.time.seconds;
  uint32_t t0 = millis();
  while (millis() - t0 < 1100) {
    M5.Rtc.getDateTime(&dt);
    if (dt.time.seconds != start) break;
    delay(1);
  }
  tm t = dt.get_tm();
  setClock(mktime(&t));
  return true;
}

static void loadTimeZone() {
  prefs.begin("watch", false);
  applyTimeZone(prefs.getString("tz", "UTC0"));
}

// ---- Dial (drawn once into the cache, redrawn when the date changes)

static int shownDay = -1;

static void drawDial(const tm &date) {
  dial.radialGradient(kDialInner, kDialOuter);
  swd::ShapeLayer shapes;  // a scratch layer, painted into the image in batches
  shapes.add(Shape::ring(kCX, kCY, 227, 1.5f, 0x2C3549));
  for (int i = 0; i < 60; i++) {
    if (i % 5 == 0) continue;
    float ax, ay, bx, by;
    polar(i * 6, 213, ax, ay);
    polar(i * 6, 221, bx, by);
    shapes.add(Shape::capsule(ax, ay, bx, by, 0.9f, kTick));
    if (shapes.count() == swd::ShapeLayer::kMaxShapes) {
      dial.paint(shapes);
      shapes.clear();
    }
  }
  for (int h = 0; h < 12; h++) {
    float ax, ay, bx, by;
    polar(h * 30, h == 0 ? 176 : 188, ax, ay);
    polar(h * 30, 220, bx, by);
    if (h == 0) {
      // Double baton at 12.
      for (float off : {-4.5f, 4.5f}) {
        float px = cosf(0) * off;
        shapes.add(Shape::capsule(ax + px, ay, bx + px, by, 2.6f, kWhite));
      }
    } else {
      shapes.add(Shape::capsule(ax, ay, bx, by, 3.4f, kWhite));
    }
  }
  // Date window at 3 o'clock.
  constexpr float kWinW = 78, kWinH = 34, kWinX = kCX + 118, kWinY = kCY - kWinH / 2;
  shapes.add(Shape::roundRect(kWinX - 1.5f, kWinY - 1.5f, kWinW + 3, kWinH + 3, 9, 0x3A4459));
  shapes.add(Shape::roundRect(kWinX, kWinY, kWinW, kWinH, 8, 0x0A0D14));
  dial.paint(shapes);

  static const char *kDays[] = {"SUN", "MON", "TUE", "WED", "THU", "FRI", "SAT"};
  auto &c = dial.canvas();
  c.setFont(&fonts::DejaVu18);
  c.setTextDatum(middle_left);
  c.setTextColor(0x8C95A8u);
  c.drawString(clockValid ? kDays[date.tm_wday] : "---", kWinX + 9, kWinY + kWinH / 2 + 1);
  char day[4];
  snprintf(day, sizeof(day), "%d", clockValid ? date.tm_mday : 0);
  c.setTextDatum(middle_right);
  c.setTextColor(0xF2F4F8u);
  c.drawString(day, kWinX + kWinW - 9, kWinY + kWinH / 2 + 1);
  dial.changed();
  shownDay = date.tm_mday;
}

// ---- Hands: paths defined once, pointing up from the pivot at (0, 0), and
// rotated into place with a transform every frame.

struct HandPaths {
  swd::Path hour, minute, second, ring, spine;
  HandPaths() {
    // Leaf-shaped (dauphine) hour and minute hands from cubic curves.
    hour.moveTo(0, 22).cubicTo(7, 14, 9, -38, 0, -122).cubicTo(-9, -38, -7, 14, 0, 22).close();
    minute.moveTo(0, 28).cubicTo(5.5f, 18, 7, -60, 0, -198).cubicTo(-7, -60, -5.5f, 18, 0, 28).close();
    // Second hand: needle plus counterweight, one fill (nonzero winding).
    second.rect(-1.2f, -206, 2.4f, 252).circle(0, 32, 6.5f);
    ring.circle(0, -160, 7);              // lollipop near the tip (stroked)
    spine.moveTo(0, -12).lineTo(0, -96);  // a thin line down the hour hand
  }
};
static HandPaths paths;

class Hands {
 public:
  void begin(swd::VectorLayer &layer) {
    for (swd::PathItem *shadow : {&hourShadow_, &minuteShadow_, &secondShadow_}) shadow->setFill(kShadow, 80);
    hourShadow_.setPath(paths.hour), minuteShadow_.setPath(paths.minute), secondShadow_.setPath(paths.second);
    hour_.setPath(paths.hour), minute_.setPath(paths.minute), second_.setPath(paths.second);
    hour_.setFill(kWhite), minute_.setFill(kWhite), second_.setFill(kAccent);
    hourSpine_.setPath(paths.spine);
    hourSpine_.noFill();
    hourSpine_.setStroke(1.6f, 0x9AA3B5);
    ring_.setPath(paths.ring);
    ring_.noFill();
    ring_.setStroke(2.6f, kAccent);
    cap_.set(Shape::circle(kCX, kCY, 9, kAccent));
    pin_.set(Shape::circle(kCX, kCY, 3, kDialInner));
    for (swd::VectorItem *it : std::initializer_list<swd::VectorItem *>{
             &hourShadow_, &minuteShadow_, &secondShadow_, &hour_, &hourSpine_, &minute_, &second_, &ring_, &cap_, &pin_}) {
      layer.add(*it);
    }
  }

  void update() {
    tm t;
    double s = now(t);
    if (t.tm_mday != shownDay) drawDial(t);
    if (!clockValid) s = 10 * 3600 + 10 * 60 + fmod(s, 60);  // 10:10 until the time is set
    // Hour and minute hands move continuously too; quantizing their angles
    // means they're only redrawn when they visibly move.
    float hour = roundf(fmod(s / 3600, 12) * 30 * 40) / 40;
    float minute = roundf(fmod(s / 60, 60) * 6 * 40) / 40;
    float second = fmod(s, 60) * 6;
    place(hour, {&hour_, &hourSpine_}, hourShadow_);
    place(minute, {&minute_}, minuteShadow_);
    place(second, {&second_, &ring_}, secondShadow_);
  }

 private:
  static void place(float deg, std::initializer_list<swd::PathItem *> items, swd::PathItem &shadow) {
    swd::Transform r = swd::Transform::rotate(deg);
    for (swd::PathItem *it : items) it->setTransform(r.then(swd::Transform::translate(kCX, kCY)));
    shadow.setTransform(r.then(swd::Transform::translate(kCX + 4, kCY + 6)));
  }

  swd::PathItem hourShadow_, minuteShadow_, secondShadow_, hour_, hourSpine_, minute_, second_, ring_;
  swd::ShapeItem cap_, pin_;
};
static Hands hands;

// The vector layer that holds the hands; it animates (the second hand sweeps).
class HandsLayer : public swd::VectorLayer {
 public:
  bool animating() const override { return true; }
  void update(const swd::FrameInfo &) override { hands.update(); }
};
static HandsLayer handsLayer;

static void onEvent(const swd::Event &e) {
  if (e.type == swd::Event::ButtonDown && e.button == swd::Event::A) stats.setVisible(!stats.visible());
}

void setup() {
  Serial.begin(115200);
  Serial.setTxTimeoutMs(0);
  auto cfg = M5.config();
  cfg.fallback_board = m5::board_t::board_M5StopWatch;
  M5.begin(cfg);
  clockValid = syncFromRtc();  // before the engine's input task shares the I2C bus
  loadTimeZone();

  if (!engine.begin()) Serial.println("error: engine init failed");
  if (!dial.begin()) Serial.println("error: dial allocation failed");
  hands.begin(handsLayer);
  stats.begin(60);
  stats.setVisible(false);
  engine.add(dial);
  engine.add(handsLayer);
  engine.add(stats);
  engine.onEvent(onEvent);
  tm t;
  now(t);
  Serial.printf("ready clock_valid=%d local %04d-%02d-%02d %02d:%02d:%02d tz=%s\n", clockValid, t.tm_year + 1900,
                t.tm_mon + 1, t.tm_mday, t.tm_hour, t.tm_min, t.tm_sec, getenv("TZ"));
}

void loop() {
  static uint32_t windowStart = millis(), frames = 0, worstUs = 0, tornStart = 0;
  static char line[64];
  static int len = 0;
  while (Serial.available()) {
    char ch = Serial.read();
    if (ch == '\n' || ch == '\r') {
      line[len] = 0;
      if (line[0] == 'T' && len > 1) {
        time_t t = strtoll(line + 1, nullptr, 10);
        setClock(t);
        tm utc;
        gmtime_r(&t, &utc);
        M5.Rtc.setDateTime(&utc);  // the RTC keeps UTC
        clockValid = true;
        shownDay = -1;  // redraw the date
        Serial.printf("time set %04d-%02d-%02d %02d:%02d:%02d UTC\n", utc.tm_year + 1900, utc.tm_mon + 1,
                      utc.tm_mday, utc.tm_hour, utc.tm_min, utc.tm_sec);
      } else if (line[0] == 'Z' && len > 1) {
        prefs.putString("tz", line + 1);
        applyTimeZone(line + 1);
        shownDay = -1;
        Serial.printf("time zone %s\n", line + 1);
      }
      len = 0;
    } else if (len < int(sizeof(line)) - 1) {
      line[len++] = ch;
    }
  }

  engine.frame();
  frames++;
  worstUs = max(worstUs, engine.lastFrame().drawUs);

  uint32_t elapsed = millis() - windowStart;
  if (elapsed >= 2000) {
    auto &st = engine.renderer().lastStats();
    uint32_t torn = engine.renderer().tornCount();
    Serial.printf("fps=%.1f worst_draw_us=%u torn=%u skipped=%u regions=%u | last: render=%u wait_render=%u slack=%d\n",
                  frames * 1000.0f / elapsed, worstUs, torn - tornStart, engine.skipped(),
                  engine.lastFrame().regions, st.render, st.waitRender, st.slackUs);
    Serial.printf("  layer render us: dial=%u hands=%u stats=%u\n", engine.layerRenderUs(0), engine.layerRenderUs(1),
                  engine.layerRenderUs(2));
    windowStart = millis();
    frames = worstUs = 0;
    tornStart = torn;
  }
}
