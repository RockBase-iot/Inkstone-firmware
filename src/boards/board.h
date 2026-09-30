// Copyright (c) 2026 RockBase-IoT, Chengdu Rockbase Co., Ltd. All rights reserved.

#pragma once
// ============================================================================
// board.h — multi-board dispatch entry point
//
// A board header is selected via -DBOARD_<NAME>=1 in platformio.ini
// build_flags. Every board must define the full board contract (see the
// header comment of each board file and docs/PORTING.md). All other firmware
// code references only the macros exported here — never hardcode pins,
// resolutions, or frame sizes.
// ============================================================================

#if defined(BOARD_NM_EPD_420_4C)
  #include "boards/nm_epd_420_4c.h"
#elif defined(BOARD_ESP32_C5_SPECTRA_E6)
  #include "boards/esp32_c5_spectra_e6.h"
// Add future boards here:
// #elif defined(BOARD_NM_EPD_XXX_4C)
//   #include "boards/nm_epd_xxx_4c.h"
#else
  #error "No board selected. Add -DBOARD_<NAME>=1 to build_flags (see platformio.ini)."
#endif

// ─── Board contract check: any missing macro fails at compile time ──────────
#ifndef BOARD_NAME
  #error "board header must define BOARD_NAME"
#endif
#ifndef BOARD_MDNS_HOST
  #error "board header must define BOARD_MDNS_HOST"
#endif
#ifndef BOARD_AP_SSID_PREFIX
  #error "board header must define BOARD_AP_SSID_PREFIX"
#endif
#ifndef EPD_WIDTH
  #error "board header must define EPD_WIDTH"
#endif
#ifndef EPD_HEIGHT
  #error "board header must define EPD_HEIGHT"
#endif
#ifndef EPD_COLORS
  #error "board header must define EPD_COLORS (4 = BWRY 2bpp, 6 = Spectra E6 4bpp)"
#endif
#ifndef EPD_FRAME_BYTES
  #error "board header must define EPD_FRAME_BYTES"
#endif

// Byte filled with the panel's "white" pixel code (4C 2bpp: 0x55, E6 4bpp: 0x11).
#ifndef EPD_BYTE_WHITE
  #define EPD_BYTE_WHITE 0x55
#endif

// Two pixel formats are implemented:
//   EPD_COLORS == 4: 2bpp BWRY, byte = c0<<6|c1<<4|c2<<2|c3 (plan section 3)
//   EPD_COLORS == 6: 4bpp Spectra E6, byte = c0<<4|c1, nibble = GxEPD2 7-color
//                    index (0 black 1 white 2 green 3 blue 4 red 5 yellow)
// Any other panel requires extending image_pipeline and display_service.
#if EPD_COLORS != 4 && EPD_COLORS != 6
  #error "Only 4-color (2bpp BWRY) and 6-color (4bpp Spectra E6) panels are implemented."
#endif
