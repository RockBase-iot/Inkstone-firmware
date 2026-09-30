// Copyright (c) 2026 RockBase-IoT, Chengdu Rockbase Co., Ltd. All rights reserved.

#include "display/display_service.h"
#include "display/frame_store.h"
#include "boards/board.h"

#include <SPI.h>

// ─── Driver selection (per board) ───────────────────────────────────────────
#if defined(BOARD_NM_EPD_420_4C)
  #include <GxEPD2_4C.h>
  #include <epd4c/GxEPD2_420c_GDEY0420F51.h>
  using EpdDriver = GxEPD2_420c_GDEY0420F51;
  // Full-height page buffer (factory tuning: reduces paged-transfer overhead)
  static GxEPD2_4C<EpdDriver, EpdDriver::HEIGHT>
      s_display(EpdDriver(PIN_EPD_CS, PIN_EPD_DC, PIN_EPD_RST, PIN_EPD_BUSY));

  static void epdInit() {
      // Official demo parameters: initial=true, reset=2ms, pulldown=false
      s_display.init(115200, true, 2, false);
  }
  static void epdDrawFrame(const uint8_t* frame) {
      s_display.epd2.writeNative(frame, nullptr, 0, 0, EPD_WIDTH, EPD_HEIGHT);
      s_display.refresh(false);
  }
  static void epdPowerOff() { s_display.powerOff(); }

#elif defined(BOARD_ESP32_C5_SPECTRA_E6)
  #include "drivers/epd1200x1600_e6.h"
  // Dual-controller panel, no GxEPD2_GFX wrapper needed: frames arrive
  // pre-quantized (EPD_FRAME_BYTES) and go straight to the native path.
  static Epd1200x1600E6 s_epd(PIN_EPD_CS, PIN_EPD_CS_S, PIN_EPD_DC,
                              PIN_EPD_RST, PIN_EPD_BUSY);

  static void epdInit() { s_epd.init(115200, true, 10, false); }
  static void epdDrawFrame(const uint8_t* frame) {
      // Zero-copy: the driver streams s_pending directly (960KB; an internal
      // copy would not fit the C5's 4MB PSRAM alongside the service buffers).
      s_epd.drawFrameExternal(frame);
  }
  static void epdPowerOff() { s_epd.powerOff(); }

#else
  #error "No EPD driver mapped for this board. Extend display_service.cpp."
#endif

namespace display {

static uint8_t*   s_frame     = nullptr;   // current frame (source of /preview.raw)
static uint8_t*   s_pending   = nullptr;   // staging frame pending refresh
static volatile State s_state = State::IDLE;
static uint32_t   s_lastEpoch      = 0;
static uint32_t   s_lastMillis     = 0;
static uint32_t   s_lastUserMillis = 0;    // rate limiting: user-requested refreshes only
static bool       s_fromUser       = true; // whether the running refresh was user-requested
static bool       s_initialized    = false;
static TaskHandle_t s_task     = nullptr;

static void refreshTask(void*) {
    // Write full frame -> full refresh (25-30 s on 4C, ~30-45 s on E6)
    // -> cut drive voltage
    epdDrawFrame(s_pending);
    epdPowerOff();

    memcpy(s_frame, s_pending, EPD_FRAME_BYTES);
    s_lastEpoch  = (uint32_t)time(nullptr);
    s_lastMillis = millis();
    if (s_fromUser) {
        s_lastUserMillis = s_lastMillis;
        framestore::save(s_frame);   // persist user frames for cold-boot restore
    }
    s_state = State::IDLE;
    s_task  = nullptr;
    vTaskDelete(nullptr);
}

bool begin() {
    s_frame   = (uint8_t*)ps_malloc(EPD_FRAME_BYTES);
    s_pending = (uint8_t*)ps_malloc(EPD_FRAME_BYTES);
    if (!s_frame || !s_pending) {
        Serial.println("[display] PSRAM alloc failed");
        return false;
    }
    memset(s_frame, EPD_BYTE_WHITE, EPD_FRAME_BYTES);   // all white

    // EPD owns the SPI bus exclusively; MISO is panel NC, pass -1 to keep the GPIO free
    SPI.end();
    SPI.begin(PIN_EPD_SCK, -1, PIN_EPD_MOSI, PIN_EPD_CS);

    epdInit();
    s_initialized = true;
    return true;
}

// Start the refresh task on the contents of s_pending.
static bool startRefresh(bool fromUser) {
    s_fromUser = fromUser;
    s_state = State::REFRESHING;
    BaseType_t ok = xTaskCreatePinnedToCore(
        refreshTask, "epd_refresh", 8192, nullptr, 1, &s_task, 0);
    if (ok != pdPASS) {          // roll back state if task creation fails
        s_state = State::IDLE;
        return false;
    }
    return true;
}

bool submitFrame(const uint8_t* frame, size_t len) {
    if (len != EPD_FRAME_BYTES || s_state == State::REFRESHING) return false;
    memcpy(s_pending, frame, EPD_FRAME_BYTES);
    return startRefresh(true);
}

uint8_t* stagingBuffer() {
    return (s_state == State::IDLE) ? s_pending : nullptr;
}

bool submitStagedFrame() {
    if (s_state == State::REFRESHING) return false;
    return startRefresh(true);
}

// Start a non-user refresh from s_frame (test pattern / NVS restore).
static void startNonUserRefresh() {
    memcpy(s_pending, s_frame, EPD_FRAME_BYTES);
    startRefresh(false);
}

void showTestPattern() {
    if (s_state == State::REFRESHING) return;
    const int rowBytes = EPD_FRAME_BYTES / EPD_HEIGHT;
#if EPD_COLORS == 6
    // Six horizontal stripes in wire index order:
    // black(0) white(1) green(2) blue(3) red(4) yellow(5), 4bpp -> byte cc
    static const uint8_t colorRow[6] = {0x00, 0x11, 0x22, 0x33, 0x44, 0x55};
    for (int y = 0; y < EPD_HEIGHT; ++y) {
        memset(s_frame + y * rowBytes, colorRow[(y * 6) / EPD_HEIGHT], rowBytes);
    }
#else
    // Four quadrant stripes: black/white/yellow/red, 75 rows each
    static const uint8_t colorRow[4] = {0x00, 0x55, 0xAA, 0xFF};
    for (int y = 0; y < EPD_HEIGHT; ++y) {
        memset(s_frame + y * rowBytes, colorRow[(y * 4) / EPD_HEIGHT], rowBytes);
    }
#endif
    startNonUserRefresh();
}

void showSavedFrame(const uint8_t* frame) {
    if (s_state == State::REFRESHING) return;
    memcpy(s_frame, frame, EPD_FRAME_BYTES);
    startNonUserRefresh();
}

State    state()                 { return s_state; }
bool     isBusy()                { return s_state == State::REFRESHING; }
bool     isInitialized()         { return s_initialized; }
uint32_t lastRefreshEpoch()      { return s_lastEpoch; }
uint32_t lastRefreshMillis()     { return s_lastMillis; }
uint32_t lastUserRefreshMillis() { return s_lastUserMillis; }
const uint8_t* currentFrame()    { return s_frame; }

void powerOff() {
    if (s_initialized && s_state == State::IDLE) epdPowerOff();
}

} // namespace display
