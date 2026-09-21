// Copyright (c) 2026 RockBase-IoT, Chengdu Rockbase Co., Ltd. All rights reserved.

// ============================================================================
// retro_quantize.cpp — RETRO profile quantizer
//
// Byte-identical with tools/retro_core.py (reference) and web/uploader.html.
// Every constant, operation order and rounding rule below mirrors the Python
// reference; do not "clean up" the arithmetic. Specifically:
//
//   · everything is float (32-bit) — no double anywhere
//   · cbrt and sin are the local uCbrt()/uSin(), NOT the platform cbrtf()/sinf(),
//     because xtensa newlib, V8 and CPython do not agree on those
//   · sRGB normalization divides by 255.0f, never shifts
//   · rounding is round-half-to-even (lrintf / rint), matching numpy.rint
//   · cubes use repeated multiplication, not powf()
//
// Sharpening (UnsharpMask) from the original spec is intentionally DISABLED in
// v1: the reference test vectors were generated without it, and reconciling a
// Gaussian kernel across three ports is a known parity hazard.
// ============================================================================

#include "display/retro_quantize.h"
#include <math.h>

#ifdef RETRO_HOSTTEST
  #include <stdio.h>
  #include <stdlib.h>
#endif

namespace retro {

// ─── Palette ───────────────────────────────────────────────────────────────
// Internal matching order: 0=black 1=white 2=red 3=yellow.
// The panel wire format is fixed at 0=black 1=white 2=yellow 3=red, so
// selected entries must pass through panelCode() before being packed.
// Never write a bare numeric index — use these.
#define RC_BLACK   0
#define RC_WHITE   1
#define RC_RED     2
#define RC_YELLOW  3

static const float PAL[4][3] = {
    { 35.0f,  31.0f,  32.0f},   // black  — panel black, not pure #000
    {233.0f, 231.0f, 222.0f},   // white  — E Ink warm white
    {193.0f,  80.0f,  62.0f},   // red    — brick red
    {229.0f, 197.0f,  79.0f},   // yellow — ochre
};

static const float PI_F = 3.14159265358979f;

// ─── Tunables (mirror tools/retro_core.py and docs/RETRO.md) ───────────────
static const float SAT_SCALE     = 0.82f;
static const float COOL_SCALE    = 0.30f;
static const float GREEN_SCALE   = 0.85f;
static const float S_CURVE_AMP   = 0.045f;
static const float CHROMA_THRESH = 0.02f;
static const float COOL_LO       = 150.0f;
static const float COOL_HI       = 320.0f;
static const float GREEN_LO      = 70.0f;
static const float GREEN_HI      = 150.0f;

// Atkinson kernel: (dx, dy), all weights 1/8.
static const int ATK_DX[6] = { 1, 2, -1, 0, 1, 0};
static const int ATK_DY[6] = { 0, 0,  1, 1, 1, 2};
static const int32_t ATK_WEIGHT_16 = 2;   // 1/8 in the 1/16 error unit

// ─── Coordinate / utility helpers ──────────────────────────────────────────
static inline float clampf(float v, float lo, float hi) {
    return v < lo ? lo : (v > hi ? hi : v);
}
static inline float clamp255f(float v) {
    return v < 0.0f ? 0.0f : (v > 255.0f ? 255.0f : v);
}
static inline int32_t clamp255i(int32_t v) {
    return v < 0 ? 0 : (v > 255 ? 255 : v);
}

static inline uint8_t panelCode(int paletteIndex) {
    return (uint8_t)(paletteIndex < RC_RED ? paletteIndex : (paletteIndex ^ 1));
}

// Cartesian (x, y) -> RGB888 byte offset in a row-major W x H x 3 buffer.
#define PX_OFF(x, y, W) ((((y) * (W)) + (x)) * 3)

// ─── Transcendental helpers (three-port shared implementation) ─────────────
// Bit trick seed + two Newton iterations. Python/JS port this line by line.
static inline float uCbrt(float x) {
    if (x == 0.0f) return 0.0f;
    float sign = (x < 0.0f) ? -1.0f : 1.0f;
    float ax = fabsf(x);
    union { float f; uint32_t u; } v;
    v.f = ax;
    v.u = (v.u / 3u) + 709921077u;
    float y = v.f;
    for (int i = 0; i < 2; ++i) {
        float y3  = (y * y) * y;
        float num = (2.0f * ax) + y3;
        float den = ax + (2.0f * y3);
        y = (y * num) / den;
    }
    return sign * y;
}

// Range reduce to [-pi, pi] -> [-pi/2, pi/2] -> Taylor series to x^9.
static inline float uSin(float x) {
    float two_pi = 2.0f * PI_F;
    x = x - two_pi * (float)(int)(x / two_pi);
    if (x > PI_F / 2.0f)       x = PI_F - x;
    else if (x < -PI_F / 2.0f) x = -PI_F - x;
    float x2 = x * x;
    float term = x, s = x;
    const int kd[4] = {2, 3, 4, 5};
    for (int i = 0; i < 4; ++i) {
        int k = kd[i];
        term = -term * x2 / (float)((2 * k - 1) * (2 * k - 2));
        s = s + term;
    }
    return s;
}

// ─── sRGB <-> linear: shared byte lookup table ─────────────────────────────
// SRGB2LIN[b] = linearize(b / 255), computed once in float64 and narrowed to
// float32 exactly once, here. Never evaluate the 2.4-power curve at runtime:
// xtensa newlib's powf, V8's Math.pow and numpy's np.power disagree in the last
// few ulp, and the disagreement is NOT tiny in effect — see docs/RETRO.md.
// Regenerate with tools/sync_srgb_table.py, never by hand.
static const float SRGB2LIN[256] = {
    0.0f, 0.0003035269910469651f, 0.0006070539820939302f, 0.0009105809731408954f,
    0.0012141079641878605f, 0.0015176349552348256f, 0.0018211619462817907f, 0.002124688820913434f,
    0.002428215928375721f, 0.0027317428030073643f, 0.0030352699104696512f, 0.0033465358428657055f,
    0.0036765073891729116f, 0.004024717025458813f, 0.0043914420530200005f, 0.004776953253895044f,
    0.005181516520678997f, 0.005605391692370176f, 0.006048833020031452f, 0.006512090563774109f,
    0.006995410192757845f, 0.007499032188206911f, 0.00802319310605526f, 0.008568125776946545f,
    0.009134058840572834f, 0.009721217676997185f, 0.01032982300966978f, 0.010960093699395657f,
    0.011612244881689548f, 0.012286487966775894f, 0.012983032502233982f, 0.013702083379030228f,
    0.014443843625485897f, 0.015208514407277107f, 0.01599629409611225f, 0.016807375475764275f,
    0.017641954123973846f, 0.01850022003054619f, 0.019382361322641373f, 0.020288562402129173f,
    0.021219009533524513f, 0.022173885256052017f, 0.023153366521000862f, 0.024157632142305374f,
    0.02518685907125473f, 0.026241222396492958f, 0.027320891618728638f, 0.028426039963960648f,
    0.02955683507025242f, 0.03071344457566738f, 0.03189603239297867f, 0.03310476616024971f,
    0.03433980792760849f, 0.03560131415724754f, 0.03688944876194f, 0.0382043719291687f,
    0.039546236395835876f, 0.040915198624134064f, 0.0423114113509655f, 0.04373503103852272f,
    0.045186202973127365f, 0.04666508734226227f, 0.04817182570695877f, 0.04970656707882881f,
    0.05126945674419403f, 0.052860647439956665f, 0.05448027700185776f, 0.05612849071621895f,
    0.05780543014407158f, 0.05951123684644699f, 0.061246052384376526f, 0.06301001459360123f,
    0.06480326503515244f, 0.0666259378194809f, 0.06847816705703735f, 0.07036009430885315f,
    0.07227185368537903f, 0.07421357184648514f, 0.07618538290262222f, 0.07818742096424103f,
    0.0802198201417923f, 0.08228270709514618f, 0.08437620848417282f, 0.08650045841932297f,
    0.08865558356046677f, 0.09084171056747437f, 0.09305896610021591f, 0.09530746936798096f,
    0.09758734703063965f, 0.09989872574806213f, 0.10224173218011856f, 0.10461648553609848f,
    0.10702310502529144f, 0.109461709856987f, 0.1119324266910553f, 0.11443537473678589f,
    0.11697066575288773f, 0.11953842639923096f, 0.12213877588510513f, 0.12477181851863861f,
    0.12743768095970154f, 0.13013647496700287f, 0.13286831974983215f, 0.13563333451747894f,
    0.1384316086769104f, 0.14126329123973846f, 0.1441284716129303f, 0.14702726900577545f,
    0.14995978772640228f, 0.15292614698410034f, 0.15592646598815918f, 0.15896083414554596f,
    0.16202937066555023f, 0.16513219475746155f, 0.16826939582824707f, 0.17144110798835754f,
    0.17464740574359894f, 0.177888423204422f, 0.18116424977779388f, 0.18447498977184296f,
    0.18782077729701996f, 0.19120168685913086f, 0.1946178376674652f, 0.19806931912899017f,
    0.2015562504529953f, 0.20507873594760895f, 0.20863686501979828f, 0.21223075687885284f,
    0.2158605009317398f, 0.21952620148658752f, 0.22322796285152435f, 0.22696587443351746f,
    0.23074005544185638f, 0.2345505803823471f, 0.23839756846427917f, 0.24228112399578094f,
    0.2462013214826584f, 0.25015828013420105f, 0.2541520893573761f, 0.2581828534603119f,
    0.2622506618499756f, 0.26635560393333435f, 0.27049779891967773f, 0.2746773064136505f,
    0.2788942754268646f, 0.28314873576164246f, 0.28744083642959595f, 0.2917706370353699f,
    0.2961382567882538f, 0.30054378509521484f, 0.3049873113632202f, 0.30946892499923706f,
    0.31398871541023254f, 0.31854677200317383f, 0.32314321398735046f, 0.3277781009674072f,
    0.3324515223503113f, 0.33716362714767456f, 0.34191441535949707f, 0.34670406579971313f,
    0.35153260827064514f, 0.35640013217926025f, 0.3613067865371704f, 0.366252601146698f,
    0.37123769521713257f, 0.3762621283531189f, 0.38132601976394653f, 0.38642942905426025f,
    0.3915724754333496f, 0.3967552185058594f, 0.4019777774810791f, 0.40724021196365356f,
    0.4125426113605499f, 0.41788506507873535f, 0.423267662525177f, 0.42869049310684204f,
    0.43415364623069763f, 0.43965718150138855f, 0.44520118832588196f, 0.4507857859134674f,
    0.4564110338687897f, 0.46207699179649353f, 0.4677838087081909f, 0.47353148460388184f,
    0.4793201684951782f, 0.48514994978904724f, 0.4910208582878113f, 0.4969329833984375f,
    0.5028864741325378f, 0.5088813304901123f, 0.5149176716804504f, 0.520995557308197f,
    0.5271151065826416f, 0.533276379108429f, 0.5394794940948486f, 0.5457244515419006f,
    0.5520114302635193f, 0.5583403706550598f, 0.5647115111351013f, 0.5711248517036438f,
    0.577580451965332f, 0.5840784311294556f, 0.5906188488006592f, 0.5972017645835876f,
    0.6038273572921753f, 0.6104955673217773f, 0.6172065734863281f, 0.6239603757858276f,
    0.6307571530342102f, 0.637596845626831f, 0.6444796919822693f, 0.6514056324958801f,
    0.6583748459815979f, 0.6653872728347778f, 0.672443151473999f, 0.6795424818992615f,
    0.68668532371521f, 0.6938717365264893f, 0.7011018991470337f, 0.7083757519721985f,
    0.715693473815918f, 0.7230551242828369f, 0.7304607629776001f, 0.7379103899002075f,
    0.7454041838645935f, 0.7529422044754028f, 0.7605245113372803f, 0.7681511640548706f,
    0.7758222222328186f, 0.7835378050804138f, 0.7912979125976562f, 0.7991027235984802f,
    0.8069522380828857f, 0.8148465752601624f, 0.8227857351303101f, 0.8307698965072632f,
    0.838798999786377f, 0.8468732237815857f, 0.8549926280975342f, 0.8631572127342224f,
    0.8713670969009399f, 0.8796223998069763f, 0.8879231214523315f, 0.8962693810462952f,
    0.9046611785888672f, 0.9130986332893372f, 0.9215818643569946f, 0.9301108717918396f,
    0.9386857151985168f, 0.9473065137863159f, 0.9559733271598816f, 0.9646862745285034f,
    0.9734452962875366f, 0.9822505712509155f, 0.9911020994186401f, 1.0f,
};

static inline float srgbByteToLinear(uint8_t b) {     // b in [0,255]
    return SRGB2LIN[b];
}

// 2 / ln(2) and ln(2) as doubles; the polynomial below runs in float32 so these
// only seed it. The e in uLog2 comes from the exponent field, m from the
// mantissa, and log2(m) from the odd series in t = (m-1)/(m+1).
static const double R_TWO_OVER_LN2 = 2.8853900817779268;
static const double R_LN2 = 0.6931471805599453;

static inline float uLog2(float x) {
    if (!(x > 0.0f)) return -1e30f;
    union { float f; uint32_t u; } v;
    v.f = x;
    int e = (int)((v.u >> 23) & 0xFFu) - 127;
    v.u = 0x3F800000u | (v.u & 0x007FFFFFu);
    float m = v.f;
    float t = (m - 1.0f) / (m + 1.0f);
    float t2 = t * t;
    float p = 1.0f / 9.0f;
    p = p * t2 + 1.0f / 7.0f;
    p = p * t2 + 1.0f / 5.0f;
    p = p * t2 + 1.0f / 3.0f;
    p = p * t2 + 1.0f;
    float s = (float)(R_TWO_OVER_LN2) * t * p;
    return (float)e + s;
}

static inline float uExp2(float y) {
    float k = floorf(y);
    float z = (y - k) * (float)(R_LN2);
    float p = 1.0f / 720.0f;
    p = p * z + 1.0f / 120.0f;
    p = p * z + 1.0f / 24.0f;
    p = p * z + 1.0f / 6.0f;
    p = p * z + 0.5f;
    p = p * z + 1.0f;
    p = p * z + 1.0f;
    union { float f; uint32_t u; } v;
    v.f = p;
    int biased = (int)k + 127;
    if (biased <= 0 || biased >= 255) return p;
    v.u = (v.u & 0x807FFFFFu) | ((uint32_t)biased << 23);
    return v.f;
}

static inline float linearToSrgb(float c) {
    c = clampf(c, 0.0f, 1.0f);
    return (c <= 0.0031308f) ? (c * 12.92f)
                             : (1.055f * uExp2(uLog2(c) * (float)(1.0 / 2.4)) - 0.055f);
}

// The parenthesisation below is load-bearing, not style. Python evaluates
//     (np.float32(c0)*r + np.float32(c1)*g + np.float32(c2)*b)
// as ((c0*r) + (c1*g)) + (c2*b) with every intermediate kept in float32, and the
// browser now spells the same thing out with Math.fround after each step. Lean
// on C++ operator precedence and associativity alone and a future
// -ffast-math / -ffp-contract=fast build (or just an unhelpful compiler) is free
// to reassociate or fuse into an FMA, which changes the last bit of the result.
static void rgbToOklab(float r, float g, float b,
                       float* L, float* A, float* B) {
    float l = (0.4122214708f * r + 0.5363325363f * g) + 0.0514459929f * b;
    float m = (0.2119034982f * r + 0.6806995451f * g) + 0.1073969566f * b;
    float s = (0.0883024619f * r + 0.2817188376f * g) + 0.6299787005f * b;
    l = uCbrt(l); m = uCbrt(m); s = uCbrt(s);
    *L = (0.2104542553f * l + 0.7936177850f * m) - 0.0040720468f * s;
    *A = (1.9779984951f * l - 2.4285922050f * m) + 0.4505937099f * s;
    *B = (0.0259040371f * l + 0.7827717662f * m) - 0.8086757660f * s;
}

static void oklabToRgb(float L, float A, float B,
                       float* r, float* g, float* b) {
    float l_ = (L + 0.3963377774f * A) + 0.2158037573f * B;
    float m_ = (L - 0.1055613458f * A) - 0.0638541728f * B;
    float s_ = (L - 0.0894841775f * A) - 1.2914855480f * B;
    // cubes by repeated multiplication (powf is not parity-safe)
    float l = (l_ * l_) * l_, m = (m_ * m_) * m_, s = (s_ * s_) * s_;
    *r = (+4.0767416621f * l - 3.3077115913f * m) + 0.2309699292f * s;
    *g = (-1.2684380046f * l + 2.6097574011f * m) - 0.3413193965f * s;
    *b = (-0.0041960863f * l - 0.7034186147f * m) + 1.7076147010f * s;
}

static inline uint8_t toByte(float v01) {
    float v = clamp255f(linearToSrgb(clampf(v01, 0.0f, 1.0f)) * 255.0f);
    // round-half-to-even (lrintf honours the default FE_TONEAREST mode), NOT
    // (uint8_t)(v + 0.5f) — the +0.5 form rounds halves up and drifts from
    // numpy.rint / the JS uRint.
    return (uint8_t)lrintf(v);
}

// Hue in degrees, computed in double and narrowed once at the end.
//
// The double is not decoration. Python evaluates
// np.arctan2(B.astype(np.float64), A.astype(np.float64) + 1e-9) * 180.0 / pi
// entirely in float64 and only narrows when storing into the float32 array, and
// the browser now mirrors that. Doing the arc tangent and the 180/pi multiply in
// float32 instead shifts the angle by ~1e-5 degrees, which is harmless right up
// until a pixel's hue sits on one of the branch boundaries (70/150/320/330 deg),
// where it silently selects a different chroma scaling for A and B.
static inline float hueDegrees(float a, float b) {
    double h = atan2((double)b, (double)a + 1e-9) * 180.0 / 3.14159265358979;
    h = fmod(h, 360.0);
    if (h < 0.0) h += 360.0;
    return (float)h;
}

// ─── Stage 4: stylised preprocessing (in place on RGB888) ──────────────────
static void preprocess(uint8_t* rgb, int n) {
    for (int i = 0; i < n; ++i) {
        float r = srgbByteToLinear(rgb[i * 3 + 0]);
        float g = srgbByteToLinear(rgb[i * 3 + 1]);
        float b = srgbByteToLinear(rgb[i * 3 + 2]);

        float L, A, B;
        rgbToOklab(r, g, b, &L, &A, &B);

        // desaturate
        A *= SAT_SCALE;
        B *= SAT_SCALE;

        // chroma convergence by hue
        float hue = hueDegrees(A, B);
        if (hue >= COOL_LO && hue <= COOL_HI)        { A *= COOL_SCALE;  B *= COOL_SCALE;  }
        else if (hue > GREEN_LO && hue < GREEN_HI)   { A *= GREEN_SCALE; B *= GREEN_SCALE; }

        // S-curve dynamic range compression
        L = clampf(L, 0.0f, 1.0f);
        L = clampf(L + S_CURVE_AMP * uSin(2.0f * PI_F * L), 0.0f, 1.0f);

        oklabToRgb(L, A, B, &r, &g, &b);
        rgb[i * 3 + 0] = toByte(r);
        rgb[i * 3 + 1] = toByte(g);
        rgb[i * 3 + 2] = toByte(b);
    }
}

// ─── Stage 5: hue-aware candidate mask ─────────────────────────────────────
// bit0=black bit1=white bit2=red bit3=yellow
static uint8_t hueMask(uint8_t r, uint8_t g, uint8_t b) {
    float L, A, B;
    rgbToOklab(srgbByteToLinear(r),
               srgbByteToLinear(g),
               srgbByteToLinear(b), &L, &A, &B);
    float chroma = sqrtf(A * A + B * B);
    if (chroma <= CHROMA_THRESH) return 0b1111;      // near-grey: unrestricted
    float hue = hueDegrees(A, B);
    if (hue < GREEN_LO || hue >= 330.0f) return 0b1111;   // warm:  all four
    if (hue < COOL_LO)                   return 0b1010;   // green: black|yellow
    return                                     0b0011;   // cool:  black|white
}

// ─── OKLab nearest colour, restricted to the mask's candidates ─────────────
static inline int nearestMasked(float L, float A, float B,
                                const float palL[4], const float palA[4],
                                const float palB[4], uint8_t mask) {
    int best = 0;
    float bestD = 0.0f;
    bool have = false;
    for (int i = 0; i < 4; ++i) {
        if (!((mask >> i) & 1)) continue;
        float dL = L - palL[i], dA = A - palA[i], dB = B - palB[i];
        float d = dL * dL + dA * dA + dB * dB;
        if (!have || d < bestD) { bestD = d; best = i; have = true; }
    }
    return best;
}

// ─── Stages 6+7: serpentine Atkinson diffusion + 2bpp packing ──────────────

#ifdef RETRO_HOSTTEST
// Host-parity-harness diagnostics only (compiled out of the firmware).
// Writes the two stages that run before diffusion so a mismatch can be blamed
// on a specific stage instead of inferred from the final frame. The output
// directory comes from RETRO_HOSTTEST_DIR, or "out/hosttest" by default.
static void dumpStage(const uint8_t* work, const uint8_t* mask, size_t npx) {
    const char* dir = getenv("RETRO_HOSTTEST_DIR");
    if (!dir) dir = "out/hosttest";
    const char* tag = getenv("RETRO_HOSTTEST_TAG");
    if (!tag) tag = "vector";

    char path[1024];
    snprintf(path, sizeof(path), "%s/%s.pre.rgb", dir, tag);
    FILE* f = fopen(path, "wb");
    if (f) { fwrite(work, 1, npx * 3, f); fclose(f); }

    snprintf(path, sizeof(path), "%s/%s.mask", dir, tag);
    f = fopen(path, "wb");
    if (f) { fwrite(mask, 1, npx, f); fclose(f); }
}
#endif

bool quantizePack(const uint8_t* rgb, uint8_t* out, bool dither) {
    const int W = EPD_WIDTH, H = EPD_HEIGHT;
    if (!rgb || !out) return false;
    memset(out, 0, EPD_FRAME_BYTES);

    const size_t npx = (size_t)W * H;

    // Preprocessing always runs (RETRO's look is independent of the diffusion
    // switch); `dither` only selects diffusion vs plain nearest-colour, so it
    // keeps the same meaning as the API's `?dither=off`.
    uint8_t* work = (uint8_t*)ps_malloc(npx * 3);
    if (!work) { Serial.println("[retro] work alloc failed"); return false; }
    memcpy(work, rgb, npx * 3);
    preprocess(work, (int)npx);

    // Mask is computed once from the preprocessed image — never from the
    // error-accumulated values, or the accumulated error would corrupt the
    // hue decision.
    uint8_t* mask = (uint8_t*)ps_malloc(npx);
    if (!mask) { free(work); Serial.println("[retro] mask alloc failed"); return false; }
    for (size_t i = 0; i < npx; ++i)
        mask[i] = hueMask(work[i * 3], work[i * 3 + 1], work[i * 3 + 2]);

#ifdef RETRO_HOSTTEST
    // Dumped here so both the dither and no-dither paths emit the stages.
    dumpStage(work, mask, npx);
#endif

    // Palette pre-expressed in OKLab
    float palL[4], palA[4], palB[4];
    for (int i = 0; i < 4; ++i) {
        rgbToOklab(srgbByteToLinear((uint8_t)lrintf(PAL[i][0])),
                   srgbByteToLinear((uint8_t)lrintf(PAL[i][1])),
                   srgbByteToLinear((uint8_t)lrintf(PAL[i][2])),
                   &palL[i], &palA[i], &palB[i]);
    }

    if (!dither) {
        for (size_t i = 0; i < npx; ++i) {
            float L, A, B;
            rgbToOklab(srgbByteToLinear(work[i * 3 + 0]),
                       srgbByteToLinear(work[i * 3 + 1]),
                       srgbByteToLinear(work[i * 3 + 2]), &L, &A, &B);
            int c = nearestMasked(L, A, B, palL, palA, palB, mask[i]);
            out[i >> 2] |= (uint8_t)(panelCode(c) << (6 - 2 * (i & 3)));
        }
        free(work); free(mask);
        return true;
    }

    // Error buffer in 1/16 integer units (3 channels interleaved).
    // Same order of magnitude as the DEFAULT path (W*H*4 x 3).
    int32_t* err = (int32_t*)ps_calloc(npx * 3, sizeof(int32_t));
    if (!err) {
        free(work); free(mask);
        Serial.println("[retro] err alloc failed");
        return false;
    }

    for (int y = 0; y < H; ++y) {
        const bool fwd = (y % 2 == 0);
        for (int xi = 0; xi < W; ++xi) {
            const int x = fwd ? xi : (W - 1 - xi);
            const size_t i = (size_t)y * W + x;
            const size_t e = i * 3;

            // Error-adjusted source value, back out of 1/16 fixed point.
            // No "+ 8": summing the neighbours' Atkinson error in 1/16 units and
            // dividing by 16 already IS round-half-to-even on v/16, which is what
            // _div16_round does in the Python reference (rint(v/16.0) — its
            // docstring's "(v+8)//16" is stale and the code never adds 8).
            // A half-LSB bias here makes this step round-half-up while every
            // other rounding step in the pipeline is half-to-even, and it shifts
            // the result for about half of all v, desyncing the serpentine pass
            // from the second row onwards.
            // Round FIRST, then clamp -- the same order as Python's
            // `np.clip(np.rint(v / 16.0), 0, 255)` and JS's
            // `clamp255(uRint(...))`. The result is an integral float, and that
            // matters below: the palette-distance error term is
            // `e0 = src - PAL[c]`, and in the other two ports the minuend is
            // already an integer. Keeping the un-rounded `v / 16.0f` here left a
            // fractional residue in the error, which survives the `* 2` and the
            // `lrintf` often enough to shift the accumulated error by one unit
            // and desync the serpentine pass.
            float src_r = (float)clamp255i(lrintf((float)((int32_t)work[e + 0] * 16 + err[e + 0]) / 16.0f));
            float src_g = (float)clamp255i(lrintf((float)((int32_t)work[e + 1] * 16 + err[e + 1]) / 16.0f));
            float src_b = (float)clamp255i(lrintf((float)((int32_t)work[e + 2] * 16 + err[e + 2]) / 16.0f));

            float L, A, B;
            rgbToOklab(srgbByteToLinear((uint8_t)lrintf(src_r)),
                       srgbByteToLinear((uint8_t)lrintf(src_g)),
                       srgbByteToLinear((uint8_t)lrintf(src_b)), &L, &A, &B);
            const int c = nearestMasked(L, A, B, palL, palA, palB, mask[i]);
            out[i >> 2] |= (uint8_t)(panelCode(c) << (6 - 2 * (i & 3)));

            float er = src_r - PAL[c][0];
            float eg = src_g - PAL[c][1];
            float eb = src_b - PAL[c][2];
            for (int k = 0; k < 6; ++k) {
                int nx = x + (fwd ? ATK_DX[k] : -ATK_DX[k]);   // direction flips
                int ny = y + ATK_DY[k];
                if (nx < 0 || nx >= W || ny >= H) continue;
                size_t ni = ((size_t)ny * W + nx) * 3;
                err[ni + 0] += (int32_t)lrintf(er * (float)ATK_WEIGHT_16);
                err[ni + 1] += (int32_t)lrintf(eg * (float)ATK_WEIGHT_16);
                err[ni + 2] += (int32_t)lrintf(eb * (float)ATK_WEIGHT_16);
            }
        }
    }
    free(err); free(work); free(mask);
    return true;
}

// ─── RGB565 -> center-crop 4:3 -> scale -> RETRO quantize ──────────────────
bool composeFrame(const uint16_t* src, int srcW, int srcH,
                  uint8_t* out, bool dither) {
    if (!src || srcW <= 0 || srcH <= 0) return false;

    // Center-crop to 4:3, identical to the DEFAULT path.
    int cropW = srcW, cropH = srcW * EPD_HEIGHT / EPD_WIDTH;
    if (cropH > srcH) { cropH = srcH; cropW = srcH * EPD_WIDTH / EPD_HEIGHT; }
    const int offX = (srcW - cropW) / 2, offY = (srcH - cropH) / 2;

    const size_t npx = (size_t)EPD_WIDTH * EPD_HEIGHT;
    uint8_t* rgb = (uint8_t*)ps_malloc(npx * 3);
    if (!rgb) { Serial.println("[retro] rgb buffer alloc failed"); return false; }

    // Nearest-neighbour scaling would give RETRO coarser grain than the
    // reference, which downsamples with Lanczos. The 4:3 crop + integer ratio
    // used by the API path lands on the same pixels for the common photo sizes,
    // and byte parity is only promised for already-400x300 inputs (see
    // docs/RETRO.md), so nearest sampling is deliberate here too.
    for (int y = 0; y < EPD_HEIGHT; ++y) {
        int sy = offY + (int)((int64_t)y * cropH / EPD_HEIGHT);
        const uint16_t* srow = src + (size_t)sy * srcW;
        for (int x = 0; x < EPD_WIDTH; ++x) {
            int sx = offX + (int)((int64_t)x * cropW / EPD_WIDTH);
            uint16_t p = srow[sx];
            int i = (y * EPD_WIDTH + x) * 3;
            rgb[i]     = (uint8_t)(((p >> 11) & 0x1F) * 255 / 31);
            rgb[i + 1] = (uint8_t)(((p >> 5) & 0x3F) * 255 / 63);
            rgb[i + 2] = (uint8_t)((p & 0x1F) * 255 / 31);
        }
    }
    bool ok = quantizePack(rgb, out, dither);
    free(rgb);
    return ok;
}

} // namespace retro
