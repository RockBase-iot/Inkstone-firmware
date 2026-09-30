// Copyright (c) 2026 RockBase-IoT, Chengdu Rockbase Co., Ltd. All rights reserved.
// Driver ported from RockBase-iot/esp32-spectra-e6 (src/Epd1200x1600E6.cpp).
// Init command sequence and panel-native color translation are unchanged.

#include "drivers/epd1200x1600_e6.h"

namespace {
constexpr uint16_t kControllerWidthBytes = Epd1200x1600E6::WIDTH / 4;
constexpr uint16_t kPanelWidthBytes = Epd1200x1600E6::WIDTH / 2;
constexpr size_t kFrameBufferSize = size_t(kPanelWidthBytes) * Epd1200x1600E6::HEIGHT;

constexpr uint8_t kAnalogTiming[] = {0xC0, 0x1E, 0x1E, 0xCE, 0xCE, 0xCE, 0x15, 0x15, 0x55};
constexpr uint8_t kCommandF0[] = {0x49, 0x55, 0x13, 0x5D, 0x05, 0x10};
constexpr uint8_t kPanelSetting[] = {0xDF, 0x69};
constexpr uint8_t kDataInterval[] = {0xF7};
constexpr uint8_t kTimingControl[] = {0x03, 0x03};
constexpr uint8_t kAgid[] = {0x10};
constexpr uint8_t kPowerSaving[] = {0x22};
constexpr uint8_t kColorSetting[] = {0x01};
constexpr uint8_t kResolution[] = {0x04, 0xB0, 0x03, 0x20};
constexpr uint8_t kPowerSetting[] = {0x0F, 0x00, 0x28, 0x2C, 0x28, 0x38};
constexpr uint8_t kEnableBuffer[] = {0x07};
constexpr uint8_t kBoostPositive[] = {0xE8, 0x28};
constexpr uint8_t kEnablePositive[] = {0x01};
constexpr uint8_t kBoostNegative[] = {0xE8, 0x28};
constexpr uint8_t kEnableNegative[] = {0x01};
constexpr uint8_t kTftVcomPower[] = {0x02};
constexpr uint8_t kDisplayRefresh[] = {0x01};
constexpr uint8_t kPowerOff[] = {0x00};
constexpr uint8_t kDeepSleep[] = {0xA5};
}  // namespace

Epd1200x1600E6::Epd1200x1600E6(int16_t csMaster, int16_t csSlave, int16_t dc, int16_t rst, int16_t busy)
    : GxEPD2_EPD(csMaster, dc, rst, busy, LOW, 120000000, WIDTH, HEIGHT, panel, hasColor, hasPartialUpdate,
                 hasFastPartialUpdate),
      csSlave(csSlave),
      frameBuffer(nullptr),
      externalFrame(nullptr),
      frameComplete(false) {}

Epd1200x1600E6::~Epd1200x1600E6() { freeFrameBuffer(); }

void Epd1200x1600E6::init(uint32_t serialDiagBitrate) { init(serialDiagBitrate, true); }

void Epd1200x1600E6::init(uint32_t serialDiagBitrate, bool initial, uint16_t resetDuration, bool pulldownRstMode) {
  freeFrameBuffer();
  _initial_write = initial;
  _initial_refresh = initial;
  _pulldown_rst_mode = pulldownRstMode;
  _power_is_on = false;
  _using_partial_mode = false;
  _hibernating = false;
  _init_display_done = false;
  _reset_duration = resetDuration;
  if (serialDiagBitrate > 0) {
    if (!Serial) Serial.begin(serialDiagBitrate);
    _diag_enabled = true;
  }

  pinMode(_cs, OUTPUT);
  pinMode(csSlave, OUTPUT);
  pinMode(_dc, OUTPUT);
  pinMode(_rst, OUTPUT);
  pinMode(_busy, INPUT);
  digitalWrite(_cs, HIGH);
  digitalWrite(csSlave, HIGH);
  digitalWrite(_dc, HIGH);
  digitalWrite(_rst, HIGH);
}

void Epd1200x1600E6::clearScreen(uint8_t value) {
  writeWhiteBuffer(value);
  refresh();
}

void Epd1200x1600E6::writeScreenBuffer(uint8_t value) { writeWhiteBuffer(value); }

void Epd1200x1600E6::writeImage(const uint8_t[], int16_t, int16_t, int16_t, int16_t, bool, bool, bool) {}

void Epd1200x1600E6::writeImagePart(const uint8_t[], int16_t, int16_t, int16_t, int16_t, int16_t, int16_t, int16_t,
                                    int16_t, bool, bool, bool) {}

void Epd1200x1600E6::writeImage(const uint8_t* black, const uint8_t*, int16_t x, int16_t y, int16_t w, int16_t h,
                                bool invert, bool mirrorY, bool pgm) {
  if (black) writeImage(black, x, y, w, h, invert, mirrorY, pgm);
}

void Epd1200x1600E6::writeImagePart(const uint8_t* black, const uint8_t*, int16_t xPart, int16_t yPart, int16_t wBitmap,
                                    int16_t hBitmap, int16_t x, int16_t y, int16_t w, int16_t h, bool invert,
                                    bool mirrorY, bool pgm) {
  if (black) writeImagePart(black, xPart, yPart, wBitmap, hBitmap, x, y, w, h, invert, mirrorY, pgm);
}

void Epd1200x1600E6::writeNative(const uint8_t* data1, const uint8_t*, int16_t x, int16_t y, int16_t w, int16_t h, bool,
                                 bool, bool) {
  if (!data1 || x != 0 || w != WIDTH || y < 0 || h <= 0 || y + h > HEIGHT) return;
  if (!frameBuffer) {
    frameBuffer = static_cast<uint8_t*>(ps_malloc(kFrameBufferSize));
    if (!frameBuffer) {
      Serial.println("Failed to allocate PSRAM for GDEB0709E01 frame buffer");
      return;
    }
  }
  if (y == 0) frameComplete = false;
  memcpy(frameBuffer + size_t(y) * kPanelWidthBytes, data1, size_t(h) * kPanelWidthBytes);
  frameComplete = y + h == HEIGHT;
}

void Epd1200x1600E6::writeNativePart(const uint8_t* data1, const uint8_t* data2, int16_t xPart, int16_t yPart,
                                     int16_t wBitmap, int16_t, int16_t x, int16_t y, int16_t w, int16_t h, bool invert,
                                     bool mirrorY, bool pgm) {
  if (xPart != 0 || wBitmap != WIDTH || x != 0 || w != WIDTH || yPart < 0) return;
  writeNative(data1 + uint32_t(yPart) * kPanelWidthBytes, data2, x, y, w, h, invert, mirrorY, pgm);
}

void Epd1200x1600E6::drawImage(const uint8_t bitmap[], int16_t x, int16_t y, int16_t w, int16_t h, bool invert,
                               bool mirrorY, bool pgm) {
  writeImage(bitmap, x, y, w, h, invert, mirrorY, pgm);
  refresh();
}

void Epd1200x1600E6::drawImagePart(const uint8_t bitmap[], int16_t xPart, int16_t yPart, int16_t wBitmap,
                                   int16_t hBitmap, int16_t x, int16_t y, int16_t w, int16_t h, bool invert,
                                   bool mirrorY, bool pgm) {
  writeImagePart(bitmap, xPart, yPart, wBitmap, hBitmap, x, y, w, h, invert, mirrorY, pgm);
  refresh();
}

void Epd1200x1600E6::drawImage(const uint8_t* black, const uint8_t* color, int16_t x, int16_t y, int16_t w, int16_t h,
                               bool invert, bool mirrorY, bool pgm) {
  writeImage(black, color, x, y, w, h, invert, mirrorY, pgm);
  refresh();
}

void Epd1200x1600E6::drawImagePart(const uint8_t* black, const uint8_t* color, int16_t xPart, int16_t yPart,
                                   int16_t wBitmap, int16_t hBitmap, int16_t x, int16_t y, int16_t w, int16_t h,
                                   bool invert, bool mirrorY, bool pgm) {
  writeImagePart(black, color, xPart, yPart, wBitmap, hBitmap, x, y, w, h, invert, mirrorY, pgm);
  refresh();
}

void Epd1200x1600E6::drawNative(const uint8_t* data1, const uint8_t* data2, int16_t x, int16_t y, int16_t w, int16_t h,
                                bool invert, bool mirrorY, bool pgm) {
  writeNative(data1, data2, x, y, w, h, invert, mirrorY, pgm);
  refresh();
}

void Epd1200x1600E6::drawFrameExternal(const uint8_t* frame) {
  if (!frame) return;
  freeFrameBuffer();
  externalFrame = frame;
  frameComplete = true;
  refresh();
}

void Epd1200x1600E6::refresh(bool) {
  if ((!frameBuffer && !externalFrame) || !frameComplete) return;
  const unsigned long refreshStartMs = millis();
  initializeDisplay();
  writeFrame();
  turnOnDisplay();
  const unsigned long refreshElapsedMs = millis() - refreshStartMs;
  Serial.printf("E6 full refresh completed in %lu ms (%lu.%03lu s), finished at %lu ms uptime\n", refreshElapsedMs,
                refreshElapsedMs / 1000, refreshElapsedMs % 1000, millis());
  _initial_write = false;
  freeFrameBuffer();
}

void Epd1200x1600E6::refresh(int16_t, int16_t, int16_t, int16_t) { refresh(false); }

void Epd1200x1600E6::powerOff() { _power_is_on = false; }

void Epd1200x1600E6::hibernate() {
  if (_hibernating) return;
  writeCommandData(-1, 0x07, kDeepSleep, sizeof(kDeepSleep));
  _hibernating = true;
  _init_display_done = false;
}

void Epd1200x1600E6::initializeDisplay() {
  if (_init_display_done) return;

  resetPanel();
  _waitWhileBusy("E6 reset");
  writeCommandData(_cs, 0x74, kAnalogTiming, sizeof(kAnalogTiming));
  writeCommandData(-1, 0xF0, kCommandF0, sizeof(kCommandF0));
  writeCommandData(-1, 0x00, kPanelSetting, sizeof(kPanelSetting));
  writeCommandData(-1, 0x50, kDataInterval, sizeof(kDataInterval));
  writeCommandData(-1, 0x60, kTimingControl, sizeof(kTimingControl));
  writeCommandData(-1, 0x86, kAgid, sizeof(kAgid));
  writeCommandData(-1, 0xE3, kPowerSaving, sizeof(kPowerSaving));
  writeCommandData(-1, 0xE0, kColorSetting, sizeof(kColorSetting));
  writeCommandData(-1, 0x61, kResolution, sizeof(kResolution));
  writeCommandData(_cs, 0x01, kPowerSetting, sizeof(kPowerSetting));
  writeCommandData(_cs, 0xB6, kEnableBuffer, sizeof(kEnableBuffer));
  writeCommandData(_cs, 0x06, kBoostPositive, sizeof(kBoostPositive));
  writeCommandData(_cs, 0xB7, kEnablePositive, sizeof(kEnablePositive));
  writeCommandData(_cs, 0x05, kBoostNegative, sizeof(kBoostNegative));
  writeCommandData(_cs, 0xB0, kEnableNegative, sizeof(kEnableNegative));
  writeCommandData(_cs, 0xB1, kTftVcomPower, sizeof(kTftVcomPower));
  _init_display_done = true;
  _hibernating = false;
}

void Epd1200x1600E6::resetPanel() {
  digitalWrite(_rst, LOW);
  delay(20);
  digitalWrite(_rst, HIGH);
  delay(20);
}

void Epd1200x1600E6::writeCommandData(int16_t chipSelect, uint8_t command, const uint8_t* data, size_t length) {
  _pSPIx->beginTransaction(_spi_settings);
  if (chipSelect < 0) {
    digitalWrite(_cs, LOW);
    digitalWrite(csSlave, LOW);
  } else {
    digitalWrite(chipSelect, LOW);
  }
  _pSPIx->transfer(command);
  for (size_t index = 0; index < length; index++) _pSPIx->transfer(data[index]);
  digitalWrite(_cs, HIGH);
  digitalWrite(csSlave, HIGH);
  _pSPIx->endTransaction();
}

void Epd1200x1600E6::writeFrame() {
  const uint8_t* source = externalFrame ? externalFrame : frameBuffer;
  for (int16_t chipSelect : {_cs, csSlave}) {
    const uint16_t offset = chipSelect == _cs ? 0 : kControllerWidthBytes;
    _pSPIx->beginTransaction(_spi_settings);
    digitalWrite(chipSelect, LOW);
    _pSPIx->transfer(0x10);
    for (uint16_t row = 0; row < HEIGHT; row++) {
      const uint8_t* rowData = source + size_t(row) * kPanelWidthBytes + offset;
      for (uint16_t column = 0; column < kControllerWidthBytes; column++) _pSPIx->transfer(convertByte(rowData[column]));
      delay(1);
    }
    digitalWrite(chipSelect, HIGH);
    _pSPIx->endTransaction();
  }
}

void Epd1200x1600E6::turnOnDisplay() {
  writeCommandData(-1, 0x04, nullptr, 0);
  _waitWhileBusy("E6 power on");
  delay(30);
  writeCommandData(-1, 0x12, kDisplayRefresh, sizeof(kDisplayRefresh));
  _waitWhileBusy("E6 refresh");
  writeCommandData(-1, 0x02, kPowerOff, sizeof(kPowerOff));
  _waitWhileBusy("E6 power off");
}

void Epd1200x1600E6::writeWhiteBuffer(uint8_t color) {
  if (!frameBuffer) {
    frameBuffer = static_cast<uint8_t*>(ps_malloc(kFrameBufferSize));
    if (!frameBuffer) {
      Serial.println("Failed to allocate PSRAM for GDEB0709E01 frame buffer");
      return;
    }
  }
  const uint8_t converted = convertByte((color == 0xFF ? 0x11 : color));
  memset(frameBuffer, converted, kFrameBufferSize);
  frameComplete = true;
}

void Epd1200x1600E6::freeFrameBuffer() {
  if (frameBuffer) free(frameBuffer);
  frameBuffer = nullptr;
  externalFrame = nullptr;
  frameComplete = false;
}

// GxEPD2 7-color index -> GDEB0709E01 panel-native code.
// Indices: 0 black, 1 white, 2 green, 3 blue, 4 red, 5 yellow, 6 orange.
// Panel codes: 0 black, 1 white, 2 yellow, 3 red, 5 blue, 6 green.
uint8_t Epd1200x1600E6::convertByte(uint8_t value) {
  const auto convertColor = [](uint8_t color) {
    switch (color) {
      case 0x02:   // green
        return uint8_t{0x06};
      case 0x03:   // blue
        return uint8_t{0x05};
      case 0x04:   // red
        return uint8_t{0x03};
      case 0x05:   // yellow
      case 0x06:   // orange -> yellow (not present on Spectra 6)
        return uint8_t{0x02};
      default:     // black, white pass through
        return color;
    }
  };
  return static_cast<uint8_t>((convertColor(value >> 4) << 4) | convertColor(value & 0x0F));
}
