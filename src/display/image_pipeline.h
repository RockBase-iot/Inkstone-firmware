// Copyright (c) 2026 RockBase-IoT, Chengdu Rockbase Co., Ltd. All rights reserved.

#pragma once
// ============================================================================
// image_pipeline — on-device image decode + quantize + 2bpp packing
// (the image channel of the Phase 2 API)
//
// Pipeline: JPEG/PNG bytes -> decode to RGB565 (PSRAM) -> center-crop to 4:3
//           -> scale to EPD_WIDTH x EPD_HEIGHT -> 4-color quantize
//           (Floyd-Steinberg by default) -> pack.
//
// The quantization algorithm is byte-identical across tools/convert_image.py,
// web/uploader.html, and this file:
//   Palette (packing RGB): black(0,0,0) white(255,255,255) yellow(255,210,0) red(200,30,30)
//   Distance: integer weighted Euclidean d = 299*dr^2 + 587*dg^2 + 114*db^2
//   Dither: serpentine scan, error accumulated as 1/16 integers,
//           weights right=7, down-left=3, down=5, down-right=1;
//           pixel value = floor((v*16 + err + 8) / 16), clamped to [0,255]
//   Packing: byte = c0<<6 | c1<<4 | c2<<2 | c3, row-major
// ============================================================================

#include <Arduino.h>

namespace imagepipe {

// Decode JPEG/PNG and convert into an EPD_FRAME_BYTES 2bpp frame at out.
// dither=true (default) applies Floyd-Steinberg; false is plain nearest-color.
// Returns true on success; failures are logged to serial.
bool convertJpeg(const uint8_t* data, size_t len, uint8_t* out, bool dither = true);
bool convertPng (const uint8_t* data, size_t len, uint8_t* out, bool dither = true);

// RGB565 full image -> center-crop 4:3 -> scale -> quantize + pack
// (shared by the decoders and tests).
bool composeFrame(const uint16_t* rgb565, int srcW, int srcH,
                  uint8_t* out, bool dither);

} // namespace imagepipe
