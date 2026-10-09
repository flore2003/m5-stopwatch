# m5-stopwatch

Playground for an M5Stack StopWatch (C152): ESP32-S3, 16 MB flash, 8 MB PSRAM,
1.75" 466×466 round AMOLED touch, IMU, RTC, mic/speaker, 450 mAh, USB-C.

Board docs: https://docs.m5stack.com/en/core/StopWatch

## Contents

- `backups/factory-full-16MB.bin`: full flash dump of the original M5Stack
  factory firmware (verified against the device; SHA-256 alongside).
- `badge-fw/v1.2.0/`: WorkOS init() conference badge firmware + `release.json`
  (source: https://github.com/chantastic/stopwatch/tree/v1.2.0).
- `archive/`: offline copy of the WorkOS badge pages, ESPtember guides, M5Stack docs and
  source repos (see `archive/README.md`).

## Flashing

Download mode: plug in USB-C (data cable), hold PWR ~2 s until the green LED lights.
Port is usually `/dev/cu.usbmodem*`.

Restore factory firmware:

```bash
uvx esptool --chip esp32s3 -p /dev/cu.usbmodem2101 -b 921600 write-flash 0 backups/factory-full-16MB.bin
```

Install init() v1.2.0 (from `badge-fw/v1.2.0/`):

```bash
uvx esptool --chip esp32s3 -p /dev/cu.usbmodem2101 -b 921600 erase-flash
uvx esptool --chip esp32s3 -p /dev/cu.usbmodem2101 -b 921600 write-flash --flash-mode dio --flash-freq 80m --flash-size 16MB 0x0 bootloader.bin 0x8000 partition-table.bin 0x10000 firmware.bin
```

After a full erase, finish at https://workos.com/init/badge/install →
"Finish an interrupted first install" (prepares the `ffat` profile storage and
syncs the clock). Without an erase, "Update badge" or "Set clock & check badge" suffices.

## Building sketches

Toolchain: `arduino-cli` with M5Stack board package 3.3.9, M5Unified 0.2.19, M5GFX 0.2.26.
Keep the default partition scheme (`app3M_fat9M_16MB`, same as init()) so uploads only
replace the app and leave the init() profile storage (`nvs`, `ffat`) alone.

```bash
arduino-cli compile -b m5stack:esp32:m5stack_stopwatch --library lib/StopWatchDisplay sketches/anim_bg
arduino-cli upload -b m5stack:esp32:m5stack_stopwatch -p /dev/cu.usbmodem2101 sketches/anim_bg
arduino-cli monitor -p /dev/cu.usbmodem2101 -c baudrate=115200
```

`lib/StopWatchDisplay` is the shared display library (namespace `swd`): `setupDisplay()`
(geometry fix), `Vsync` (TE refresh counter), `FramePacer` (start frames every Nth
refresh), `StripRenderer` (render callback + two-core SRAM strips + DMA, circle-only
spans, `drawFrame`/`drawRows`/`drawRect`, per-draw `lastStats()`), and `Overlay`
(keyed sprite composited inside a render callback). `sketches/anim_bg` is the reference
user. `sketches/tap_color` and `sketches/push_bench` predate it and are standalone.

Display gotcha (from ESPtember): right after `M5.begin()`, set the panel to 468×466 with
`offset_x = 6`, rotation 0, and never draw rows 466–467, or a black bar appears.

## Display timing (measured)

The CO5300 refreshes at 59.68 Hz (16.76 ms) and pulses its tearing-effect (TE) pin, GPIO38,
once per refresh. M5GFX enables TE but never reads it. After the TE edge the panel blanks
for ~2.95 ms, then scans rows top to bottom at ~29.7 µs per row (13.8 ms for 466 rows):
**row r is scanned at TE + 2.95 ms + r × 29.7 µs.** Period and row time come from
`sketches/scan_probe` (moves the TE line with command `0x44`); the blanking was tuned by
eye with `sketches/ball`.

**Tear-free rule:** write each row after the scan has passed it and before it comes round
again. Everything written in one pass then appears together, exactly one refresh later.
`swd::StripRenderer` does this by default (`followScan`): each strip waits until the scan
has passed it.

Bus speed: a full 468×466 RGB565 frame takes ~22–24 ms over QSPI, so full-screen updates
top out at 30 fps; ~30 ms per frame is the limit before the next scan overtakes the write
(a full-frame sprite push from PSRAM, ~31 ms, always tears). Sending only the visible circle
(84% of pixels) saves ~2.7 ms. Render callbacks must keep up: the sending core waits on the
renderer at the start of each frame (`lastStats().waitRender`).

What runs where:
- `sketches/anim_bg`: full-screen animated gradient, 30 fps.
- `sketches/ball`: ball at 60 fps over the 30 fps gradient. Full frame every second
  refresh, plus a small patch of the ball's old and new position in between, both chasing
  the scan. Dead ends: alternating screen halves (one half shows a refresh late, so the ball
  tears where they meet) and patching without the 2.95 ms blanking (patches land a refresh
  early: 30 fps look plus tearing).

M5GFX quirks: `pushImageDMA` returns before the transfer is done and `waitDMA()` doesn't
wait for it; the time only shows up in `endWrite()`. `Panel_AMOLED::command_list()` sends
raw panel commands (include `lgfx/v1/panel/Panel_AMOLED.hpp`).
