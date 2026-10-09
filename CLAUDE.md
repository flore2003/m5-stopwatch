# m5-stopwatch

Firmware playground for an M5Stack StopWatch (C152, ESP32-S3, 468x466 round AMOLED).
Read README.md first: it has flashing/restore commands and the measured display findings.

- Build: `arduino-cli compile -b m5stack:esp32:m5stack_stopwatch --library lib/StopWatchDisplay sketches/<name>`
- Upload: `arduino-cli upload -b m5stack:esp32:m5stack_stopwatch -p /dev/cu.usbmodem2101 sketches/<name>`
- Keep the default partition scheme so the init() profile storage survives uploads.
- Use `lib/StopWatchDisplay` (`swd::`) for anything animated: TE vsync on GPIO38, frame
  pacing, two-core SRAM strip rendering. Full-screen updates max out at 30 fps; small
  regions can run at 60. Don't push full frames from PSRAM (tears).
- Verify on the device: read serial output (115200) for timings; the user judges tearing visually.
- Board reference: `archive/workos/SKILL.md`, `archive/m5stack/` (local only, gitignored).
