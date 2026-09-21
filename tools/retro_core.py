# -*- coding: utf-8 -*-
"""
retro_core.py — RETRO quantization profile (reference implementation)
=====================================================================
This is the authoritative Python port of the RETRO profile. The firmware
(src/display/retro_quantize.cpp) and the browser page (web/uploader.html)
must reproduce it byte-for-byte.

RETRO in one sentence: OKLab perceptual palette matching on a panel-calibrated
warm palette, with hue-aware candidate masking (cool hues dither black/white
only) and serpentine Atkinson error diffusion.

Three-port parity constraints (see docs/RETRO.md):
  · all intermediates are float32 — never float64
  · transcendental functions use the local u_cbrt() / u_sin() so that C++,
    JS and Python share one implementation instead of the platform libm
  · operation order is fixed; no FMA contraction, no fast-math
  · rounding is round-half-to-even everywhere

Sharpening (UnsharpMask) from the original spec is intentionally DISABLED in
v1: the test vectors were generated without it, and a per-platform Gaussian
kernel would break byte parity.

Pure-Python pixel loops here are the *reference*, not the production path —
the browser and the device do the real work. Expect ~40 s per 400x300 image.
"""

import numpy as np

W, H = 400, 300
FRAME_BYTES = W * H // 4          # 30000

# ─── Internal palette — 0=black 1=white 2=red 3=yellow ─────────────────────
# NOTE: this differs from the panel wire order (black/white/yellow/red).
# The packer maps the two chromatic RETRO indices to the fixed panel codes.
RETRO_PAL = np.array([
    [ 35.0,  31.0,  32.0],   # 0 black  — panel black, not pure #000
    [233.0, 231.0, 222.0],   # 1 white  — E Ink warm white
    [193.0,  80.0,  62.0],   # 2 red    — brick red
    [229.0, 197.0,  79.0],   # 3 yellow — ochre
], dtype=np.float32)

# Preview-only palette (simulated panel tone; never participates in packing)
RETRO_PAL_VIEW = np.array([
    [ 20,  20,  20],
    [245, 243, 235],
    [185,  45,  40],
    [225, 190,  25],
], dtype=np.uint8)


def panel_code(palette_index: int) -> int:
    """Map RETRO's internal red/yellow ordering to the panel's 2bpp codes."""
    return palette_index if palette_index < 2 else palette_index ^ 1


RETRO_PANEL_PAL_VIEW = RETRO_PAL_VIEW[[0, 1, 3, 2]]

# ─── Tunables (single source of truth — keep in sync with docs/RETRO.md) ────
SAT_SCALE     = np.float32(0.82)     # OKLab a/b desaturation
COOL_SCALE    = np.float32(0.30)     # chroma convergence, cool hues
GREEN_SCALE   = np.float32(0.85)     # chroma convergence, green hues
S_CURVE_AMP   = np.float32(0.045)    # dynamic-range compression on L
CHROMA_THRESH = np.float32(0.02)     # below this a pixel counts as near-grey
COOL_LO, COOL_HI   = np.float32(150.0), np.float32(320.0)
GREEN_LO, GREEN_HI = np.float32(70.0),  np.float32(150.0)
PI_F = np.float32(3.14159265358979)

# Constants for the shared log2/exp2 pair that replaces pow: see
# docs/RETRO.md §The sRGB transfer function. R_TWO_OVER_LN2 and R_LN2 are kept
# as doubles because they only seed the float32 polynomial.
R_TWO_OVER_LN2 = np.float32(2.0 / 0.6931471805599453)
R_LN2 = np.float32(0.6931471805599453)
R_INV_24 = np.float32(1.0 / 2.4)

# Atkinson kernel: 6 neighbours, each 1/8 → 75% of the error is propagated and
# 25% is deliberately discarded, which is what produces the finer film grain.
ATKINSON = [(1, 0), (2, 0), (-1, 1), (0, 1), (1, 1), (0, 2)]
ATK_WEIGHT_16 = np.int32(2)          # 1/8 expressed in the 1/16 error unit


# ═══════════════════════════════════════════════════════════════════════════
# Transcendental helpers — C++/JS must port these line by line.
# ═══════════════════════════════════════════════════════════════════════════
def u_cbrt_scalar(x: float) -> float:
    """Unified cube root: bit trick seed + 2 Newton iterations, all in float32."""
    x = float(np.float32(x))
    if x == 0.0:
        return 0.0
    sign = -1.0 if x < 0.0 else 1.0
    ax = abs(x)
    import struct
    bits = struct.unpack("<I", struct.pack("<f", ax))[0]
    bits = (bits // 3) + 709921077
    y = struct.unpack("<f", struct.pack("<I", bits & 0xFFFFFFFF))[0]
    y = float(np.float32(y))
    for _ in range(2):
        y3 = float(np.float32(np.float32(y * y) * y))
        num = float(np.float32(np.float32(2.0 * ax) + y3))
        den = float(np.float32(ax + np.float32(2.0 * y3)))
        y = float(np.float32(np.float32(y * num) / den))
    return sign * y


def u_sin_scalar(x: float) -> float:
    """Unified sine: range reduction to [-pi/4, pi/4] + Taylor to x^9."""
    x = float(np.float32(x))
    two_pi = float(np.float32(2.0 * PI_F))
    x = x - two_pi * float(np.float32(int(x / two_pi)))
    pi = float(PI_F)
    if x > pi / 2:
        x = pi - x
    elif x < -pi / 2:
        x = -pi - x
    x2 = float(np.float32(x * x))
    term = x
    s = x
    for k in [2, 3, 4, 5]:
        term = float(np.float32(-term * x2 / float(np.float32((2 * k - 1) * (2 * k - 2)))))
        s = float(np.float32(s + term))
    return s


def _cbrt_array(x: np.ndarray) -> np.ndarray:
    return np.vectorize(u_cbrt_scalar, otypes=[np.float32])(x).astype(np.float32)


def u_log2_scalar(x: float) -> float:
    """float32 log2: exponent from the bit pattern, odd series in t=(m-1)/(m+1)."""
    xf = np.float32(x)
    if not (xf > np.float32(0.0)):
        return -1e30
    bits = int(np.frombuffer(xf.tobytes(), dtype=np.uint32)[0])
    e = ((bits >> 23) & 0xFF) - 127
    m = np.frombuffer(np.uint32(0x3F800000 | (bits & 0x007FFFFF)).tobytes(),
                      dtype=np.float32)[0]
    t = np.float32(np.float32(m - np.float32(1)) / np.float32(m + np.float32(1)))
    t2 = np.float32(t * t)
    p = np.float32(1.0 / 9.0)
    for c in (1.0 / 7.0, 1.0 / 5.0, 1.0 / 3.0, 1.0):
        p = np.float32(np.float32(p * t2) + np.float32(c))
    s = np.float32(np.float32(R_TWO_OVER_LN2 * t) * p)
    return float(np.float32(np.float32(e) + s))


def u_exp2_scalar(y: float) -> float:
    """float32 2**y: integer part straight into the exponent field."""
    yf = np.float32(y)
    k = np.float32(np.floor(yf))
    z = np.float32(np.float32(yf - k) * R_LN2)
    p = np.float32(1.0 / 720.0)
    for c in (1.0 / 120.0, 1.0 / 24.0, 1.0 / 6.0, 0.5, 1.0, 1.0):
        p = np.float32(np.float32(p * z) + np.float32(c))
    biased = int(k) + 127
    if biased <= 0 or biased >= 255:
        return float(p)
    bits = int(np.frombuffer(p.tobytes(), dtype=np.uint32)[0])
    bits = (bits & 0x807FFFFF) | (biased << 23)
    return float(np.frombuffer(np.uint32(bits).tobytes(), dtype=np.float32)[0])


def u_linear_to_srgb_scalar(c: float) -> float:
    """float32 linear -> sRGB for one channel; mirrors the C++/JS ports exactly."""
    cf = np.float32(c)
    if cf <= np.float32(0.0031308):
        return float(np.float32(cf * np.float32(12.92)))
    lg = np.float32(u_log2_scalar(float(cf)) * R_INV_24)
    return float(np.float32(np.float32(np.float32(1.055) *
                                       np.float32(u_exp2_scalar(float(lg)))) -
                             np.float32(0.055)))


def _sin_array(x: np.ndarray) -> np.ndarray:
    return np.vectorize(u_sin_scalar, otypes=[np.float32])(x).astype(np.float32)


# ═══════════════════════════════════════════════════════════════════════════
# Colour space (float32 throughout)
# ═══════════════════════════════════════════════════════════════════════════
SRGB2LIN = np.array([
    0.0, 0.0003035269910469651, 0.0006070539820939302, 0.0009105809731408954,
    0.0012141079641878605, 0.0015176349552348256, 0.0018211619462817907, 0.002124688820913434,
    0.002428215928375721, 0.0027317428030073643, 0.0030352699104696512, 0.0033465358428657055,
    0.0036765073891729116, 0.004024717025458813, 0.0043914420530200005, 0.004776953253895044,
    0.005181516520678997, 0.005605391692370176, 0.006048833020031452, 0.006512090563774109,
    0.006995410192757845, 0.007499032188206911, 0.00802319310605526, 0.008568125776946545,
    0.009134058840572834, 0.009721217676997185, 0.01032982300966978, 0.010960093699395657,
    0.011612244881689548, 0.012286487966775894, 0.012983032502233982, 0.013702083379030228,
    0.014443843625485897, 0.015208514407277107, 0.01599629409611225, 0.016807375475764275,
    0.017641954123973846, 0.01850022003054619, 0.019382361322641373, 0.020288562402129173,
    0.021219009533524513, 0.022173885256052017, 0.023153366521000862, 0.024157632142305374,
    0.02518685907125473, 0.026241222396492958, 0.027320891618728638, 0.028426039963960648,
    0.02955683507025242, 0.03071344457566738, 0.03189603239297867, 0.03310476616024971,
    0.03433980792760849, 0.03560131415724754, 0.03688944876194, 0.0382043719291687,
    0.039546236395835876, 0.040915198624134064, 0.0423114113509655, 0.04373503103852272,
    0.045186202973127365, 0.04666508734226227, 0.04817182570695877, 0.04970656707882881,
    0.05126945674419403, 0.052860647439956665, 0.05448027700185776, 0.05612849071621895,
    0.05780543014407158, 0.05951123684644699, 0.061246052384376526, 0.06301001459360123,
    0.06480326503515244, 0.0666259378194809, 0.06847816705703735, 0.07036009430885315,
    0.07227185368537903, 0.07421357184648514, 0.07618538290262222, 0.07818742096424103,
    0.0802198201417923, 0.08228270709514618, 0.08437620848417282, 0.08650045841932297,
    0.08865558356046677, 0.09084171056747437, 0.09305896610021591, 0.09530746936798096,
    0.09758734703063965, 0.09989872574806213, 0.10224173218011856, 0.10461648553609848,
    0.10702310502529144, 0.109461709856987, 0.1119324266910553, 0.11443537473678589,
    0.11697066575288773, 0.11953842639923096, 0.12213877588510513, 0.12477181851863861,
    0.12743768095970154, 0.13013647496700287, 0.13286831974983215, 0.13563333451747894,
    0.1384316086769104, 0.14126329123973846, 0.1441284716129303, 0.14702726900577545,
    0.14995978772640228, 0.15292614698410034, 0.15592646598815918, 0.15896083414554596,
    0.16202937066555023, 0.16513219475746155, 0.16826939582824707, 0.17144110798835754,
    0.17464740574359894, 0.177888423204422, 0.18116424977779388, 0.18447498977184296,
    0.18782077729701996, 0.19120168685913086, 0.1946178376674652, 0.19806931912899017,
    0.2015562504529953, 0.20507873594760895, 0.20863686501979828, 0.21223075687885284,
    0.2158605009317398, 0.21952620148658752, 0.22322796285152435, 0.22696587443351746,
    0.23074005544185638, 0.2345505803823471, 0.23839756846427917, 0.24228112399578094,
    0.2462013214826584, 0.25015828013420105, 0.2541520893573761, 0.2581828534603119,
    0.2622506618499756, 0.26635560393333435, 0.27049779891967773, 0.2746773064136505,
    0.2788942754268646, 0.28314873576164246, 0.28744083642959595, 0.2917706370353699,
    0.2961382567882538, 0.30054378509521484, 0.3049873113632202, 0.30946892499923706,
    0.31398871541023254, 0.31854677200317383, 0.32314321398735046, 0.3277781009674072,
    0.3324515223503113, 0.33716362714767456, 0.34191441535949707, 0.34670406579971313,
    0.35153260827064514, 0.35640013217926025, 0.3613067865371704, 0.366252601146698,
    0.37123769521713257, 0.3762621283531189, 0.38132601976394653, 0.38642942905426025,
    0.3915724754333496, 0.3967552185058594, 0.4019777774810791, 0.40724021196365356,
    0.4125426113605499, 0.41788506507873535, 0.423267662525177, 0.42869049310684204,
    0.43415364623069763, 0.43965718150138855, 0.44520118832588196, 0.4507857859134674,
    0.4564110338687897, 0.46207699179649353, 0.4677838087081909, 0.47353148460388184,
    0.4793201684951782, 0.48514994978904724, 0.4910208582878113, 0.4969329833984375,
    0.5028864741325378, 0.5088813304901123, 0.5149176716804504, 0.520995557308197,
    0.5271151065826416, 0.533276379108429, 0.5394794940948486, 0.5457244515419006,
    0.5520114302635193, 0.5583403706550598, 0.5647115111351013, 0.5711248517036438,
    0.577580451965332, 0.5840784311294556, 0.5906188488006592, 0.5972017645835876,
    0.6038273572921753, 0.6104955673217773, 0.6172065734863281, 0.6239603757858276,
    0.6307571530342102, 0.637596845626831, 0.6444796919822693, 0.6514056324958801,
    0.6583748459815979, 0.6653872728347778, 0.672443151473999, 0.6795424818992615,
    0.68668532371521, 0.6938717365264893, 0.7011018991470337, 0.7083757519721985,
    0.715693473815918, 0.7230551242828369, 0.7304607629776001, 0.7379103899002075,
    0.7454041838645935, 0.7529422044754028, 0.7605245113372803, 0.7681511640548706,
    0.7758222222328186, 0.7835378050804138, 0.7912979125976562, 0.7991027235984802,
    0.8069522380828857, 0.8148465752601624, 0.8227857351303101, 0.8307698965072632,
    0.838798999786377, 0.8468732237815857, 0.8549926280975342, 0.8631572127342224,
    0.8713670969009399, 0.8796223998069763, 0.8879231214523315, 0.8962693810462952,
    0.9046611785888672, 0.9130986332893372, 0.9215818643569946, 0.9301108717918396,
    0.9386857151985168, 0.9473065137863159, 0.9559733271598816, 0.9646862745285034,
    0.9734452962875366, 0.9822505712509155, 0.9911020994186401, 1.0,
], dtype=np.float32)

def srgb_byte_to_linear(b) -> np.ndarray:
    """Byte-indexed sRGB -> linear. Keeps the float32 table as the single source."""
    return SRGB2LIN[np.asarray(b, dtype=np.uint8)]


def linear_to_srgb(c: np.ndarray) -> np.ndarray:
    """float32, elementwise through the shared u_log2/u_exp2 pair.

    Not vectorised via np.power on purpose: numpy's float32 pow is not correctly
    rounded and drifts up to 13 ulp from the float64 result, which is enough to
    flip a quantisation decision. The firmware and the browser use the same two
    helpers, so this has to stay a scalar loop to match them bit for bit.
    """
    arr = np.asarray(c, dtype=np.float32)
    flat = np.clip(arr, np.float32(0.0), np.float32(1.0)).reshape(-1)
    out = np.empty(flat.size, dtype=np.float32)
    for i, v in enumerate(flat):
        out[i] = u_linear_to_srgb_scalar(float(v))
    return out.reshape(arr.shape)


def rgb_to_oklab(rgb01: np.ndarray):
    """rgb01: float32 (...,3) in [0,1] -> (L, A, B) float32"""
    lin = srgb_byte_to_linear(np.rint(np.asarray(rgb01, dtype=np.float32)
                                       * np.float32(255.0)).astype(np.uint8))
    r, g, b = lin[..., 0], lin[..., 1], lin[..., 2]
    l = (np.float32(0.4122214708) * r + np.float32(0.5363325363) * g
         + np.float32(0.0514459929) * b).astype(np.float32)
    m = (np.float32(0.2119034982) * r + np.float32(0.6806995451) * g
         + np.float32(0.1073969566) * b).astype(np.float32)
    s = (np.float32(0.0883024619) * r + np.float32(0.2817188376) * g
         + np.float32(0.6299787005) * b).astype(np.float32)
    l, m, s = _cbrt_array(l), _cbrt_array(m), _cbrt_array(s)
    L = (np.float32(0.2104542553) * l + np.float32(0.7936177850) * m
         - np.float32(0.0040720468) * s).astype(np.float32)
    A = (np.float32(1.9779984951) * l - np.float32(2.4285922050) * m
         + np.float32(0.4505937099) * s).astype(np.float32)
    B = (np.float32(0.0259040371) * l + np.float32(0.7827717662) * m
         - np.float32(0.8086757660) * s).astype(np.float32)
    return L, A, B


def oklab_to_rgb(L, A, B):
    l_ = (L + np.float32(0.3963377774) * A + np.float32(0.2158037573) * B).astype(np.float32)
    m_ = (L - np.float32(0.1055613458) * A - np.float32(0.0638541728) * B).astype(np.float32)
    s_ = (L - np.float32(0.0894841775) * A - np.float32(1.2914855480) * B).astype(np.float32)
    # cubes by repeated multiplication, not pow() — powf differs across ports
    l = (l_ * l_ * l_).astype(np.float32)
    m = (m_ * m_ * m_).astype(np.float32)
    s = (s_ * s_ * s_).astype(np.float32)
    r = (np.float32(4.0767416621) * l - np.float32(3.3077115913) * m
         + np.float32(0.2309699292) * s).astype(np.float32)
    g = (np.float32(-1.2684380046) * l + np.float32(2.6097574011) * m
         - np.float32(0.3413193965) * s).astype(np.float32)
    b = (np.float32(-0.0041960863) * l - np.float32(0.7034186147) * m
         + np.float32(1.7076147010) * s).astype(np.float32)
    return r, g, b


# ═══════════════════════════════════════════════════════════════════════════
# Stage 4 — stylised preprocessing
# ═══════════════════════════════════════════════════════════════════════════
def retro_preprocess(rgb_u8: np.ndarray) -> np.ndarray:
    """uint8 (H,W,3) -> uint8 (H,W,3), desaturated / tone-mapped, in OKLab."""
    n01 = (rgb_u8.astype(np.float32) / np.float32(255.0)).astype(np.float32)
    L, A, B = rgb_to_oklab(n01)

    A = (A * SAT_SCALE).astype(np.float32)
    B = (B * SAT_SCALE).astype(np.float32)

    hue = (np.arctan2(B.astype(np.float64), A.astype(np.float64) + 1e-9)
           * 180.0 / 3.14159265358979)
    hue = (hue % 360.0).astype(np.float32)
    cold = (hue >= COOL_LO) & (hue <= COOL_HI)
    green = (hue > GREEN_LO) & (hue < GREEN_HI)
    A = np.where(cold, A * COOL_SCALE,
                 np.where(green, A * GREEN_SCALE, A)).astype(np.float32)
    B = np.where(cold, B * COOL_SCALE,
                 np.where(green, B * GREEN_SCALE, B)).astype(np.float32)

    L = np.clip(L, np.float32(0.0), np.float32(1.0)).astype(np.float32)
    L = np.clip(L + S_CURVE_AMP * _sin_array(
        (np.float32(2.0) * PI_F * L).astype(np.float32)),
        np.float32(0.0), np.float32(1.0)).astype(np.float32)

    r, g, b = oklab_to_rgb(L, A, B)
    r = linear_to_srgb(np.clip(r, 0, 1))
    g = linear_to_srgb(np.clip(g, 0, 1))
    b = linear_to_srgb(np.clip(b, 0, 1))
    out = (np.stack([r, g, b], axis=-1) * np.float32(255.0)).astype(np.float32)
    return np.clip(out, 0, 255).astype(np.uint8)


# ═══════════════════════════════════════════════════════════════════════════
# Stage 5 — hue-aware candidate mask
# ═══════════════════════════════════════════════════════════════════════════
def retro_hue_mask(rgb_u8: np.ndarray) -> np.ndarray:
    """Preprocessed uint8 (H,W,3) -> uint8 (H,W) mask, bit0..3 = black/white/red/yellow."""
    n01 = (rgb_u8.astype(np.float32) / np.float32(255.0)).astype(np.float32)
    L, A, B = rgb_to_oklab(n01)
    chroma = np.sqrt((A * A + B * B).astype(np.float32)).astype(np.float32)
    hue = (np.arctan2(B.astype(np.float64), A.astype(np.float64) + 1e-9)
           * 180.0 / 3.14159265358979)
    hue = (hue % 360.0).astype(np.float32)

    warm = (hue < GREEN_LO) | (hue >= np.float32(330.0))
    green = (hue >= GREEN_LO) & (hue < COOL_LO)

    mask = np.full((H, W), 0b1111, dtype=np.uint8)
    mask = np.where(green, np.uint8(0b1010), mask)            # green: black|yellow
    mask = np.where(~warm & ~green, np.uint8(0b0011), mask)   # cool:  black|white
    mask = np.where(chroma <= CHROMA_THRESH, np.uint8(0b1111), mask)
    return mask.astype(np.uint8)


# ═══════════════════════════════════════════════════════════════════════════
# Stages 6+7 — serpentine Atkinson error diffusion + 2bpp packing
# ═══════════════════════════════════════════════════════════════════════════
def _nearest_masked(L, A, B, pal_lab, mask) -> int:
    """Nearest palette entry in OKLab, restricted to the mask's candidates."""
    best, bd = 0, None
    for i in range(4):
        if not ((mask >> i) & 1):
            continue
        dL = L - pal_lab[i][0]
        dA = A - pal_lab[i][1]
        dB = B - pal_lab[i][2]
        d = dL * dL + dA * dA + dB * dB
        if bd is None or d < bd:
            best, bd = i, d
    return best


def _div16_round(v: np.ndarray) -> np.ndarray:
    """(v/16) rounded half-to-even, clamped to [0,255]."""
    v = v.astype(np.int32)
    r = np.rint(v.astype(np.float64) / 16.0).astype(np.int32)
    return np.clip(r, 0, 255)


def _rint32(v: np.ndarray) -> np.ndarray:
    return np.rint(v.astype(np.float64)).astype(np.int32)


def retro_quantize(rgb_u8: np.ndarray, dither: bool = True) -> bytes:
    """rgb_u8: uint8 (H,W,3), must already be 400x300. Returns 30000 bytes."""
    assert rgb_u8.shape == (H, W, 3), f"expect {(H, W, 3)}, got {rgb_u8.shape}"

    work = retro_preprocess(rgb_u8)
    mask = retro_hue_mask(work)

    pal_lab = []
    for i in range(4):
        c = (RETRO_PAL[i] / np.float32(255.0)).astype(np.float32)
        L, A, B = rgb_to_oklab(c)
        pal_lab.append([float(L), float(A), float(B)])

    out = bytearray(FRAME_BYTES)

    if not dither:
        n01 = (work.astype(np.float32) / np.float32(255.0)).astype(np.float32)
        L, A, B = rgb_to_oklab(n01)
        for y in range(H):
            for x in range(W):
                i = y * W + x
                c = _nearest_masked(float(L[y, x]), float(A[y, x]), float(B[y, x]),
                                    pal_lab, int(mask[y, x]))
                out[i >> 2] |= panel_code(c) << (6 - 2 * (i & 3))
        return bytes(out)

    err = np.zeros((H, W, 3), dtype=np.int32)

    for y in range(H):
        fwd = (y % 2 == 0)
        xs = range(W) if fwd else range(W - 1, -1, -1)
        for x in xs:
            i = y * W + x
            cur = work[y, x].astype(np.int32)
            e16 = err[y, x]
            v = _div16_round(cur * 16 + e16)
            vr, vg, vb = float(v[0]), float(v[1]), float(v[2])
            n01 = np.array([vr, vg, vb], dtype=np.float32) / np.float32(255.0)
            L, A, B = rgb_to_oklab(n01)
            c = _nearest_masked(float(L), float(A), float(B),
                                pal_lab, int(mask[y, x]))
            out[i >> 2] |= panel_code(c) << (6 - 2 * (i & 3))
            e0 = np.array([vr, vg, vb], dtype=np.float32) - RETRO_PAL[c]
            for dx, dy in ATKINSON:
                nx = x + (dx if fwd else -dx)
                ny = y + dy
                if 0 <= nx < W and 0 <= ny < H:
                    err[ny, nx] += _rint32(e0 * np.float32(ATK_WEIGHT_16))
    return bytes(out)
