// Copyright (c) 2026 RockBase-IoT, Chengdu Rockbase Co., Ltd. All rights reserved.

#pragma once
// ============================================================================
// display_service — board-agnostic e-paper display service
//
// Responsibilities:
//   - Select the GxEPD2 driver per board.h and init the EPD SPI bus (FSPI/SPI2)
//   - Manage PSRAM frame buffers (current frame + upload staging frame)
//   - Refresh in a dedicated FreeRTOS task: HTTP returns 202 immediately,
//     clients poll /status for busy
//   - powerOff() after refresh to cut panel drive voltage (prevents ghosting)
//
// Pixel format (plan section 3): 2 bits/px, 0=black 1=white 2=yellow 3=red,
// 4 pixels per byte MSB first, row-major, EPD_FRAME_BYTES per frame —
// bit-identical to the panel line format, so writeNative() can push it raw.
// ============================================================================

#include <Arduino.h>

namespace display {

enum class State : uint8_t { IDLE = 0, REFRESHING = 1 };

// Init driver and frame buffers (PSRAM). Call once, early in setup().
bool begin();

// True after begin() has initialized the panel driver successfully.
bool isInitialized();

// Submit a frame and start an asynchronous refresh. The frame is copied into
// an internal buffer, so the caller's buffer may be reused immediately.
// len must be == EPD_FRAME_BYTES; returns false while refreshing (caller
// should respond 409).
bool submitFrame(const uint8_t* frame, size_t len);

// Built-in 4-color stripe test pattern (T1 acceptance / factory self-check).
void showTestPattern();

// Display a frame restored from the NVS cache on cold boot. Counted as a
// non-user refresh (does not consume the rate-limit quota).
void showSavedFrame(const uint8_t* frame);

State       state();
bool        isBusy();
uint32_t    lastRefreshEpoch();      // Unix time of last refresh completion (0 = never)
uint32_t    lastRefreshMillis();     // millis() of last refresh completion (0 = never)
uint32_t    lastUserRefreshMillis(); // millis() of last user-requested refresh (rate
                                     // limiting); the boot self-check pattern is excluded

// Current frame pointer (for /preview.raw); length is always EPD_FRAME_BYTES.
const uint8_t* currentFrame();

// Cut panel drive voltage immediately (called by the pre-sleep sequence).
void powerOff();

} // namespace display
