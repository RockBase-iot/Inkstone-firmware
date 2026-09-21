// Copyright (c) 2026 RockBase-IoT, Chengdu Rockbase Co., Ltd. All rights reserved.

#pragma once
// ============================================================================
// frame_store — flash cache of the last user-pushed display frame
//
// Storage: dedicated 2MB raw flash partition "frame" (subtype 0x99, see
// partitions_inkstone.csv) — sized for future panels up to Spectra 6
// 1600x1200 @ 4bpp (960KB) with margin. NVS is intentionally NOT used for
// this: a ~1MB blob does not belong in NVS.
//
// Layout: 16-byte header {magic, len, crc32, reserved} + frame payload.
// Integrity is verified by CRC32 on load.
//
// Logic: after each successful user-requested refresh the frame is persisted.
// On cold boot, a valid cache is restored; otherwise the built-in four-color
// test pattern is displayed.
// ============================================================================

#include <Arduino.h>

namespace framestore {

// Persist a frame (EPD_FRAME_BYTES). Returns false on flash failure.
bool save(const uint8_t* frame);

// Load the cached frame into out (must hold EPD_FRAME_BYTES).
// Returns false when no valid cache exists or CRC mismatches.
bool load(uint8_t* out);

// Invalidate the cache (e.g. rescue reset).
void clear();

} // namespace framestore
