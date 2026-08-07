#include <HalDisplay.h>
#include <HalGPIO.h>

#ifdef CROSSPOINT_MURPHY_M4
// M4 pins come from BoardConfig::ACTIVE; ctor values are legacy and unused by FreeInkDisplay.
HalDisplay::HalDisplay() : einkDisplay(-1, -1, -1, -1, -1, -1) {}
#else
#define SD_SPI_MISO 7

HalDisplay::HalDisplay() : einkDisplay(EPD_SCLK, EPD_MOSI, EPD_CS, EPD_DC, EPD_RST, EPD_BUSY) {}
#endif

HalDisplay::~HalDisplay() {}

void HalDisplay::begin() {
#ifdef CROSSPOINT_X3
  einkDisplay.setDisplayX3();
#endif
  Serial.printf("[%lu] [M4-DISP] begin() panel=%ux%u buffer=%lu\n", millis(),
                static_cast<unsigned>(DISPLAY_WIDTH), static_cast<unsigned>(DISPLAY_HEIGHT),
                static_cast<unsigned long>(BUFFER_SIZE));
  einkDisplay.begin();
  if (getFrameBuffer() == nullptr) {
    Serial.printf("[%lu] [M4-DISP] ERROR: framebuffer allocation failed\n", millis());
  } else {
    Serial.printf("[%lu] [M4-DISP] framebuffer ready\n", millis());
  }
}

void HalDisplay::clearScreen(uint8_t color) const { einkDisplay.clearScreen(color); }

void HalDisplay::drawImage(const uint8_t* imageData, uint16_t x, uint16_t y, uint16_t w, uint16_t h,
                           bool fromProgmem) const {
  einkDisplay.drawImage(imageData, x, y, w, h, fromProgmem);
}

void HalDisplay::drawImageTransparent(const uint8_t* imageData, uint16_t x, uint16_t y, uint16_t w, uint16_t h,
                                      bool fromProgmem) const {
  einkDisplay.drawImageTransparent(imageData, x, y, w, h, fromProgmem);
}

EInkDisplay::RefreshMode convertRefreshMode(HalDisplay::RefreshMode mode) {
  switch (mode) {
    case HalDisplay::FULL_REFRESH:
      return EInkDisplay::FULL_REFRESH;
    case HalDisplay::HALF_REFRESH:
      return EInkDisplay::HALF_REFRESH;
    case HalDisplay::UI_FAST_REFRESH:
      // Murphy SSD1677 has no separate UI-fast LUT; map to FAST (DU-class) which is
      // the proven low-latency path. Never silently no-op UI refreshes.
      return EInkDisplay::FAST_REFRESH;
    case HalDisplay::FAST_REFRESH:
    default:
      return EInkDisplay::FAST_REFRESH;
  }
}

void HalDisplay::displayBuffer(HalDisplay::RefreshMode mode, bool turnOffScreen) {
  einkDisplay.displayBuffer(convertRefreshMode(mode), turnOffScreen);
}

void HalDisplay::refreshDisplay(HalDisplay::RefreshMode mode, bool turnOffScreen) {
  einkDisplay.refreshDisplay(convertRefreshMode(mode), turnOffScreen);
}

void HalDisplay::deepSleep() { einkDisplay.deepSleep(); }

uint8_t* HalDisplay::getFrameBuffer() const { return einkDisplay.getFrameBuffer(); }

void HalDisplay::copyGrayscaleBuffers(const uint8_t* lsbBuffer, const uint8_t* msbBuffer) {
  einkDisplay.copyGrayscaleBuffers(lsbBuffer, msbBuffer);
}

void HalDisplay::copyGrayscaleLsbBuffers(const uint8_t* lsbBuffer) { einkDisplay.copyGrayscaleLsbBuffers(lsbBuffer); }

void HalDisplay::copyGrayscaleMsbBuffers(const uint8_t* msbBuffer) { einkDisplay.copyGrayscaleMsbBuffers(msbBuffer); }

void HalDisplay::cleanupGrayscaleBuffers(const uint8_t* bwBuffer) { einkDisplay.cleanupGrayscaleBuffers(bwBuffer); }

void HalDisplay::displayGrayBuffer(bool turnOffScreen) { einkDisplay.displayGrayBuffer(turnOffScreen); }

uint32_t HalDisplay::waveformLabRefresh(const uint8_t* prev, const uint8_t* next, const uint8_t* lut,
                                        bool turnOff) {
  return einkDisplay.waveformLabRefresh(prev, next, lut, turnOff);
}

void HalDisplay::waveformLabBaseline(const uint8_t* frame) {
  einkDisplay.waveformLabBaseline(frame);
}

uint32_t HalDisplay::waveformLabRefreshWindow(const uint8_t* prev, const uint8_t* next, const uint8_t* lut,
                                              uint16_t x, uint16_t y, uint16_t w, uint16_t h,
                                              bool syncAfter) {
  return einkDisplay.waveformLabRefreshWindow(prev, next, lut, x, y, w, h, syncAfter);
}

uint32_t HalDisplay::waveformLabRefreshWindowBufs(const uint8_t* redWin, const uint8_t* bwWin,
                                                  const uint8_t* lut, uint16_t x, uint16_t y, uint16_t w,
                                                  uint16_t h) {
  return einkDisplay.waveformLabRefreshWindowBufs(redWin, bwWin, lut, x, y, w, h);
}

void HalDisplay::waveformLabWriteDiffWindow(const uint8_t* prev, const uint8_t* next, uint16_t x, uint16_t y,
                                            uint16_t w, uint16_t h) {
  einkDisplay.waveformLabWriteDiffWindow(prev, next, x, y, w, h);
}

uint32_t HalDisplay::waveformLabActivate(const uint8_t* lut) {
  return einkDisplay.waveformLabActivate(lut);
}

uint32_t HalDisplay::waveformLabActivateWindow(uint16_t x, uint16_t y, uint16_t w, uint16_t h,
                                               const uint8_t* lut) {
  return einkDisplay.waveformLabActivateWindow(x, y, w, h, lut);
}

void HalDisplay::waveformLabEqualizeWindow(const uint8_t* frame, uint16_t x, uint16_t y, uint16_t w, uint16_t h) {
  einkDisplay.waveformLabEqualizeWindow(frame, x, y, w, h);
}

void HalDisplay::setCustomLUT(bool enabled, const unsigned char* lutData) {
  einkDisplay.setCustomLUT(enabled, lutData);
}
