// Measures the CO5300's scan timing by moving the tearing-effect (TE) line.
//
// Command 0x44 sets the scanline at which the panel raises TE. For each
// line L we time-stamp TE edges and compute their phase within the refresh
// period. phase(L) - phase(466) tells when the scan reaches line L relative
// to the default TE point, which gives scan direction, speed and blanking.
// Results go to serial. The screen stays black.
#include <M5Unified.h>
#include <StopWatchDisplay.h>
#include <lgfx/v1/panel/Panel_AMOLED.hpp>

static constexpr int kPinTE = 38;
static constexpr int kMax = 256;
static volatile uint32_t riseUs[kMax], fallUs[kMax];
static volatile uint32_t rises = 0, falls = 0;

static void IRAM_ATTR onTE() {
  uint32_t t = micros();
  if (digitalRead(kPinTE)) riseUs[rises++ % kMax] = t;
  else fallUs[falls++ % kMax] = t;
}

static void setTeLine(int line) {
  const uint8_t cmds[] = {0x44, 2, uint8_t(line >> 8), uint8_t(line & 0xFF), 0xFF, 0xFF};
  auto *panel = static_cast<lgfx::Panel_AMOLED *>(M5.Display.getPanel());
  panel->command_list(cmds);
}

// Collects n rising edges; returns their timestamps and the mean pulse width.
static int collect(uint32_t *out, int n, float &widthUs) {
  uint32_t start = rises;
  while (rises - start < uint32_t(n + 1)) delay(1);
  float w = 0;
  int count = 0;
  for (int i = 0; i < n; i++) {
    uint32_t idx = (start + 1 + i) % kMax;
    out[i] = riseUs[idx];
    uint32_t fall = fallUs[idx];
    if (int32_t(fall - out[i]) > 0 && fall - out[i] < 5000) { w += fall - out[i]; count++; }
  }
  widthUs = count ? w / count : -1;
  return n;
}

void setup() {
  Serial.begin(115200);
  Serial.setTxTimeoutMs(0);
  auto cfg = M5.config();
  cfg.fallback_board = m5::board_t::board_M5StopWatch;
  M5.begin(cfg);
  swd::setupDisplay();
  M5.Display.fillScreen(TFT_BLACK);
  pinMode(kPinTE, INPUT);
  attachInterrupt(digitalPinToInterrupt(kPinTE), onTE, CHANGE);
  delay(2500);  // let the host open the port

  // Exact refresh period from 120 edges at the default line.
  static uint32_t ts[130];
  float width;
  setTeLine(466);
  delay(100);
  collect(ts, 121, width);
  double period = double(ts[120] - ts[0]) / 120.0;
  const uint32_t ref = ts[0];
  Serial.printf("probe period_us=%.2f (%.3f Hz) pulse_width_us=%.0f\n", period, 1e6 / period, width);

  auto phaseOf = [&](uint32_t *t, int n) {
    // Circular mean of (t - ref) mod period, in microseconds.
    double sx = 0, sy = 0;
    for (int i = 0; i < n; i++) {
      double ph = fmod(double(t[i] - ref), period) / period * 2 * PI;
      sx += cos(ph);
      sy += sin(ph);
    }
    double a = atan2(sy, sx);
    if (a < 0) a += 2 * PI;
    return a / (2 * PI) * period;
  };

  // Lines beyond ~480 produce no TE pulses (collect() would wait forever).
  const int lines[] = {466, 0, 1, 58, 116, 175, 233, 291, 350, 408, 464, 465, 466, 467, 470, 475, 479, 480};
  double base = 0;
  for (int line : lines) {
    setTeLine(line);
    delay(60);  // settle a few refreshes
    collect(ts, 30, width);
    double ph = phaseOf(ts, 30);
    if (line == 466 && base == 0) base = ph;
    double d = fmod(ph - base + period, period);
    Serial.printf("probe line=%3d phase_us=%8.1f delta_from_466_us=%8.1f pulse_width_us=%.0f\n", line, ph, d, width);
  }
  setTeLine(466);
  Serial.println("probe done");
}

void loop() { delay(1000); }
