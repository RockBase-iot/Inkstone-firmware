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
  #error "board header must define EPD_COLORS (4 = BWRY 2bpp)"
#endif
#ifndef EPD_FRAME_BYTES
  #error "board header must define EPD_FRAME_BYTES"
#endif

// The quantize/pack/direct-write pipeline currently implements only 4-color
// 2bpp (pixel format facts, plan section 3). A new panel with the same
// 4-color 2bpp MSB-first line format can reuse it directly; other formats
// require extending image_pipeline and display_service.
#if EPD_COLORS != 4
  #error "Only 4-color (2bpp BWRY) panels are implemented so far."
#endif
