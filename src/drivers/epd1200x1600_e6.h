// Copyright (c) 2026 RockBase-IoT, Chengdu Rockbase Co., Ltd. All rights reserved.
// Driver ported from RockBase-iot/esp32-spectra-e6 (src/Epd1200x1600E6.*),
// extended with drawFrameExternal() so display_service can hand over its
// PSRAM staging frame without a second 960KB copy.

#pragma once

#include <GxEPD2_EPD.h>

class Epd1200x1600E6 : public GxEPD2_EPD {
 public:
  static const uint16_t WIDTH = 1200;
  static const uint16_t WIDTH_VISIBLE = WIDTH;
  static const uint16_t HEIGHT = 1600;
  static const GxEPD2::Panel panel = GxEPD2::GDEY073D46;
  static const bool hasColor = true;
  static const bool hasPartialUpdate = false;
  static const bool hasFastPartialUpdate = false;

  Epd1200x1600E6(int16_t csMaster, int16_t csSlave, int16_t dc, int16_t rst, int16_t busy);
  ~Epd1200x1600E6();

  void init(uint32_t serialDiagBitrate = 0) override;
  void init(uint32_t serialDiagBitrate, bool initial, uint16_t resetDuration = 10,
            bool pulldownRstMode = false) override;
  void clearScreen(uint8_t value = 0xFF) override;
  void writeScreenBuffer(uint8_t value = 0xFF) override;
  void writeImage(const uint8_t bitmap[], int16_t x, int16_t y, int16_t w, int16_t h, bool invert = false,
                  bool mirrorY = false, bool pgm = false) override;
  void writeImagePart(const uint8_t bitmap[], int16_t xPart, int16_t yPart, int16_t wBitmap, int16_t hBitmap, int16_t x,
                      int16_t y, int16_t w, int16_t h, bool invert = false, bool mirrorY = false,
                      bool pgm = false) override;
  void writeImage(const uint8_t* black, const uint8_t* color, int16_t x, int16_t y, int16_t w, int16_t h,
                  bool invert = false, bool mirrorY = false, bool pgm = false);
  void writeImagePart(const uint8_t* black, const uint8_t* color, int16_t xPart, int16_t yPart, int16_t wBitmap,
                      int16_t hBitmap, int16_t x, int16_t y, int16_t w, int16_t h, bool invert = false,
                      bool mirrorY = false, bool pgm = false);
  void writeNative(const uint8_t* data1, const uint8_t* data2, int16_t x, int16_t y, int16_t w, int16_t h,
                   bool invert = false, bool mirrorY = false, bool pgm = false);
  void writeNativePart(const uint8_t* data1, const uint8_t* data2, int16_t xPart, int16_t yPart, int16_t wBitmap,
                       int16_t hBitmap, int16_t x, int16_t y, int16_t w, int16_t h, bool invert = false,
                       bool mirrorY = false, bool pgm = false);
  void drawImage(const uint8_t bitmap[], int16_t x, int16_t y, int16_t w, int16_t h, bool invert = false,
                 bool mirrorY = false, bool pgm = false);
  void drawImagePart(const uint8_t bitmap[], int16_t xPart, int16_t yPart, int16_t wBitmap, int16_t hBitmap, int16_t x,
                     int16_t y, int16_t w, int16_t h, bool invert = false, bool mirrorY = false, bool pgm = false);
  void drawImage(const uint8_t* black, const uint8_t* color, int16_t x, int16_t y, int16_t w, int16_t h,
                 bool invert = false, bool mirrorY = false, bool pgm = false);
  void drawImagePart(const uint8_t* black, const uint8_t* color, int16_t xPart, int16_t yPart, int16_t wBitmap,
                     int16_t hBitmap, int16_t x, int16_t y, int16_t w, int16_t h, bool invert = false,
                     bool mirrorY = false, bool pgm = false);
  void drawNative(const uint8_t* data1, const uint8_t* data2, int16_t x, int16_t y, int16_t w, int16_t h,
                  bool invert = false, bool mirrorY = false, bool pgm = false);
  void drawNativePart(const uint8_t* data1, const uint8_t* data2, int16_t xPart, int16_t yPart, int16_t wBitmap,
                      int16_t hBitmap, int16_t x, int16_t y, int16_t w, int16_t h, bool invert = false,
                      bool mirrorY = false, bool pgm = false);

  // Inkstone addition: render a caller-owned full 1200x1600 4bpp frame
  // (EPD_FRAME_BYTES) without copying it into the driver's own PSRAM buffer.
  // The caller must keep the buffer valid until refresh() completes; the
  // driver never frees it.
  void drawFrameExternal(const uint8_t* frame);

  void refresh(bool partialUpdateMode = false) override;
  void refresh(int16_t x, int16_t y, int16_t w, int16_t h) override;
  void powerOff() override;
  void hibernate() override;

 private:
  void initializeDisplay();
  void resetPanel();
  void writeCommandData(int16_t chipSelect, uint8_t command, const uint8_t* data, size_t length);
  void writeFrame();
  void turnOnDisplay();
  void writeWhiteBuffer(uint8_t color);
  void freeFrameBuffer();
  static uint8_t convertByte(uint8_t value);

  int16_t csSlave;
  uint8_t* frameBuffer;
  const uint8_t* externalFrame;   // Inkstone: zero-copy full-frame path
  bool frameComplete;
};
