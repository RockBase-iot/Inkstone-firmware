// Copyright (c) 2026 RockBase-IoT, Chengdu Rockbase Co., Ltd. All rights reserved.

// ============================================================================
// hosttest/parity_main.cpp — run the FIRMWARE quantizer on the host
//
// This compiles the real src/display/retro_quantize.cpp (not a copy, not a
// re-implementation) against a small Arduino shim, feeds it the same 400x300
// RGB888 vectors the Python reference and the browser use, and writes the
// resulting 30000-byte frame to disk.
//
// Why bother: the three-way byte-parity claim in docs/RETRO.md has three ends.
// Python and the browser can both be exercised on the development machine, but
// the firmware end normally needs a board flashed and a serial log read back -
// which means the claim usually only ever gets verified two-thirds of the way,
// and float32 drift in the C++ port goes unnoticed until someone eyeballs a
// panel. Building the same translation unit for the host closes that gap.
//
// What this does NOT prove: that the Xtensa code generator preserves float32
// semantics. That is what -ffp-contract=off in platformio.ini is for, and it is
// verified by flashing and reading the frame back off the device. The host run
// proves the *source-level* arithmetic matches; the flag list proves the
// compiler is not allowed to change it.
//
// Usage:
//   parity_main <input.rgb> <output.bin> [--no-dither]
//   exit 0 = frame written, 1 = quantizer reported failure, 2 = bad usage/IO
//
// Build: see tools/hosttest/run_hosttest.py (it knows the include paths).
// ============================================================================

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#include "display/retro_quantize.h"

int main(int argc, char** argv) {
    if (argc < 3) {
        std::fprintf(stderr,
            "usage: %s <input.rgb> <output.bin> [--no-dither]\n",
            argv[0]);
        return 2;
    }
    const char* in_path = argv[1];
    const char* out_path = argv[2];
    bool dither = true;
    for (int i = 3; i < argc; ++i) {
        if (std::strcmp(argv[i], "--no-dither") == 0) dither = false;
    }

    // 400x300 RGB888 is what every test vector starts as. Read strictly: a short
    // or oversized file means the caller passed the wrong thing, and silently
    // zero-filling would produce a plausible-looking frame that matches nothing.
    const size_t kPixels = (size_t)EPD_WIDTH * EPD_HEIGHT;
    const size_t kRgbBytes = kPixels * 3;
    std::vector<uint8_t> rgb(kRgbBytes);

    std::FILE* f = std::fopen(in_path, "rb");
    if (!f) {
        std::fprintf(stderr, "cannot open input: %s\n", in_path);
        return 2;
    }
    const size_t got = std::fread(rgb.data(), 1, kRgbBytes, f);
    const int extra = std::fgetc(f);
    std::fclose(f);
    if (got != kRgbBytes || extra != EOF) {
        std::fprintf(stderr, "input must be exactly %zu bytes (got %zu%s)\n",
                     kRgbBytes, got, extra == EOF ? "" : "+");
        return 2;
    }

    std::vector<uint8_t> frame(EPD_FRAME_BYTES);
    if (!retro::quantizePack(rgb.data(), frame.data(), dither)) {
        std::fprintf(stderr, "retro::quantizePack reported failure\n");
        return 1;
    }

    f = std::fopen(out_path, "wb");
    if (!f) {
        std::fprintf(stderr, "cannot open output: %s\n", out_path);
        return 2;
    }
    const size_t put = std::fwrite(frame.data(), 1, frame.size(), f);
    std::fclose(f);
    if (put != frame.size()) {
        std::fprintf(stderr, "short write: %zu of %zu\n", put, frame.size());
        return 2;
    }
    return 0;
}
