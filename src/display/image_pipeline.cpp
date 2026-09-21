// Copyright (c) 2026 RockBase-IoT, Chengdu Rockbase Co., Ltd. All rights reserved.

#include "display/image_pipeline.h"
#include "display/retro_quantize.h"
#include "boards/board.h"

#include <JPEGDEC.h>
#include <PNGdec.h>

namespace imagepipe {

// ─── 4-color palette (packing RGB, identical on all three ports) ────────────
static const int16_t PAL[4][3] = {
    {0, 0, 0},        // 0 black
    {255, 255, 255},  // 1 white
    {255, 210, 0},    // 2 yellow
    {200, 30, 30},    // 3 red
};

static inline int nearestColor(int r, int g, int b) {
    int best = 0;
    int32_t bestD = INT32_MAX;
    for (int i = 0; i < 4; ++i) {
        int32_t dr = r - PAL[i][0], dg = g - PAL[i][1], db = b - PAL[i][2];
        int32_t d = 299 * dr * dr + 587 * dg * dg + 114 * db * db;
        if (d < bestD) { bestD = d; best = i; }
    }
    return best;
}

// Floor division (identical on all three ports: Python //, JS Math.floor, here)
static inline int32_t idivFloor(int32_t a, int32_t b) {
    int32_t q = a / b;
    if ((a % b != 0) && ((a < 0) != (b < 0))) --q;
    return q;
}

static inline int clamp255(int32_t v) { return v < 0 ? 0 : (v > 255 ? 255 : (int)v); }

// ─── Quantize + pack ────────────────────────────────────────────────────────
// rgb: EPD_WIDTH*EPD_HEIGHT*3, row-major RGB888
static void quantizePack(const uint8_t* rgb, uint8_t* out, bool dither) {
    const int W = EPD_WIDTH, H = EPD_HEIGHT;
    memset(out, 0, EPD_FRAME_BYTES);

    if (!dither) {
        for (int i = 0; i < W * H; ++i) {
            int c = nearestColor(rgb[i * 3], rgb[i * 3 + 1], rgb[i * 3 + 2]);
            out[i / 4] |= (uint8_t)(c << (6 - 2 * (i % 4)));
        }
        return;
    }

    // Floyd-Steinberg, serpentine scan, error accumulated as 1/16 integers
    int32_t* errR = (int32_t*)ps_calloc(W * H, sizeof(int32_t));
    int32_t* errG = (int32_t*)ps_calloc(W * H, sizeof(int32_t));
    int32_t* errB = (int32_t*)ps_calloc(W * H, sizeof(int32_t));
    if (!errR || !errG || !errB) {   // fall back to nearest-color if PSRAM is short
        free(errR); free(errG); free(errB);
        quantizePack(rgb, out, false);
        return;
    }

    for (int y = 0; y < H; ++y) {
        bool fwd = (y % 2 == 0);
        for (int xi = 0; xi < W; ++xi) {
            int x = fwd ? xi : (W - 1 - xi);
            int idx = y * W + x;
            int r = clamp255(idivFloor(rgb[idx * 3]     * 16 + errR[idx] + 8, 16));
            int g = clamp255(idivFloor(rgb[idx * 3 + 1] * 16 + errG[idx] + 8, 16));
            int b = clamp255(idivFloor(rgb[idx * 3 + 2] * 16 + errB[idx] + 8, 16));
            int c = nearestColor(r, g, b);
            out[idx / 4] |= (uint8_t)(c << (6 - 2 * (idx % 4)));

            int32_t er = (r - PAL[c][0]), eg = (g - PAL[c][1]), eb = (b - PAL[c][2]);
            auto spread = [&](int ni, int w) {
                if (ni < 0 || ni >= W * H) return;
                errR[ni] += er * w; errG[ni] += eg * w; errB[ni] += eb * w;
            };
            int xr = fwd ? x + 1 : x - 1;              // right (serpentine direction)
            if (xr >= 0 && xr < W) spread(y * W + xr, 7);
            if (y + 1 < H) {
                int xl = fwd ? x - 1 : x + 1;          // down-left
                if (xl >= 0 && xl < W) spread((y + 1) * W + xl, 3);
                spread((y + 1) * W + x, 5);            // down
                if (xr >= 0 && xr < W) spread((y + 1) * W + xr, 1); // down-right
            }
        }
    }
    free(errR); free(errG); free(errB);
}

// ─── RGB565 -> center-crop 4:3 -> scale to panel size -> quantize + pack ────
static bool composeFrameImpl(const uint16_t* src, int srcW, int srcH,
                             uint8_t* out, Profile prof) {
    if (!src || srcW <= 0 || srcH <= 0) return false;

    // Center-crop to 4:3 (EPD_WIDTH:EPD_HEIGHT)
    int cropW = srcW, cropH = srcW * EPD_HEIGHT / EPD_WIDTH;
    if (cropH > srcH) { cropH = srcH; cropW = srcH * EPD_WIDTH / EPD_HEIGHT; }
    int offX = (srcW - cropW) / 2, offY = (srcH - cropH) / 2;

    uint8_t* rgb = (uint8_t*)ps_malloc(EPD_WIDTH * EPD_HEIGHT * 3);
    if (!rgb) { Serial.println("[img] rgb buffer alloc failed"); return false; }

    // Nearest-neighbor scaling (400x300 target; bilinear gains are invisible
    // on a 4-color panel)
    for (int y = 0; y < EPD_HEIGHT; ++y) {
        int sy = offY + (int)((int64_t)y * cropH / EPD_HEIGHT);
        const uint16_t* srow = src + sy * srcW;
        for (int x = 0; x < EPD_WIDTH; ++x) {
            int sx = offX + (int)((int64_t)x * cropW / EPD_WIDTH);
            uint16_t p = srow[sx];
            int i = (y * EPD_WIDTH + x) * 3;
            rgb[i]     = (uint8_t)(((p >> 11) & 0x1F) * 255 / 31);
            rgb[i + 1] = (uint8_t)(((p >> 5) & 0x3F) * 255 / 63);
            rgb[i + 2] = (uint8_t)((p & 0x1F) * 255 / 31);
        }
    }
    // RETRO lives in its own translation unit and never touches the DEFAULT
    // code path below, so the legacy output stays byte-identical.
    bool ok = (prof == Profile::IPP_RETRO)
                  ? retro::quantizePack(rgb, out, true)
                  : (quantizePack(rgb, out, prof != Profile::IPP_NONE), true);
    free(rgb);
    return ok;
}

bool composeFrame(const uint16_t* src, int srcW, int srcH,
                  uint8_t* out, bool dither) {
    return composeFrameImpl(src, srcW, srcH, out,
                            dither ? Profile::IPP_DEFAULT : Profile::IPP_NONE);
}

bool composeFrame(const uint16_t* src, int srcW, int srcH,
                  uint8_t* out, Profile p) {
    return composeFrameImpl(src, srcW, srcH, out, p);
}

// ─── JPEG decode ────────────────────────────────────────────────────────────
struct JpegCtx { uint16_t* buf; int w; };

static int jpegDraw(JPEGDRAW* d) {
    JpegCtx* ctx = (JpegCtx*)d->pUser;
    for (int y = 0; y < d->iHeight; ++y) {
        memcpy(ctx->buf + (size_t)(d->y + y) * ctx->w + d->x,
               d->pPixels + (size_t)y * d->iWidth,
               (size_t)d->iWidth * sizeof(uint16_t));
    }
    return 1;
}

bool convertJpeg(const uint8_t* data, size_t len, uint8_t* out, Profile prof) {
    JPEGDEC jpeg;
    if (!jpeg.openRAM(const_cast<uint8_t*>(data), len, jpegDraw)) {
        Serial.println("[img] JPEG open failed");
        return false;
    }
    int w = jpeg.getWidth(), h = jpeg.getHeight();
    uint16_t* buf = (uint16_t*)ps_malloc((size_t)w * h * 2);
    if (!buf) { Serial.println("[img] jpeg frame alloc failed"); jpeg.close(); return false; }
    JpegCtx ctx{buf, w};
    jpeg.setUserPointer(&ctx);
    jpeg.setPixelType(RGB565_LITTLE_ENDIAN);
    bool ok = jpeg.decode(0, 0, 0) == 1;
    jpeg.close();
    if (ok) ok = composeFrame(buf, w, h, out, prof);
    free(buf);
    return ok;
}

bool convertJpeg(const uint8_t* data, size_t len, uint8_t* out, bool dither) {
    return convertJpeg(data, len, out,
                       dither ? Profile::IPP_DEFAULT : Profile::IPP_NONE);
}

// ─── PNG decode ─────────────────────────────────────────────────────────────
struct PngCtx { PNG* png; uint16_t* buf; int w; };

static int pngDraw(PNGDRAW* d) {
    PngCtx* ctx = (PngCtx*)d->pUser;
    ctx->png->getLineAsRGB565(d, ctx->buf + (size_t)d->y * ctx->w,
                              PNG_RGB565_LITTLE_ENDIAN, 0xFFFFFFFF);
    return 1;
}

bool convertPng(const uint8_t* data, size_t len, uint8_t* out, Profile prof) {
    PNG png;
    if (!png.openRAM(const_cast<uint8_t*>(data), len, pngDraw)) {
        Serial.println("[img] PNG open failed");
        return false;
    }
    int w = png.getWidth(), h = png.getHeight();
    uint16_t* buf = (uint16_t*)ps_malloc((size_t)w * h * 2);
    if (!buf) { Serial.println("[img] png frame alloc failed"); png.close(); return false; }
    PngCtx ctx{&png, buf, w};
    bool ok = png.decode(&ctx, 0) == PNG_SUCCESS;
    png.close();
    if (ok) ok = composeFrame(buf, w, h, out, prof);
    free(buf);
    return ok;
}

bool convertPng(const uint8_t* data, size_t len, uint8_t* out, bool dither) {
    return convertPng(data, len, out,
                      dither ? Profile::IPP_DEFAULT : Profile::IPP_NONE);
}

// ─── Profile token parsing (shared by the HTTP layer) ──────────────────────
bool parseProfile(const char* token, Profile* out) {
    if (!token || !out) return false;
    if (strcmp(token, "none")    == 0) { *out = Profile::IPP_NONE;    return true; }
    if (strcmp(token, "default") == 0) { *out = Profile::IPP_DEFAULT; return true; }
    if (strcmp(token, "retro")   == 0) { *out = Profile::IPP_RETRO;   return true; }
    return false;
}

} // namespace imagepipe
