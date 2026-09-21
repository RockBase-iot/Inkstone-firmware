// Copyright (c) 2026 RockBase-IoT, Chengdu Rockbase Co., Ltd. All rights reserved.

#pragma once
// ============================================================================
// retro_quantize — RETRO profile quantizer
//
// A second, style-oriented quantizer alongside the DEFAULT Floyd-Steinberg
// path. Characteristics:
//   - OKLab perceptual colour space for palette matching
//   - panel-calibrated warm palette (soft white 233,231,222 / brick red / ochre)
//   - hue-aware candidate masking: cool hues are dithered with black+white only
//   - stylized preprocessing (desaturation + S-curve dynamic compression)
//   - serpentine Atkinson error diffusion (6 x 1/8 -> 75% of error, finer grain)
//
// The internal palette order is black/white/RED/YELLOW. Before packing, RETRO
// maps red and yellow to the panel's fixed 2bpp codes: black/white/YELLOW/RED.
//
// This file is byte-identical with tools/retro_core.py (the Python reference)
// and web/uploader.html (the browser port). See docs/RETRO.md for the float32
// constraints that make that hold. The path is fully isolated from DEFAULT, so
// reverting the RETRO feature cannot change existing output.
// ============================================================================

#include <Arduino.h>
#include "boards/board.h"

namespace retro {

// Quantize an RGB888 buffer (EPD_WIDTH*EPD_HEIGHT*3) into an
// EPD_FRAME_BYTES 2bpp frame. `dither=false` falls back to plain
// nearest-colour in OKLab (no error diffusion) — the preprocessing still runs.
// Returns true on success; failures are logged to serial.
bool quantizePack(const uint8_t* rgb, uint8_t* out, bool dither = true);

// RGB565 source image -> center-crop 4:3 -> scale to panel size ->
// RETRO quantize -> 2bpp pack. Used by the JPEG/PNG decoders.
bool composeFrame(const uint16_t* src, int srcW, int srcH,
                  uint8_t* out, bool dither = true);

} // namespace retro
