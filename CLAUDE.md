# m5-stopwatch

Firmware playground for an M5Stack StopWatch (C152, ESP32-S3, 468x466 round AMOLED).
Read README.md first: it has flashing/restore commands and the measured display findings.

- Build: `arduino-cli compile -b m5stack:esp32:m5stack_stopwatch --library lib/StopWatchDisplay sketches/<name>`
- Upload: `arduino-cli upload -b m5stack:esp32:m5stack_stopwatch -p "$(ls /dev/cu.usbmodem* | head -1)" sketches/<name>`
  (the port name changes with the USB socket)
- Keep the default partition scheme so the init() profile storage survives uploads.
- Build apps on `swd::Engine` + layers (`Engine.h`, `Layers.h`; see `sketches/ball`): layers
  change state in `update()` and `invalidate()` what changes; the engine redraws only that,
  chasing the panel scan, at up to 60 fps full screen. Don't draw with `M5.Display` while
  the engine runs. Check `tornCount()` / the stats pill rather than guessing about tearing.
- The scan model (row r at TE + 2.95 ms + r x 29.7 us) is in README "Display timing";
  don't guess display timing, measure it.
- Verify on the device: read serial output (115200) for timings; the user judges tearing visually.
- Board reference: `archive/workos/SKILL.md`, `archive/m5stack/` (local only, gitignored).
