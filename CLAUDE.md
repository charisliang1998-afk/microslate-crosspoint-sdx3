# MicroSlate — Xteink X3 port

Context for any Claude session working in this repo. Read this first.

## What this project is

A fork of [MicroSlate](https://github.com/Josh-writes/microslate-firmware), a
distraction-free writing firmware (BLE keyboard -> notes on SD card) written for
the **Xteink X4**. This fork ports it to the **Xteink X3**.

End goal: **dual-boot CrossPoint (e-reader) + MicroSlate (writer)** on one X3,
using the two OTA app slots.

The owner is not a developer. Explain things plainly, avoid jargon dumps, and
walk through terminal steps concretely rather than assuming background knowledge.

## Hardware situation — READ BEFORE FLASHING ANYTHING

- **The device is USB-flash-locked.** Confirmed properly: a 4-pin (data-capable)
  pogo cable still yields "no compatible device found" in the web flasher, so
  this is a genuine lock, not the common charge-only-cable false alarm.
- **Flashing is SD-card only.** Rename the app image to exactly `update.bin`,
  place it at the ROOT of a FAT32 card, insert, then hold **LEFT side button +
  POWER**. The Xteink OEM bootloader picks it up. This has been done
  successfully and is the proven route in and out.
- **UP + Power recovery mode does NOT work on this unit.** Some guides mention a
  microSD firmware-picker; it was tested with a valid `update.bin` present and
  did nothing. Do not rely on it.
- **The bootloader route is the safety net.** It runs before the app image, so a
  bad MicroSlate build should still be recoverable by flashing CrossPoint back.
  Keep a known-good CrossPoint `update.bin` available at all times.
- CrossPoint is currently installed and working on the device.
- Beware: community docs warn that flashing non-CrossPoint/CrossInk firmware on
  a locked device carries brick risk. Judged acceptable here because the SD
  bootloader path is bootloader-level, and `partitions.csv` deliberately matches
  CrossPoint's layout.

## Build situation

- **Cannot be built on the owner's Mac.** It's Apple Silicon, and MicroSlate uses
  `framework = arduino, espidf`, whose toolchain has no Mac ARM support.
- **Builds run on GitHub Actions** — see `.github/workflows/build.yml`. Push,
  then read the run. Artifacts: `microslate-x3-update.bin` (rename to
  `update.bin` for SD flashing) and `dualboot-x3.bin`.
- `gh run list` / `gh run view --log-failed` is the fastest way to read errors.

## X3 vs X4 — what actually differs

Same ESP32-C3, same display SPI pins (SCLK 8 / MOSI 10 / CS 21 / DC 4 / RST 5 /
BUSY 6), same SD wiring. The differences that matter:

| | X4 | X3 |
|---|---|---|
| Panel controller | SSD1677 | **UC8253** (older run) or **UC8279d** (newer) |
| Resolution | 800x480 | **792x528** |
| BUSY signal | HIGH = busy | **LOW = busy, two-phase** |
| RAM row order | top-to-bottom | **bottom-to-top** |
| Display SPI clock | 40 MHz | **20 MHz** (UC8253 datasheet max) |
| GPIO13 | battery latch | **SD-card power rail, active HIGH** |
| Battery sensing | ADC on GPIO0 | BQ27220 fuel gauge, I2C 0x55 (SDA 20 / SCL 0) |
| USB | USB-C | pogo pin only — no USB detect line |

## What has been changed so far

- `lib/EInkDisplay/` — rewritten for UC8253. Public API kept identical to the X4
  version on purpose, so `HalDisplay`, `GfxRenderer` and all app code build
  unchanged. Waveform LUTs in `src/Uc8253X3Luts.h` are taken verbatim from
  CrossPoint's proven X3 driver (namespaced `uc8253`).
- **Grayscale is stubbed.** Verified that MicroSlate's app code never calls the
  grayscale path — no call sites outside GfxRenderer's own unused wrappers. Do
  not port the 4-level AA machinery unless something starts needing it.
- `lib/hal/HalGPIO.*` — GPIO13 driven HIGH at boot, LOW before deep sleep;
  `isUsbConnected()` returns false.
- `platformio.ini` — env renamed `xteink_x4` -> `xteink_x3`, `-DXTEINK_X3_HARDWARE=1`,
  serial logging left ON for bring-up (`RELEASE_BUILD` removed).
- `.github/workflows/build.yml` — added. Josh's `release.yml` was removed.

## Known unknowns / likely failure points

1. **UC8253 vs UC8279.** Unresolved — the sticker doesn't say and there's no
   About screen in CrossPoint. The UC8253 driver was written first as the more
   likely and better-tested option. **A blank or scrambled screen on first boot
   most likely means this device is UC8279**, which needs a different driver
   (see freeink-sdk `Uc8279Driver`).
2. **BUSY polarity** (`waitWhileBusy`). If the UI hangs on the first draw, look
   here first.
3. **Resolution register.** `initDisplayController()` programs 792x600, not
   792x528, mirroring the shipping X3 init. If the image is vertically offset or
   squashed, change that pair to `0x02 0x58`.
4. **RAM.** Framebuffer grows 48,000 -> 52,272 bytes. The C3 has ~380KB usable
   and BLE/WiFi stacks are hungry. Watch free heap.
5. **ADC button ladder** in `lib/InputManager` is calibrated from X4 devices and
   has NOT been re-measured on X3, whose buttons are physically arranged
   differently. If buttons misbehave, log raw `adc1_get_raw` values and retune
   `ADC_RANGES_1` / `ADC_RANGES_2`.
6. **Battery** still reads the X4's ADC path. Not yet ported to the BQ27220.
   Expect wrong percentages until it is.
7. Nothing in this port has been compiled or run. Expect compile errors first.

## Dual-boot, once MicroSlate boots

`partitions.csv` already matches CrossPoint's layout (two 6.5MB OTA slots), so no
partition work is needed. MicroSlate's OTA switching in `src/main.cpp` is
hardware-agnostic — it registers its name in the `ota_names` NVS namespace and
calls `esp_ota_set_boot_partition()`.

Two caveats:
- Josh's prebuilt CrossPoint (`typeslate.com/tools/crosspoint/firmware/app.bin`)
  is **X4-only**. Build upstream CrossPoint instead — its default env sets both
  `FREEINK_DEVICE_X4` and `FREEINK_DEVICE_X3` and picks the driver at boot.
- Upstream CrossPoint has **no reciprocal "switch app" menu entry** — its
  `OtaBootSwitch` is only called from `FirmwareFlasher`. Without a small patch
  you can go MicroSlate -> CrossPoint but not back without reflashing.
- CrossPoint's `OtaBootSwitch.h` notes that `esp_ota_set_boot_partition()` can be
  rejected on these devices with bogus efuse errors; CrossPoint works around it
  by writing otadata raw plus a linker `--wrap`. MicroSlate has neither. If
  switching fails with an image-verify error, port `ota_boot::switchTo()`.

## Useful upstream references

- `github.com/crosspoint-reader/crosspoint-reader` — CrossPoint (X3 + X4)
- `github.com/Free-Ink/freeink-sdk` — the HAL with both X3 panel drivers,
  `BoardConfig.h` board profiles, and `docs/xteink-x3-uc8279-support.md`
- `github.com/Josh-writes/microslate-firmware` — upstream MicroSlate (X4)
