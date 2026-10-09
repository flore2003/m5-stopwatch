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
arduino-cli compile -b m5stack:esp32:m5stack_stopwatch --library lib/StopWatchDisplay sketches/ball
arduino-cli upload -b m5stack:esp32:m5stack_stopwatch -p "$(ls /dev/cu.usbmodem* | head -1)" sketches/ball
arduino-cli monitor -p "$(ls /dev/cu.usbmodem* | head -1)" -c baudrate=115200
```

The port name depends on the USB socket (`/dev/cu.usbmodem101`, `...2101`, …).

## The display library (`lib/StopWatchDisplay`, namespace `swd`)

Engine level (`Engine.h`, `Layers.h`), for apps:
- `Engine`: `begin()` after `M5.begin()`, `add(layer)` back to front, `frame()` in `loop()`.
  Each frame waits for the next refresh, calls `update()` on every layer and redraws only
  what the layers invalidated (one full frame if that's most of the screen).
- `Layer`: override `update(FrameInfo)` (main core: change state, `invalidate(rect)` what
  changes; `FrameInfo::dt` counts whole refreshes) and `render(Span)` (render core: only
  read state). A layer that animates more slowly just invalidates less often; areas
  redrawn for other layers show it unchanged.
- `GradientLayer` (interval N = update every N refreshes), `SpriteLayer` (M5GFX canvas with
  a transparent key color; `moveTo()` invalidates old and new area), `StatsLayer` (fps and
  tear count pill).

Low level (`StopWatchDisplay.h`): `setupDisplay()` (geometry fix), `Vsync` (TE refresh
counter, measured period), `FramePacer`, `StripRenderer` (render callback, two-core SRAM
strips DMA'd straight to the panel, circle-only spans, scan chasing, per-draw stats with a
tear check, `drawCount()`/`tornCount()`), `Overlay`.

Don't draw with `M5.Display` while the engine runs: M5GFX's framebuffer no longer matches
the screen and its next flush would overwrite it.

Display gotcha (from ESPtember): right after `M5.begin()`, set the panel to 468×466 with
`offset_x = 6`, rotation 0, and never draw rows 466–467, or a black bar appears
(`setupDisplay()` / `Engine::begin()` do this).

## Display timing (measured)

The CO5300 refreshes at 59.68 Hz (16.76 ms) and pulses its tearing-effect (TE) pin, GPIO38,
once per refresh. M5GFX enables TE but never reads it. After the TE edge the panel blanks
for ~2.95 ms, then scans rows top to bottom at ~29.7 µs per row (13.8 ms for 466 rows):
**row r is scanned at TE + 2.95 ms + r × 29.7 µs.** Period and row time come from
`sketches/scan_probe` (moves the TE line with command `0x44`); the blanking was tuned by
eye with the ball sketch.

**Tear-free rule:** each row must be written entirely before or after the scan passes it,
and all rows of one update must land in the same refresh. `StripRenderer` chases the scan
by default (`followScan`): each strip waits until the scan has passed it, so an update
appears exactly one refresh later. Its tear check measures every strip's DMA window and
reports `torn`, `slackUs` and the refresh that shows it; `sketches/tear_test` validates it
(chasing ON / OFF: 0 torn, NO SYNC: ~25% torn, matching what's visible).

Bus and frame times:
- M5GFX's StopWatch setup draws into a PSRAM framebuffer and copies its dirty bounding box
  to the panel on `endWrite()`. Going through it, a full frame cost ~24 ms (two copies).
  `StripRenderer` bypasses it and DMAs SRAM strips straight to `Panel_AMOLED`: a full frame
  takes ~9.5 ms unsynced (~105 fps ceiling) and ~16.3 ms when chasing the scan, so the
  gradient runs full screen at 60 fps.
- Rendering is now the bottleneck (~7–9 ms per gradient frame on the second core). Keep
  render callbacks lean (`lastStats().waitRender`).
- A full frame written fast enough also lands cleanly without chasing (it starts before
  the scan and stays ahead, showing a refresh earlier with more slack). Partial updates
  and slow writes need chasing.

What runs where:
- `sketches/anim_bg`: full-screen gradient at 60 fps (engine).
- `sketches/ball`: ball over the gradient (engine). KEY A: gradient every refresh (full
  frames) or every second refresh (engine draws 30 full frames + 30 ball patches per
  second). Both 60 fps, 0 torn.
- `sketches/tear_test`: tearing test pattern; `sketches/scan_probe`: scan timing probe.
- Dead ends kept for reference: alternating screen halves (one half shows a refresh late,
  so things tear where they meet) and a scan model without the 2.95 ms blanking (patches
  land a refresh early: 30 fps look plus tearing).

M5GFX notes: `Panel_AMOLED::command_list()` sends raw panel commands (include
`lgfx/v1/panel/Panel_AMOLED.hpp`); the real panel is the `_panel` member of
`M5.Display.getPanel()` (a `Panel_AMOLED_Framebuffer`).
