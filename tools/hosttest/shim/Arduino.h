// Copyright (c) 2026 RockBase-IoT, Chengdu Rockbase Co., Ltd. All rights reserved.

// ============================================================================
// hosttest/shim/Arduino.h — minimal Arduino shim for the host parity harness
//
// tools/hosttest/parity_main.cpp compiles the REAL src/display/retro_quantize.cpp
// on the development machine and feeds it the same 400x300 RGB888 vectors as the
// Python reference and the browser port. That makes the firmware end of the
// three-way parity claim testable without a board in the loop.
//
// Only three Arduino things are actually needed: Serial.println (diagnostics),
// ps_malloc/ps_calloc (the PSRAM allocators) and the board macros from
// boards/board.h. Everything else in retro_quantize.cpp is plain C++/math.h.
//
// This header is NOT part of the firmware build - it lives under tools/ and is
// only on the include path of the host harness.
// ============================================================================

#pragma once

#include <cstdarg>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

// ─── Serial ────────────────────────────────────────────────────────────────
// The firmware logs failures through Serial.println. On the host those go to
// stderr so they never contaminate the stdout hex digest the harness prints.
namespace host_shim {

class HostSerial {
public:
    void println(const char* s) { std::fprintf(stderr, "%s\n", s); }
    void println() { std::fputc('\n', stderr); }
    void println(int v) { std::fprintf(stderr, "%d\n", v); }
    void print(const char* s) { std::fprintf(stderr, "%s", s); }
    void printf(const char* fmt, ...) {
        va_list ap;
        va_start(ap, fmt);
        std::vfprintf(stderr, fmt, ap);
        va_end(ap);
    }
    void begin(unsigned long) {}
};

}  // namespace host_shim

static host_shim::HostSerial Serial;

// ─── PSRAM allocators ──────────────────────────────────────────────────────
// On device these land in PSRAM. On the host plain malloc/calloc is enough as
// long as the semantics (zero-init for calloc, freeable with free()) match —
// retro_quantize.cpp relies on exactly that.
static inline void* ps_malloc(size_t n) { return std::malloc(n); }

static inline void* ps_calloc(size_t count, size_t size) {
    return std::calloc(count, size);
}

// ─── Arduino odds and ends the sources may reference ───────────────────────
using byte = uint8_t;
