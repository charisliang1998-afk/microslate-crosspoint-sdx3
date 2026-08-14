#pragma once
#include <Arduino.h>
#include <SPI.h>

// ============================================================================
// EInkDisplay - Xteink X3 (UC8253, 792x528) port of MicroSlate's display layer.
//
// The X4 original targets an SSD1677 at 800x480. The X3 uses a different panel
// controller (UC8253) at a different resolution, so this is a rewrite of the
// controller-facing half. The PUBLIC API is deliberately identical to the X4
// version so HalDisplay, GfxRenderer and all app code build unchanged.
//
// Key hardware differences handled here:
//   * UC8253 command set (PSR/PON/DRF/DTM1/DTM2), not SSD1677 registers
//   * BUSY is ACTIVE LOW and two-phase (HIGH->LOW, then LOW->HIGH).
//     The SSD1677 is simply "HIGH means busy". Getting this wrong hangs the UI.
//   * RAM rows are written bottom-to-top (see sendPlaneFlipped)
//   * Waveform LUTs must be uploaded by the host; no auto RAM clear on init
//   * SPI runs at 20 MHz (UC8253 datasheet max), not the X4's 40 MHz
//
// Grayscale is stubbed. MicroSlate's app code never calls the grayscale path,
// so the 4-level AA machinery from the reader firmware is not ported. The stubs
// keep the symbols present for the linker.
// ============================================================================

class EInkDisplay {
 public:
  EInkDisplay(int8_t sclk, int8_t mosi, int8_t cs, int8_t dc, int8_t rst, int8_t busy);
  ~EInkDisplay() = default;

  enum RefreshMode {
    FULL_REFRESH,  // OEM full bank from a white baseline + settle pass
    HALF_REFRESH,  // scrub bank: drive every pixel to target, ignore old plane
    FAST_REFRESH   // turbo differential against the retained old plane
  };

  void begin();

  // --- X3 panel geometry (X4 was 800x480) ---
  static constexpr uint16_t DISPLAY_WIDTH = 792;
  static constexpr uint16_t DISPLAY_HEIGHT = 528;
  static constexpr uint16_t DISPLAY_WIDTH_BYTES = DISPLAY_WIDTH / 8;             // 99
  static constexpr uint32_t BUFFER_SIZE = DISPLAY_WIDTH_BYTES * DISPLAY_HEIGHT;  // 52272

  void clearScreen(uint8_t color = 0xFF) const;
  void drawImage(const uint8_t* imageData, uint16_t x, uint16_t y, uint16_t w, uint16_t h,
                 bool fromProgmem = false) const;

#ifndef EINK_DISPLAY_SINGLE_BUFFER_MODE
  void swapBuffers();
#endif
  void setFramebuffer(const uint8_t* bwBuffer) const;

  // Grayscale stubs - retained for API compatibility, no-ops on this port.
  void copyGrayscaleBuffers(const uint8_t* lsbBuffer, const uint8_t* msbBuffer);
  void copyGrayscaleLsbBuffers(const uint8_t* lsbBuffer);
  void copyGrayscaleMsbBuffers(const uint8_t* msbBuffer);
#ifdef EINK_DISPLAY_SINGLE_BUFFER_MODE
  void cleanupGrayscaleBuffers(const uint8_t* bwBuffer);
#endif
  void displayGrayBuffer(bool turnOffScreen = false);
  void grayscaleRevert();

  void displayBuffer(RefreshMode mode = FAST_REFRESH, bool turnOffScreen = false);

  // Non-blocking refresh API (used by the editor for responsive typing)
  void beginRefresh(RefreshMode mode = FAST_REFRESH, bool turnOffScreen = false);
  bool isRefreshing() const { return _refreshState != IDLE; }
  bool pollRefresh();
  void displayWindow(uint16_t x, uint16_t y, uint16_t w, uint16_t h, bool turnOffScreen = false);

  void refreshDisplay(RefreshMode mode = FAST_REFRESH, bool turnOffScreen = false);

  void setCustomLUT(bool enabled, const unsigned char* lutData = nullptr);

  void deepSleep();

  uint8_t* getFrameBuffer() const { return frameBuffer; }

  void saveFrameBufferAsPBM(const char* filename);

 private:
  int8_t _sclk, _mosi, _cs, _dc, _rst, _busy;

  uint8_t frameBuffer0[BUFFER_SIZE];
  uint8_t* frameBuffer;
#ifndef EINK_DISPLAY_SINGLE_BUFFER_MODE
  uint8_t frameBuffer1[BUFFER_SIZE];
  uint8_t* frameBufferActive;
#endif

  SPISettings spiSettings;

  bool isScreenOn;
  bool customLutActive;
  bool inGrayscaleMode;
  bool drawGrayscale;

  // UC8253 differential state. oldPlaneValid tracks whether DTM1 holds a real
  // previous frame; until it does, a fast diff would smear against garbage.
  bool oldPlaneValid = false;
  uint8_t initialFullSyncsRemaining = 1;

  enum RefreshState { IDLE, REFRESHING };
  RefreshState _refreshState = IDLE;
  RefreshMode _pendingMode = FAST_REFRESH;
  bool _pendingTurnOff = false;
  bool _pendingFullSync = false;
  unsigned long _refreshStartMs = 0;

  void resetDisplay();
  void sendCommand(uint8_t command);
  void sendData(uint8_t data);
  void sendData(const uint8_t* data, uint16_t length);
  void sendDataProgmem(const uint8_t* data, uint16_t length);
  void waitWhileBusy(const char* comment = nullptr);
  void initDisplayController();

  // UC8253 plane helpers
  void sendPlaneFlipped(uint8_t ramCmd, const uint8_t* plane);
  void fillPlane(uint8_t ramCmd, uint8_t fillByte);
  void loadLutBank(uint8_t bank);
  void loadLutBankCdi(uint8_t cdi0, uint8_t cdi1, uint8_t bank);
  void powerOnIfNeeded();
  void syncOldPlane(const uint8_t* fb);
};
