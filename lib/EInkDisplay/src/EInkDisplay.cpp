#include "EInkDisplay.h"

#include <string.h>

#include "Uc8253X3Luts.h"

// ============================================================================
// UC8253 command set (Xteink X3)
// ============================================================================
namespace {
constexpr uint8_t CMD_PANEL_SETTING = 0x00;
constexpr uint8_t CMD_POWER_SETTING = 0x01;
constexpr uint8_t CMD_POWER_OFF = 0x02;
constexpr uint8_t CMD_POWER_OFF_SEQ = 0x03;
constexpr uint8_t CMD_POWER_ON = 0x04;
constexpr uint8_t CMD_BOOSTER_SOFT_START = 0x06;
constexpr uint8_t CMD_DEEP_SLEEP = 0x07;
constexpr uint8_t CMD_DTM1 = 0x10;  // "old" plane
constexpr uint8_t CMD_DATA_STOP = 0x11;
constexpr uint8_t CMD_DISPLAY_REFRESH = 0x12;
constexpr uint8_t CMD_DTM2 = 0x13;  // "new" plane
constexpr uint8_t CMD_LUT_VCOM = 0x20;
constexpr uint8_t CMD_LUT_WW = 0x21;
constexpr uint8_t CMD_LUT_BW = 0x22;
constexpr uint8_t CMD_LUT_WB = 0x23;
constexpr uint8_t CMD_LUT_BB = 0x24;
constexpr uint8_t CMD_PLL_CONTROL = 0x30;
constexpr uint8_t CMD_VCOM_DATA_INTERVAL = 0x50;
constexpr uint8_t CMD_RESOLUTION = 0x61;
constexpr uint8_t CMD_GATE_SOURCE_START = 0x65;
constexpr uint8_t CMD_VCOM_DC = 0x82;
constexpr uint8_t CMD_LV_SELECTION = 0xE1;

// LUT bank selectors
constexpr uint8_t BANK_NORMAL = 0;
constexpr uint8_t BANK_HALF = 1;
constexpr uint8_t BANK_FAST = 2;
constexpr uint8_t BANK_FULL = 3;
}  // namespace

EInkDisplay::EInkDisplay(int8_t sclk, int8_t mosi, int8_t cs, int8_t dc, int8_t rst, int8_t busy)
    : _sclk(sclk),
      _mosi(mosi),
      _cs(cs),
      _dc(dc),
      _rst(rst),
      _busy(busy),
      frameBuffer(nullptr),
#ifndef EINK_DISPLAY_SINGLE_BUFFER_MODE
      frameBufferActive(nullptr),
#endif
      isScreenOn(false),
      customLutActive(false),
      inGrayscaleMode(false),
      drawGrayscale(false) {
  if (Serial) Serial.printf("[%lu] EInkDisplay(X3/UC8253): ctor\n", millis());
}

void EInkDisplay::begin() {
  if (Serial) Serial.printf("[%lu] EInkDisplay: begin() 792x528 UC8253\n", millis());

  frameBuffer = frameBuffer0;
#ifndef EINK_DISPLAY_SINGLE_BUFFER_MODE
  frameBufferActive = frameBuffer1;
  memset(frameBuffer1, 0xFF, BUFFER_SIZE);
#endif
  memset(frameBuffer0, 0xFF, BUFFER_SIZE);

  SPI.begin(_sclk, -1, _mosi, _cs);
  // 20 MHz = UC8253 datasheet max for serial writes. The X4 ran 40 MHz on an
  // SSD1677; pushing the UC8253 that hard glitches plane writes.
  spiSettings = SPISettings(20000000, MSBFIRST, SPI_MODE0);

  pinMode(_cs, OUTPUT);
  pinMode(_dc, OUTPUT);
  pinMode(_rst, OUTPUT);
  pinMode(_busy, INPUT);
  digitalWrite(_cs, HIGH);
  digitalWrite(_dc, HIGH);

  resetDisplay();
  initDisplayController();

  oldPlaneValid = false;
  initialFullSyncsRemaining = 1;

  if (Serial) Serial.printf("[%lu]   UC8253 init complete\n", millis());
}

// ============================================================================
// Low level
// ============================================================================

void EInkDisplay::resetDisplay() {
  digitalWrite(_rst, HIGH);
  delay(20);
  digitalWrite(_rst, LOW);
  delay(2);
  digitalWrite(_rst, HIGH);
  delay(50);  // X3 needs a longer settle after reset than the X4
}

void EInkDisplay::sendCommand(uint8_t command) {
  SPI.beginTransaction(spiSettings);
  digitalWrite(_dc, LOW);
  digitalWrite(_cs, LOW);
  SPI.transfer(command);
  digitalWrite(_cs, HIGH);
  SPI.endTransaction();
}

void EInkDisplay::sendData(uint8_t data) {
  SPI.beginTransaction(spiSettings);
  digitalWrite(_dc, HIGH);
  digitalWrite(_cs, LOW);
  SPI.transfer(data);
  digitalWrite(_cs, HIGH);
  SPI.endTransaction();
}

void EInkDisplay::sendData(const uint8_t* data, uint16_t length) {
  SPI.beginTransaction(spiSettings);
  digitalWrite(_dc, HIGH);
  digitalWrite(_cs, LOW);
  SPI.writeBytes(data, length);
  digitalWrite(_cs, HIGH);
  SPI.endTransaction();
}

// LUT tables live in PROGMEM; copy to a stack buffer before sending.
void EInkDisplay::sendDataProgmem(const uint8_t* data, uint16_t length) {
  uint8_t buf[64];
  uint16_t sent = 0;
  while (sent < length) {
    const uint16_t n = (length - sent) < sizeof(buf) ? (length - sent) : sizeof(buf);
    memcpy_P(buf, data + sent, n);
    sendData(buf, n);
    sent += n;
  }
}

// UC8253 BUSY is ACTIVE LOW and two-phase: the controller pulls it LOW when the
// operation starts, then releases it HIGH when done. This is the OPPOSITE of the
// SSD1677 on the X4, which simply holds BUSY HIGH while busy. If you ever see the
// UI freeze on the first refresh, this function is the first place to look.
void EInkDisplay::waitWhileBusy(const char* comment) {
  const unsigned long start = millis();

  // Phase 1: wait for the falling edge (operation actually started).
  while (digitalRead(_busy) == HIGH) {
    delay(1);
    if (millis() - start > 1000) {
      // Missed the edge - the op may already be finished. Not fatal.
      if (comment && Serial) Serial.printf("[%lu]   busy: no LOW edge%s\n", millis(), comment);
      return;
    }
  }

  // Phase 2: wait for the release back to HIGH.
  while (digitalRead(_busy) == LOW) {
    delay(1);
    if (millis() - start > 30000) {
      if (Serial) Serial.printf("[%lu]   busy TIMEOUT%s\n", millis(), comment ? comment : "");
      break;
    }
  }

  if (comment && Serial) {
    Serial.printf("[%lu]   Wait complete: %s (%lu ms)\n", millis(), comment, millis() - start);
  }
}

void EInkDisplay::initDisplayController() {
  sendCommand(CMD_PANEL_SETTING);
  sendData(0x3F);
  sendData(0x0A);

  // Resolution: 0x0318 = 792 wide, 0x0258 = 600 gate lines. The OEM scans the
  // full gate count (600) even though the visible panel is 528 tall - this is
  // taken from the shipping X3 init sequence. If the image appears vertically
  // offset or squashed, this pair is the thing to change.
  sendCommand(CMD_RESOLUTION);
  sendData(0x03);
  sendData(0x18);
  sendData(0x02);
  sendData(0x58);

  sendCommand(CMD_GATE_SOURCE_START);
  sendData(0x00);
  sendData(0x00);
  sendData(0x00);
  sendData(0x00);

  sendCommand(CMD_POWER_OFF_SEQ);
  sendData(0x20);

  sendCommand(CMD_POWER_SETTING);
  sendData(0x07);
  sendData(0x17);
  sendData(0x3F);
  sendData(0x3F);
  sendData(0x17);

  sendCommand(CMD_VCOM_DC);
  sendData(0x24);

  sendCommand(CMD_BOOSTER_SOFT_START);
  sendData(0x25);
  sendData(0x25);
  sendData(0x3C);
  sendData(0x37);

  sendCommand(CMD_PLL_CONTROL);
  sendData(0x09);

  sendCommand(CMD_LV_SELECTION);
  sendData(0x02);

  // Unlike the SSD1677, the UC8253 does not clear its RAM on init. Fill both
  // planes white so the first differential refresh diffs against white rather
  // than whatever the panel happened to be holding.
  fillPlane(CMD_DTM1, 0xFF);
  sendCommand(CMD_DATA_STOP);
  fillPlane(CMD_DTM2, 0xFF);
  sendCommand(CMD_DATA_STOP);

  isScreenOn = false;
}

// ============================================================================
// Plane transfer
// ============================================================================

// The X3 expects RAM rows bottom-to-top. Sent in one CS-low burst for speed.
void EInkDisplay::sendPlaneFlipped(uint8_t ramCmd, const uint8_t* plane) {
  sendCommand(ramCmd);
  SPI.beginTransaction(spiSettings);
  digitalWrite(_dc, HIGH);
  digitalWrite(_cs, LOW);
  for (int y = static_cast<int>(DISPLAY_HEIGHT) - 1; y >= 0; y--) {
    SPI.writeBytes(plane + static_cast<uint32_t>(y) * DISPLAY_WIDTH_BYTES, DISPLAY_WIDTH_BYTES);
  }
  digitalWrite(_cs, HIGH);
  SPI.endTransaction();
}

void EInkDisplay::fillPlane(uint8_t ramCmd, uint8_t fillByte) {
  uint8_t chunk[128];
  memset(chunk, fillByte, sizeof(chunk));
  sendCommand(ramCmd);
  SPI.beginTransaction(spiSettings);
  digitalWrite(_dc, HIGH);
  digitalWrite(_cs, LOW);
  for (uint16_t y = 0; y < DISPLAY_HEIGHT; y++) {
    uint16_t remaining = DISPLAY_WIDTH_BYTES;
    while (remaining) {
      const uint16_t n = remaining < sizeof(chunk) ? remaining : sizeof(chunk);
      SPI.writeBytes(chunk, n);
      remaining -= n;
    }
  }
  digitalWrite(_cs, HIGH);
  SPI.endTransaction();
}

void EInkDisplay::loadLutBank(uint8_t bank) {
  const uint8_t* vcom;
  const uint8_t* ww;
  const uint8_t* bw;
  const uint8_t* wb;
  const uint8_t* bb;

  switch (bank) {
    case BANK_HALF:
      vcom = uc8253::lut_x3_vcom_half;
      ww = uc8253::lut_x3_ww_half;
      bw = uc8253::lut_x3_bw_half;
      wb = uc8253::lut_x3_wb_half;
      bb = uc8253::lut_x3_bb_half;
      break;
    case BANK_FAST:
      vcom = uc8253::lut_x3_vcom_fast;
      ww = uc8253::lut_x3_ww_fast;
      bw = uc8253::lut_x3_bw_fast;
      wb = uc8253::lut_x3_wb_fast;
      bb = uc8253::lut_x3_bb_fast;
      break;
    case BANK_FULL:
      vcom = uc8253::lut_x3_vcom_full;
      ww = uc8253::lut_x3_ww_full;
      bw = uc8253::lut_x3_bw_full;
      wb = uc8253::lut_x3_wb_full;
      bb = uc8253::lut_x3_bb_full;
      break;
    default:
      vcom = uc8253::lut_x3_vcom_normal;
      ww = uc8253::lut_x3_ww_normal;
      bw = uc8253::lut_x3_bw_normal;
      wb = uc8253::lut_x3_wb_normal;
      bb = uc8253::lut_x3_bb_normal;
      break;
  }

  const uint16_t len = uc8253::X3_LUT_LEN;
  sendCommand(CMD_LUT_VCOM);
  sendDataProgmem(vcom, len);
  sendCommand(CMD_LUT_WW);
  sendDataProgmem(ww, len);
  sendCommand(CMD_LUT_BW);
  sendDataProgmem(bw, len);
  sendCommand(CMD_LUT_WB);
  sendDataProgmem(wb, len);
  sendCommand(CMD_LUT_BB);
  sendDataProgmem(bb, len);
}

void EInkDisplay::loadLutBankCdi(uint8_t cdi0, uint8_t cdi1, uint8_t bank) {
  sendCommand(CMD_VCOM_DATA_INTERVAL);
  sendData(cdi0);
  sendData(cdi1);
  loadLutBank(bank);
}

void EInkDisplay::powerOnIfNeeded() {
  if (!isScreenOn) {
    sendCommand(CMD_POWER_ON);
    waitWhileBusy(" X3_PON");
    isScreenOn = true;
  }
}

void EInkDisplay::syncOldPlane(const uint8_t* fb) {
  sendPlaneFlipped(CMD_DTM1, fb);
  sendCommand(CMD_DATA_STOP);
  oldPlaneValid = true;
}

// ============================================================================
// Framebuffer ops (unchanged in behaviour from the X4 version)
// ============================================================================

void EInkDisplay::clearScreen(const uint8_t color) const { memset(frameBuffer, color, BUFFER_SIZE); }

void EInkDisplay::drawImage(const uint8_t* imageData, const uint16_t x, const uint16_t y, const uint16_t w,
                            const uint16_t h, const bool fromProgmem) const {
  if (!imageData) return;
  const uint16_t imgWidthBytes = (w + 7) / 8;
  for (uint16_t row = 0; row < h; row++) {
    const uint16_t destY = y + row;
    if (destY >= DISPLAY_HEIGHT) break;
    for (uint16_t colByte = 0; colByte < imgWidthBytes; colByte++) {
      const uint16_t destXbit = x + colByte * 8;
      if (destXbit >= DISPLAY_WIDTH) break;
      uint8_t srcByte = fromProgmem ? pgm_read_byte(&imageData[row * imgWidthBytes + colByte])
                                    : imageData[row * imgWidthBytes + colByte];
      for (uint8_t bit = 0; bit < 8; bit++) {
        const uint16_t destX = destXbit + bit;
        if (destX >= DISPLAY_WIDTH) break;
        const uint32_t idx = static_cast<uint32_t>(destY) * DISPLAY_WIDTH_BYTES + (destX / 8);
        const uint8_t mask = 0x80 >> (destX % 8);
        if (srcByte & (0x80 >> bit)) {
          frameBuffer[idx] |= mask;
        } else {
          frameBuffer[idx] &= ~mask;
        }
      }
    }
  }
}

void EInkDisplay::setFramebuffer(const uint8_t* bwBuffer) const {
  if (bwBuffer) memcpy(frameBuffer, bwBuffer, BUFFER_SIZE);
}

#ifndef EINK_DISPLAY_SINGLE_BUFFER_MODE
void EInkDisplay::swapBuffers() {
  uint8_t* tmp = frameBuffer;
  frameBuffer = frameBufferActive;
  frameBufferActive = tmp;
}
#endif

// ============================================================================
// Refresh
// ============================================================================

void EInkDisplay::displayBuffer(RefreshMode mode, const bool turnOffScreen) {
  beginRefresh(mode, turnOffScreen);
  while (_refreshState != IDLE) {
    if (!pollRefresh()) delay(2);
  }
}

void EInkDisplay::refreshDisplay(const RefreshMode mode, const bool turnOffScreen) {
  displayBuffer(mode, turnOffScreen);
}

void EInkDisplay::beginRefresh(RefreshMode mode, const bool turnOffScreen) {
  if (_refreshState != IDLE) return;

  // Waking the panel gets a stronger waveform than a plain fast diff.
  if (!isScreenOn && !turnOffScreen && mode == FAST_REFRESH) mode = HALF_REFRESH;

  const bool fastMode = (mode == FAST_REFRESH);
  const bool halfMode = (mode == HALF_REFRESH);
  const bool ghostLimitHit = fastMode && consecutiveFastRefreshes >= FAST_REFRESH_GHOST_LIMIT;
  const bool doFullSync =
      (!fastMode && !halfMode) || !oldPlaneValid || initialFullSyncsRemaining > 0 || ghostLimitHit;
  const bool doHalfSync = halfMode && !doFullSync;
  consecutiveFastRefreshes = (doFullSync || doHalfSync) ? 0 : consecutiveFastRefreshes + 1;

  if (doFullSync) {
    // OEM full bank driven from a white baseline in DTM1.
    loadLutBankCdi(0x29, 0x07, BANK_FULL);
    fillPlane(CMD_DTM1, 0xFF);
    sendCommand(CMD_DATA_STOP);
    sendPlaneFlipped(CMD_DTM2, frameBuffer);
  } else if (doHalfSync) {
    // Scrub bank: WW==BW and WB==BB, so every pixel is driven to its target
    // regardless of what the old plane holds.
    loadLutBankCdi(0xA9, 0x07, BANK_HALF);
    sendPlaneFlipped(CMD_DTM2, frameBuffer);
  } else {
    // Turbo differential against the retained old plane.
    loadLutBankCdi(0x29, 0x07, BANK_FAST);
    sendPlaneFlipped(CMD_DTM2, frameBuffer);
  }

  // A full sync re-powers the charge pump even when already on.
  if (!isScreenOn || doFullSync) {
    sendCommand(CMD_POWER_ON);
    waitWhileBusy(" X3_PON");
    isScreenOn = true;
  }

  sendCommand(CMD_DISPLAY_REFRESH);

  // Confirm the waveform actually started (BUSY went LOW) before handing the CPU
  // back, so pollRefresh() only has to watch for the release edge.
  {
    const unsigned long t0 = millis();
    while (digitalRead(_busy) == HIGH && millis() - t0 < 50) delay(1);
  }

  _pendingMode = mode;
  _pendingTurnOff = turnOffScreen;
  _pendingFullSync = doFullSync;
  _refreshStartMs = millis();
  _refreshState = REFRESHING;
}

bool EInkDisplay::pollRefresh() {
  if (_refreshState != REFRESHING) return true;

  // Still driving the waveform (BUSY low) - let the caller keep working.
  if (digitalRead(_busy) == LOW && millis() - _refreshStartMs < 30000) return false;

  if (_pendingTurnOff) {
    sendCommand(CMD_POWER_OFF);
    waitWhileBusy(" X3_POF");
    isScreenOn = false;
  }

  if (_pendingMode != FAST_REFRESH) delay(200);

  // Sync DTM1 with the frame just shown so the next fast diff has a baseline.
  syncOldPlane(frameBuffer);

  // On the X3 the first differential after a full refresh garbles; spend a
  // no-op fast settle of the same frame so the next real diff is clean.
  if (_pendingFullSync) {
    loadLutBankCdi(0x29, 0x07, BANK_FAST);
    sendPlaneFlipped(CMD_DTM2, frameBuffer);
    powerOnIfNeeded();
    sendCommand(CMD_DISPLAY_REFRESH);
    waitWhileBusy(" X3_SETTLE");
    if (_pendingTurnOff) {
      sendCommand(CMD_POWER_OFF);
      waitWhileBusy(" X3_POF");
      isScreenOn = false;
    }
    syncOldPlane(frameBuffer);
    if (initialFullSyncsRemaining > 0) initialFullSyncsRemaining--;
  }

  _refreshState = IDLE;
  return true;
}

// Windowed update is not implemented for UC8253 in this port; fall back to a
// full-screen fast refresh, which is correct if slower.
void EInkDisplay::displayWindow(uint16_t x, uint16_t y, uint16_t w, uint16_t h, const bool turnOffScreen) {
  (void)x;
  (void)y;
  (void)w;
  (void)h;
  displayBuffer(FAST_REFRESH, turnOffScreen);
}

void EInkDisplay::setCustomLUT(const bool enabled, const unsigned char* lutData) {
  (void)lutData;
  customLutActive = enabled;
}

void EInkDisplay::deepSleep() {
  if (isScreenOn) {
    sendCommand(CMD_POWER_OFF);
    waitWhileBusy(" X3_POF");
    isScreenOn = false;
  }
  sendCommand(CMD_DEEP_SLEEP);
  sendData(0xA5);
  oldPlaneValid = false;
}

// ============================================================================
// Grayscale stubs - MicroSlate's editor never uses these.
// ============================================================================

void EInkDisplay::copyGrayscaleBuffers(const uint8_t* lsbBuffer, const uint8_t* msbBuffer) {
  (void)lsbBuffer;
  (void)msbBuffer;
}
void EInkDisplay::copyGrayscaleLsbBuffers(const uint8_t* lsbBuffer) { (void)lsbBuffer; }
void EInkDisplay::copyGrayscaleMsbBuffers(const uint8_t* msbBuffer) { (void)msbBuffer; }
#ifdef EINK_DISPLAY_SINGLE_BUFFER_MODE
void EInkDisplay::cleanupGrayscaleBuffers(const uint8_t* bwBuffer) { (void)bwBuffer; }
#endif
void EInkDisplay::displayGrayBuffer(const bool turnOffScreen) { displayBuffer(FAST_REFRESH, turnOffScreen); }
void EInkDisplay::grayscaleRevert() {}

void EInkDisplay::saveFrameBufferAsPBM(const char* filename) { (void)filename; }
