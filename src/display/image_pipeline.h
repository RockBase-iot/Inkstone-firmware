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

// Quantization profile. Values are the public `?profile=` tokens.
//
// The enumerators carry an IPP_ prefix because <Arduino.h> does
//     #define DEFAULT 1
// so a plain `DEFAULT` here is textually replaced by `1` and the enum fails to
// compile ("expected identifier before numeric constant"), taking every
// translation unit that includes this header down with it. Name them IPP_NONE /
// IPP_DEFAULT / IPP_RETRO; the wire tokens stay "none" / "default" / "retro" and
// are mapped in parseProfile().
enum class Profile : uint8_t {
    IPP_NONE    = 0,   // nearest colour, no diffusion
    IPP_DEFAULT = 1,   // existing luma-weighted Floyd-Steinberg (byte-stable)
    IPP_RETRO   = 2,   // OKLab + hue-aware masking + serpentine Atkinson
};

// Decode JPEG/PNG and convert into an EPD_FRAME_BYTES 2bpp frame at out.
// dither=true (default) applies Floyd-Steinberg; false is plain nearest-color.
// Returns true on success; failures are logged to serial.
bool convertJpeg(const uint8_t* data, size_t len, uint8_t* out, bool dither = true);
bool convertPng (const uint8_t* data, size_t len, uint8_t* out, bool dither = true);

// RGB565 full image -> center-crop 4:3 -> scale -> quantize + pack
// (shared by the decoders and tests).
bool composeFrame(const uint16_t* rgb565, int srcW, int srcH,
                  uint8_t* out, bool dither);

// ─── Profile-aware overloads ───────────────────────────────────────────────
// Added alongside the legacy bool form rather than replacing it, so existing
// call sites keep their exact behaviour. Profile::IPP_NONE/DEFAULT map onto
// dither=false/true; Profile::IPP_RETRO dispatches into the isolated retro_*
// module (see display/retro_quantize.h).
bool convertJpeg(const uint8_t* data, size_t len, uint8_t* out, Profile p);
bool convertPng (const uint8_t* data, size_t len, uint8_t* out, Profile p);
bool composeFrame(const uint16_t* rgb565, int srcW, int srcH,
                  uint8_t* out, Profile p);

// Profile token <-> enum. Returns false for an unrecognised token.
bool parseProfile(const char* token, Profile* out);

} // namespace imagepipe
