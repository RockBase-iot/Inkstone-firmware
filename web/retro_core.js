"use strict";
/* Copyright (c) 2026 RockBase-IoT, Chengdu Rockbase Co., Ltd. All rights reserved.
 * ============================================================================
 * retro_core.js — RETRO quantization profile (browser port)
 *
 * Byte-identical with tools/retro_core.py (reference) and the firmware's
 * src/display/retro_quantize.cpp. The parity rules that make this hold:
 *
 *   · every intermediate is float32 — this file keeps them in Float32Array
 *     buffers so assignment truncates automatically, and wraps scalar maths in
 *     fr() (= Math.fround) rather than sprinkling casts
 *   · cbrt and sin are the local uCbrt()/uSin(), NOT Math.cbrt()/Math.sin(),
 *     because V8's libm does not match xtensa newlib or numpy
 *   · sRGB normalization divides by 255, never shifts
 *   · rounding is round-half-to-even (uRint), matching numpy.rint / lrintf
 *     — note JS Math.round() is half-up and disagrees for negative halves
 *   · cubes use repeated multiplication, not Math.pow()
 *
 * Sharpening is intentionally disabled in v1 (see docs/RETRO.md).
 *
 * §9 of the firmware header explains the packing contract; this module only
 * produces the 30000-byte 2bpp frame.
 * ============================================================================ */

const RETRO_W = 400, RETRO_H = 300, RETRO_FRAME_BYTES = 30000;

/* Internal palette order: 0=black 1=white 2=RED 3=YELLOW. The GDEY0420F51
  wire format instead uses 2=yellow and 3=red, so pack/unpack through
  retroPanelCode(). */
const RETRO_PAL = new Float32Array([
  35, 31, 32,       /* black  */
  233, 231, 222,    /* white  */
  193, 80, 62,      /* red    */
  229, 197, 79      /* yellow */
]);

/* Preview-only palette (simulated panel tone; never packed) */
const RETRO_PAL_VIEW = [
  [20, 20, 20],
  [245, 243, 235],
  [185, 45, 40],
  [225, 190, 25]
];

/* Red and yellow swap between RETRO's internal palette and the panel's 2bpp
  wire format. The conversion is its own inverse. */
function retroPanelCode(color) { return color < 2 ? color : color ^ 1; }

/* Tunables — must match tools/retro_core.py */
const fr = Math.fround;

/* bit-reinterpret scratch, allocated once — uCbrt runs 3x per pixel */
const cbrtF = new Float32Array(1), cbrtU = new Uint32Array(cbrtF.buffer);

/* Tunables — must match tools/retro_core.py.
   NOTE: every one is wrapped in fr(). The reference stores them as np.float32,
   so `A * SAT_SCALE` there is float32*float32. A bare JS double would multiply
   in float64 and land on a different last bit. */
const R_SAT_SCALE = fr(0.82), R_COOL_SCALE = fr(0.30), R_GREEN_SCALE = fr(0.85);
const R_S_CURVE_AMP = fr(0.045), R_CHROMA_THRESH = fr(0.02);
const R_COOL_LO = 150.0, R_COOL_HI = 320.0, R_GREEN_LO = 70.0, R_GREEN_HI = 150.0;
const R_PI = 3.14159265358979;

/* Atkinson kernel: 6 neighbours, each 1/8 -> 75% of the error is propagated.
   The discarded 25% is what gives the finer film grain. */
const R_ATK_DX = [1, 2, -1, 0, 1, 0];
const R_ATK_DY = [0, 0, 1, 1, 1, 2];
const R_ATK_WEIGHT_16 = 2;   /* 1/8 expressed in the 1/16 error unit */

/* OKLab matrices. Every entry is wrapped in fr() so the literal is stored as the
   float32 it will be multiplied as. A bare JS numeric literal is a double: using
   it directly means `fr(fr(0.0883024619) * r)` starts from 0.08830246190000000...821
   instead of the float32 0.08830246329307556, and the product comes out 1 ulp
   low. That is invisible per-pixel until it lands on a rounding tie — it is what
   made one red channel in v6 come out 236 instead of 235. Python keeps these as
   np.float32(...) constants; this is the JS equivalent. */
const BASE_IDX = fr(0.4122214708), BASE_MG = fr(0.5363325363), BASE_SB = fr(0.0514459929);
const LMS_L = fr(0.2119034982), LMS_MG = fr(0.6806995451), LMS_SB = fr(0.1073969566);
const S_L = fr(0.0883024619), S_MG = fr(0.2817188376), S_SB = fr(0.6299787005);
const O_LL = fr(0.2104542553), O_LM = fr(0.7936177850), O_LS = fr(-0.0040720468);
const O_AL = fr(1.9779984951), O_AM = fr(-2.4285922050), O_AS = fr(0.4505937099);
const O_BL = fr(0.0259040371), O_BM = fr(0.7827717662), O_BS = fr(-0.8086757660);

/* ── Transcendental helpers: ported line by line from the Python reference ── */
function uCbrt(x) {
  x = fr(x);
  if (x === 0) return 0;
  const sign = x < 0 ? -1 : 1;
  const ax = Math.abs(x);
  cbrtF[0] = ax;
  cbrtU[0] = Math.floor(cbrtU[0] / 3) + 709921077;
  let y = fr(cbrtF[0]);
  for (let i = 0; i < 2; i++) {
    const y3 = fr(fr(y * y) * y);
    const num = fr(fr(2 * ax) + y3);
    const den = fr(ax + fr(2 * y3));
    y = fr(fr(y * num) / den);
  }
  return sign * y;
}

/* Reciprocal two-pi, hardcoded so the range reduction does not depend on how
   the host rounds the division or the double->float32 conversion. */
const R_INV_TWO_PI = 1.5915494309189535e-1;   /* 1 / (2*pi) as a double */
const R_TWO_PI = 6.2831854820251465;          /* f32(2 * f32(pi)), as a double */

const f32buf = new Float32Array(1), u32buf = new Uint32Array(f32buf.buffer);
const f32 = x => { f32buf[0] = x; return f32buf[0]; };

/* uSin mirrors the Python reference *including its stray float64*: Python's
   u_sin_scalar receives a numpy.float32 `x`, computes `two_pi` as a Python
   float (float64), and the `pi - x` branch promotes to float64 before
   `float(np.float32(...))` narrows once at the end. Narrowing every step in JS
   instead loses the last bit and shifts the result by 1 ulp, which is enough to
   flip the Taylor tail and break byte parity. So: reduce and fold in double
   (f64f), narrow once. */
const f64f = x => x;   /* JS numbers already are float64 */

function uSin(x) {
  x = f64f(x);
  const n = Math.trunc(x * R_INV_TWO_PI);       /* (int)(x / two_pi) */
  x = x - R_TWO_PI * n;                        /* float64, as in Python */
  const pi = 3.1415927410125732;               /* f32(pi) widened to double */
  if (x > pi / 2) x = pi - x;
  else if (x < -pi / 2) x = -pi - x;
  x = f32(x);                                  /* float(np.float32(x)) */
  const x2 = f32(x * x);
  let term = x, s = x;
  for (const k of [2, 3, 4, 5]) {
    term = f32(-term * x2 / f32((2 * k - 1) * (2 * k - 2)));
    s = f32(s + term);
  }
  return s;
}

/* ── The sRGB transfer function, without any floating-point power ─────────────
   This is the one place where numpy, xtensa newlib and V8 genuinely disagree:
   numpy's float32 `np.power` is a low-precision operation (up to 13 ulp off the
   correctly-rounded result) and no two libms round 2.4 the same way, so the
   2.4 exponent cannot be "ported" — it has to be replaced.

   Both directions are therefore power-free:

   · srgbByteToLinear takes only 256 distinct inputs (byte / 255), so it is an
     exact 256-entry table. Zero error, zero ambiguity.
   · linearToSrgb takes arbitrary floats, so it uses uExp2/uLog2:
         x^(1/2.4) = exp2(log2(x) / 2.4)
     Measured against a correctly-rounded reference the result is within
     2.3e-3 of one 8-bit step — about 2000x below the rounding threshold, so it
     never changes the output byte.

   See docs/RETRO.md §The sRGB transfer function. */

/* SRGB2LIN[k] = f32(k/255) linearized, byte-exact */
const SRGB2LIN_BITS = new Uint32Array([
  0x00000000, 0x399F22B4, 0x3A1F22B4, 0x3A6EB40E, 0x3A9F22B4, 0x3AC6EB61,
  0x3AEEB40E, 0x3B0B3E5D, 0x3B1F22B4, 0x3B33070A, 0x3B46EB61, 0x3B5B518E,
  0x3B70F18F, 0x3B83E1C6, 0x3B8FE616, 0x3B9C87FD, 0x3BA9C9B6, 0x3BB7AD6F,
  0x3BC6354A, 0x3BD56360, 0x3BE539C1, 0x3BF5BA71, 0x3C0373B6, 0x3C0C6153,
  0x3C15A705, 0x3C1F45BE, 0x3C293E6B, 0x3C3391F7, 0x3C3E4149, 0x3C494D44,
  0x3C54B6C9, 0x3C607EB4, 0x3C6CA5DF, 0x3C792D22, 0x3C830AA9, 0x3C89AF9F,
  0x3C9085DC, 0x3C978DC6, 0x3C9EC7C2, 0x3CA63433, 0x3CADD37D, 0x3CB5A602,
  0x3CBDAC21, 0x3CC5E63A, 0x3CCE54AC, 0x3CD6F7D5, 0x3CDFD010, 0x3CE8DDBA,
  0x3CF2212D, 0x3CFB9AC3, 0x3D02A56A, 0x3D0798DD, 0x3D0CA7E6, 0x3D11D2AF,
  0x3D171964, 0x3D1C7C30, 0x3D21FB3C, 0x3D2796B2, 0x3D2D4EBB, 0x3D332381,
  0x3D39152B, 0x3D3F23E4, 0x3D454FD2, 0x3D4B991D, 0x3D51FFEC, 0x3D588468,
  0x3D5F26B6, 0x3D65E6FD, 0x3D6CC563, 0x3D73C20E, 0x3D7ADD24, 0x3D810B65,
  0x3D84B793, 0x3D88732E, 0x3D8C3E48, 0x3D9018F4, 0x3D940344, 0x3D97FD49,
  0x3D9C0715, 0x3DA020BA, 0x3DA44A4A, 0x3DA883D6, 0x3DACCD6F, 0x3DB12727,
  0x3DB5910F, 0x3DBA0B38, 0x3DBE95B3, 0x3DC33090, 0x3DC7DBE0, 0x3DCC97B4,
  0x3DD1641D, 0x3DD6412B, 0x3DDB2EEE, 0x3DE02D76, 0x3DE53CD4, 0x3DEA5D18,
  0x3DEF8E51, 0x3DF4D090, 0x3DFA23E5, 0x3DFF885E, 0x3E027F06, 0x3E05427F,
  0x3E080EA2, 0x3E0AE377, 0x3E0DC104, 0x3E10A753, 0x3E13966A, 0x3E168E51,
  0x3E198F0F, 0x3E1C98AC, 0x3E1FAB30, 0x3E22C6A1, 0x3E25EB07, 0x3E29186A,
  0x3E2C4ED0, 0x3E2F8E42, 0x3E32D6C5, 0x3E362862, 0x3E39831F, 0x3E3CE703,
  0x3E405417, 0x3E43CA60, 0x3E4749E6, 0x3E4AD2AF, 0x3E4E64C3, 0x3E520029,
  0x3E55A4E7, 0x3E595305, 0x3E5D0A89, 0x3E60CB7A, 0x3E6495DF, 0x3E6869BE,
  0x3E6C471F, 0x3E702E07, 0x3E741E7E, 0x3E78188B, 0x3E7C1C33, 0x3E8014BF,
  0x3E822039, 0x3E84308B, 0x3E8645B8, 0x3E885FC3, 0x3E8A7EB0, 0x3E8CA281,
  0x3E8ECB3B, 0x3E90F8DF, 0x3E932B72, 0x3E9562F6, 0x3E979F6F, 0x3E99E0E0,
  0x3E9C274C, 0x3E9E72B6, 0x3EA0C321, 0x3EA31890, 0x3EA57307, 0x3EA7D288,
  0x3EAA3716, 0x3EACA0B6, 0x3EAF0F68, 0x3EB18332, 0x3EB3FC15, 0x3EB67A14,
  0x3EB8FD34, 0x3EBB8576, 0x3EBE12DE, 0x3EC0A56E, 0x3EC33D2A, 0x3EC5DA14,
  0x3EC87C30, 0x3ECB2380, 0x3ECDD008, 0x3ED081CA, 0x3ED338C9, 0x3ED5F508,
  0x3ED8B68A, 0x3EDB7D52, 0x3EDE4963, 0x3EE11ABF, 0x3EE3F169, 0x3EE6CD65,
  0x3EE9AEB5, 0x3EEC955B, 0x3EEF815C, 0x3EF272B8, 0x3EF56974, 0x3EF86593,
  0x3EFB6716, 0x3EFE6E00, 0x3F00BD2B, 0x3F02460C, 0x3F03D1A5, 0x3F055FF7,
  0x3F06F104, 0x3F0884CD, 0x3F0A1B54, 0x3F0BB499, 0x3F0D509F, 0x3F0EEF65,
  0x3F1090EF, 0x3F12353D, 0x3F13DC50, 0x3F15862A, 0x3F1732CC, 0x3F18E237,
  0x3F1A946E, 0x3F1C4970, 0x3F1E0140, 0x3F1FBBDE, 0x3F21794D, 0x3F23398C,
  0x3F24FC9F, 0x3F26C285, 0x3F288B41, 0x3F2A56D2, 0x3F2C253C, 0x3F2DF67F,
  0x3F2FCA9C, 0x3F31A194, 0x3F337B6A, 0x3F35581D, 0x3F3737B0, 0x3F391A24,
  0x3F3AFF7A, 0x3F3CE7B2, 0x3F3ED2CF, 0x3F40C0D2, 0x3F42B1BC, 0x3F44A58E,
  0x3F469C49, 0x3F4895EF, 0x3F4A9280, 0x3F4C91FF, 0x3F4E946C, 0x3F5099C9,
  0x3F52A216, 0x3F54AD56, 0x3F56BB88, 0x3F58CCAF, 0x3F5AE0CC, 0x3F5CF7DF,
  0x3F5F11EA, 0x3F612EEF, 0x3F634EEE, 0x3F6571E9, 0x3F6797E0, 0x3F69C0D5,
  0x3F6BECCA, 0x3F6E1BBF, 0x3F704DB5, 0x3F7282AE, 0x3F74BAAB, 0x3F76F5AE,
  0x3F7933B6, 0x3F7B74C6, 0x3F7DB8DE, 0x3F800000,
]);

const SRGB2LIN = new Float32Array(SRGB2LIN_BITS.buffer);

/* uLog2: x = m * 2^e with m in [1,2), then log2(m) via the odd series in
   t = (m-1)/(m+1), which converges fast and needs only multiplies/divides. */
const R_TWO_OVER_LN2 = 2.8853900817779268;   /* 2 / ln(2), as a double */
function uLog2(x) {
  x = fr(x);
  if (!(x > 0)) return -1e30;                /* callers never pass <= 0 */
  f32buf[0] = x;
  const e = ((u32buf[0] >>> 23) & 0xFF) - 127;
  u32buf[0] = 0x3F800000 | (u32buf[0] & 0x007FFFFF);
  const m = f32buf[0];
  const t = fr(fr(m - 1) / fr(m + 1));
  const t2 = fr(t * t);
  let p = fr(1 / 9);
  p = fr(fr(p * t2) + fr(1 / 7));
  p = fr(fr(p * t2) + fr(1 / 5));
  p = fr(fr(p * t2) + fr(1 / 3));
  p = fr(fr(p * t2) + 1);
  const s = fr(fr(fr(R_TWO_OVER_LN2) * t) * p);
  return fr(fr(e) + s);
}

/* uExp2: 2^y = 2^floor(y) * 2^frac, the fractional part via exp(frac*ln2) */
const R_LN2 = 0.6931471805599453;
function uExp2(y) {
  y = fr(y);
  const k = Math.floor(y);
  const z = fr(fr(y - k) * fr(R_LN2));
  let p = fr(1 / 720);
  p = fr(fr(p * z) + fr(1 / 120));
  p = fr(fr(p * z) + fr(1 / 24));
  p = fr(fr(p * z) + fr(1 / 6));
  p = fr(fr(p * z) + 0.5);
  p = fr(fr(p * z) + 1);
  p = fr(fr(p * z) + 1);
  f32buf[0] = p;                             /* scale by 2^k via the exponent field */
  const biased = k + 127;
  if (biased <= 0 || biased >= 255) return p;
  u32buf[0] = (u32buf[0] & 0x807FFFFF) | (biased << 23);
  return fr(f32buf[0]);
}

const R_INV_24 = fr(1 / 2.4);

/* ── Colour space (float32 throughout) ──
   srgbByteToLinear is a byte table lookup (exact); linearToSrgb uses uLog2/uExp2.
   Neither direction calls Math.pow, so the ports cannot drift — see the block
   above SRGB2LIN_BITS. */
/* srgbByteToLinear(b): b is a byte 0..255 — the only domain this needs */
function srgbByteToLinear(b) {
  return SRGB2LIN[b];
}

function linearToSrgb(c) {
  c = fr(Math.min(1, Math.max(0, c)));
  return c <= fr(0.0031308) ? fr(c * fr(12.92))
                            : fr(fr(fr(1.055) * uExp2(fr(uLog2(c) * R_INV_24))) - fr(0.055));
}

/* colour-space results are handed back through module scratch variables:
   allocating an array per pixel dominated the runtime of a 120k-pixel pass */
let oL = 0, oA = 0, oB = 0;
function rgbToOklabInto(r, g, b) {
  const l = fr(fr(fr(BASE_IDX * r) + fr(BASE_MG * g)) + fr(BASE_SB * b));
  const m = fr(fr(fr(LMS_L * r) + fr(LMS_MG * g)) + fr(LMS_SB * b));
  const s = fr(fr(fr(S_L * r) + fr(S_MG * g)) + fr(S_SB * b));
  const l_ = uCbrt(l), m_ = uCbrt(m), s_ = uCbrt(s);
  oL = fr(fr(fr(O_LL * l_) + fr(O_LM * m_)) + fr(O_LS * s_));
  oA = fr(fr(fr(O_AL * l_) + fr(O_AM * m_)) + fr(O_AS * s_));
  oB = fr(fr(fr(O_BL * l_) + fr(O_BM * m_)) + fr(O_BS * s_));
}

let oR = 0, oG = 0, oBl = 0;
function oklabToRgbInto(L, A, B) {
  const l_ = fr(fr(L + fr(fr(0.3963377774) * A)) + fr(fr(0.2158037573) * B));
  const m_ = fr(fr(L - fr(fr(0.1055613458) * A)) - fr(fr(0.0638541728) * B));
  const s_ = fr(fr(L - fr(fr(0.0894841775) * A)) - fr(fr(1.2914855480) * B));
  const l = fr(fr(l_ * l_) * l_), m = fr(fr(m_ * m_) * m_), s = fr(fr(s_ * s_) * s_);
  oR = fr(fr(fr(fr(4.0767416621) * l) - fr(fr(3.3077115913) * m)) + fr(fr(0.2309699292) * s));
  oG = fr(fr(fr(fr(-1.2684380046) * l) + fr(fr(2.6097574011) * m)) - fr(fr(0.3413193965) * s));
  oBl = fr(fr(fr(fr(-0.0041960863) * l) - fr(fr(0.7034186147) * m)) + fr(fr(1.7076147010) * s));
}

/* Allocating wrappers, kept for the debug/trace paths only. */
function rgbToOklab(r, g, b) { rgbToOklabInto(r, g, b); return [oL, oA, oB]; }
function oklabToRgb(L, A, B) { oklabToRgbInto(L, A, B); return [oR, oG, oBl]; }

/* round half to even, matching numpy.rint / lrintf (Math.round is half-up) */
function uRint(x) {
  const f = Math.floor(x), d = x - f;
  if (d > 0.5) return f + 1;
  if (d < 0.5) return f;
  return (f % 2 === 0) ? f : f + 1;
}

const clamp255 = v => v < 0 ? 0 : v > 255 ? 255 : v;
const toByte = v01 => {
  /* The *255 must stay in float32. Computing it in float64 and rounding that is
     a different function: 0x3F6C6C6C is 0.92352938652038574, which is
     235.49999356269836426 in float64 (rounds to 235) but exactly 235.5 once
     narrowed to float32 (rounds to 236 under half-to-even). Python does
     np.float32(x) * np.float32(255.0) and the firmware does `float * 255.0f`,
     so both land on the float32 value; without fr() here the browser is the odd
     one out and one blue pixel in v6 comes out 1 too low. */
  const v = clamp255(fr(linearToSrgb(Math.min(1, Math.max(0, v01))) * 255));
  return uRint(v);          /* round-half-to-even, matching numpy.rint / lrintf */
};

/* ── Stage 4: stylised preprocessing. rgb: Uint8ClampedArray (W*H*3) ── */
function retroPreprocess(rgb, n) {
  for (let i = 0; i < n; i++) {
    const r = srgbByteToLinear(rgb[i * 3]);
    const g = srgbByteToLinear(rgb[i * 3 + 1]);
    const b = srgbByteToLinear(rgb[i * 3 + 2]);
    rgbToOklabInto(r, g, b);
    let L = oL, A = fr(oA * R_SAT_SCALE), B = fr(oB * R_SAT_SCALE);

    /* Hue is evaluated in float64 and only narrowed once, at the end. Python
       computes np.arctan2(B.astype(np.float64), A.astype(np.float64) + 1e-9) in
       float64 and multiplies by 180/pi in float64 too, so narrowing the atan2
       result to float32 first (which is what fr() around Math.atan2 did) loses
       the low bits of the angle and puts this pixel's hue 1 ulp off. That is
       invisible until a hue lands exactly on the 320 deg branch boundary, where
       it flips which scale A and B get. */
    let hue = (Math.atan2(B, A + 1e-9) * 180 / R_PI) % 360;
    if (hue < 0) hue += 360;
    hue = fr(hue);
    if (hue >= R_COOL_LO && hue <= R_COOL_HI) { A = fr(A * R_COOL_SCALE); B = fr(B * R_COOL_SCALE); }
    else if (hue > R_GREEN_LO && hue < R_GREEN_HI) { A = fr(A * R_GREEN_SCALE); B = fr(B * R_GREEN_SCALE); }

    L = fr(Math.min(1, Math.max(0, L)));
    L = fr(Math.min(1, Math.max(0, fr(L + fr(R_S_CURVE_AMP * uSin(fr(fr(2 * R_PI) * L)))))));

    oklabToRgbInto(L, A, B);
    rgb[i * 3] = toByte(oR);
    rgb[i * 3 + 1] = toByte(oG);
    rgb[i * 3 + 2] = toByte(oBl);
  }
}

/* ── Stage 5: hue-aware candidate mask ── */
function hueMask(r, g, b) {
  rgbToOklabInto(srgbByteToLinear(r), srgbByteToLinear(g), srgbByteToLinear(b));
  const chroma = fr(Math.sqrt(fr(fr(oA * oA) + fr(oB * oB))));
  if (chroma <= R_CHROMA_THRESH) return 0b1111;
  /* Same float64 hue as retroPreprocess above, and the same as Python's
     retro_hue_mask: (atan2 in f64 * 180/pi) % 360, narrowed once at the end. */
  let hue = (Math.atan2(oB, oA + 1e-9) * 180 / R_PI) % 360;
  if (hue < 0) hue += 360;
  hue = fr(hue);
  if (hue < R_GREEN_LO || hue >= 330) return 0b1111;   /* warm */
  if (hue < R_COOL_LO) return 0b1010;                  /* green: black|yellow */
  return 0b0011;                                       /* cool:  black|white */
}

function nearestMasked(L, A, B, pal, mask) {
  let best = 0, bestD = 0, have = false;
  for (let i = 0; i < 4; i++) {
    if (!((mask >> i) & 1)) continue;
    const dL = fr(L - pal.L[i]), dA = fr(A - pal.A[i]), dB = fr(B - pal.B[i]);
    const d = fr(fr(fr(dL * dL) + fr(dA * dA)) + fr(dB * dB));
    if (!have || d < bestD) { bestD = d; best = i; have = true; }
  }
  return best;
}

/* ── Stages 6+7: serpentine Atkinson diffusion + 2bpp packing ──
   rgbIn: Uint8ClampedArray of W*H*3 bytes (already 400x300).
   Returns Uint8Array of 30000. */
function retroQuantize(rgbIn, dither = true) {
  const W = RETRO_W, H = RETRO_H;
  const n = W * H;
  const work = new Uint8ClampedArray(rgbIn);   /* copy: preprocess is in place */
  retroPreprocess(work, n);

  const mask = new Uint8Array(n);
  for (let i = 0; i < n; i++) mask[i] = hueMask(work[i * 3], work[i * 3 + 1], work[i * 3 + 2]);

  /* Palette in OKLab, kept in flat arrays (the inner loops read these a lot) */
  const palL = new Float32Array(4), palA = new Float32Array(4), palB = new Float32Array(4);
  for (let i = 0; i < 4; i++) {
    rgbToOklabInto(srgbByteToLinear(RETRO_PAL[i * 3]),
                   srgbByteToLinear(RETRO_PAL[i * 3 + 1]),
                   srgbByteToLinear(RETRO_PAL[i * 3 + 2]));
    palL[i] = oL; palA[i] = oA; palB[i] = oB;
  }
  const palLab = { L: palL, A: palA, B: palB };

  const out = new Uint8Array(RETRO_FRAME_BYTES);

  if (!dither) {
    for (let i = 0; i < n; i++) {
      rgbToOklabInto(srgbByteToLinear(work[i * 3]),
                     srgbByteToLinear(work[i * 3 + 1]),
                     srgbByteToLinear(work[i * 3 + 2]));
      const c = nearestMasked(oL, oA, oB, palLab, mask[i]);
      out[i >> 2] |= retroPanelCode(c) << (6 - 2 * (i & 3));
    }
    return out;
  }

  const errR = new Int32Array(n), errG = new Int32Array(n), errB = new Int32Array(n);

  for (let y = 0; y < H; y++) {
    const fwd = (y % 2 === 0);
    for (let xi = 0; xi < W; xi++) {
      const x = fwd ? xi : W - 1 - xi, i = y * W + x;

      /* Back out of the 1/16 fixed-point error buffer.
         NO "+8" here. Summing the neighbours' Atkinson error in 1/16 units and
         dividing by 16 IS round-half-to-even on v/16 — that is what
         _div16_round does in the Python reference (rint(v/16.0), and note its
         docstring's "(v+8)//16" is stale, the code never adds 8). Adding a
         half-LSB bias makes this round-half-up while every other rounding step
         in the pipeline is half-to-even, and it changes the result for about
         half of all v — enough to desync the whole serpentine pass from the
         second row onward. */
      const sr = clamp255(uRint(fr(fr(work[i * 3] * 16 + errR[i]) / 16)));
      const sg = clamp255(uRint(fr(fr(work[i * 3 + 1] * 16 + errG[i]) / 16)));
      const sb = clamp255(uRint(fr(fr(work[i * 3 + 2] * 16 + errB[i]) / 16)));

      rgbToOklabInto(srgbByteToLinear(sr),
                     srgbByteToLinear(sg),
                     srgbByteToLinear(sb));
      const c = nearestMasked(oL, oA, oB, palLab, mask[i]);
      out[i >> 2] |= retroPanelCode(c) << (6 - 2 * (i & 3));

      const er = fr(sr - RETRO_PAL[c * 3]);
      const eg = fr(sg - RETRO_PAL[c * 3 + 1]);
      const eb = fr(sb - RETRO_PAL[c * 3 + 2]);
      for (let k = 0; k < 6; k++) {
        const nx = x + (fwd ? R_ATK_DX[k] : -R_ATK_DX[k]);
        const ny = y + R_ATK_DY[k];
        if (nx < 0 || nx >= W || ny >= H) continue;
        const ni = ny * W + nx;
        errR[ni] += uRint(fr(er * R_ATK_WEIGHT_16));
        errG[ni] += uRint(fr(eg * R_ATK_WEIGHT_16));
        errB[ni] += uRint(fr(eb * R_ATK_WEIGHT_16));
      }
    }
  }
  return out;
}

/* Unpack a 2bpp frame back to RGBA via the preview palette (display only) */
function retroFrameToImageData(raw) {
  const img = new ImageData(RETRO_W, RETRO_H);
  for (let i = 0; i < RETRO_W * RETRO_H; i++) {
    const c = (raw[i >> 2] >> (6 - 2 * (i & 3))) & 3;
    const p = RETRO_PAL_VIEW[retroPanelCode(c)], o = i * 4;
    img.data[o] = p[0]; img.data[o + 1] = p[1]; img.data[o + 2] = p[2]; img.data[o + 3] = 255;
  }
  return img;
}

/* Debug helper: dump every intermediate for one pixel so the JS numbers can be
   lined up against the Python reference. Not used by the page. */
function retroTracePixel(rgb, i) {
  const r = srgbByteToLinear(rgb[i * 3] );
  const g = srgbByteToLinear(rgb[i * 3 + 1] );
  const b = srgbByteToLinear(rgb[i * 3 + 2] );
  const lin = [r, g, b];
  let [L, A, B] = rgbToOklab(r, g, b);
  const oklab0 = [L, A, B];
  A = fr(A * R_SAT_SCALE); B = fr(B * R_SAT_SCALE);
  let hue = fr(fr(Math.atan2(B, A + 1e-9)) * fr(180 / R_PI));
  if (hue < 0) hue = fr(hue + 360);
  const hue1 = hue;
  if (hue >= R_COOL_LO && hue <= R_COOL_HI) { A = fr(A * R_COOL_SCALE); B = fr(B * R_COOL_SCALE); }
  else if (hue > R_GREEN_LO && hue < R_GREEN_HI) { A = fr(A * R_GREEN_SCALE); B = fr(B * R_GREEN_SCALE); }
  const hue2 = fr(fr(Math.atan2(B, A + 1e-9)) * fr(180 / R_PI));
  L = fr(Math.min(1, Math.max(0, L)));
  const Lc = L;
  const sArg = fr(fr(2 * R_PI) * L);
  const sinV = uSin(sArg);
  L = fr(Math.min(1, Math.max(0, fr(L + fr(R_S_CURVE_AMP * sinV)))));
  const Lpost = L;
  const rgbLin = oklabToRgb(L, A, B);
  const srgb = [linearToSrgb(Math.min(1, Math.max(0, rgbLin[0]))),
                linearToSrgb(Math.min(1, Math.max(0, rgbLin[1]))),
                linearToSrgb(Math.min(1, Math.max(0, rgbLin[2])))];

  /* Isolated probes, to expose a bad helper independent of the pipeline */
  return {
    i,
    src: [rgb[i * 3], rgb[i * 3 + 1], rgb[i * 3 + 2]],
    lin,
    oklab0,
    hue1, hue2,
    Lc, sArg, sinV, Lpost,
    rgbLin, srgb,
    bytes: [toByte(rgbLin[0]), toByte(rgbLin[1]), toByte(rgbLin[2])],
    probes: {
      cbrt_0_5: uCbrt(0.5),
      sin_1: uSin(1.0),
      sin_pi_4: uSin(fr(R_PI / 4)),
      sin_small: uSin(fr(0.0565486678)),
      srgb2lin_of_1: srgbByteToLinear(255),
      lin2srgb_of_half: linearToSrgb(0.5),
    },
  };
}
