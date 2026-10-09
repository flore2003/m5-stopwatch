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

Tearing: the panel TE signal is on GPIO38 (60 Hz, M5GFX enables it but never reads it).
For tear-free full-screen changes, wait for the TE rising edge, then write top to bottom
fast enough that the scan never overtakes the write. A streamed `fillScreen` (~24 ms) is clean; a full-frame sprite push
from PSRAM (~31 ms) tears regardless of sync or write direction. See `sketches/tap_color`.

Full-screen animation (`sketches/anim_bg`): render into two 32-row SRAM strips on the second
core while the main core pushes them, start each frame on a TE edge, present every second
refresh. Measured: ~22 ms bus-only, ~23.6 ms with the gradient, steady 30 fps; 60 fps is
out of reach for full-screen updates. Sending only the visible circle (84% of pixels) saves
~2.7 ms. Keep render callbacks lean: the sending core waits on the renderer at the start of
each frame (check `lastStats().waitRender`); clamping per pixel and 16-bit stores cost the
gradient ~7 ms per frame.
M5GFX quirks: `pushImageDMA` returns before the transfer is done and `waitDMA()` does not
wait for it; the time only shows up in `endWrite()`.
