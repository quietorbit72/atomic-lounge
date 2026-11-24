/* Copied into Vintage Chonk on 17 Nov 2026 from chroma-mod/fx/texture_reverbs/_tx_common.h
 * (the developer's own Chroma Console effects project). MFE changes, each marked "MFE:": none (verbatim). */
/* Texture-module reverbs 141-145: shared helpers (17 Nov 2026).
 * Texture (stage 3, last in the chain) processes the incoming STEREO block in place and owns its dry/wet,
 * so these effects return dry + wet. The pedal gives Texture one knob: AMOUNT (k2). k1/k3 are unused. */
#ifndef TX_COMMON_H
#define TX_COMMON_H
#include "fx_api.h"

/* Q14 product truncated toward zero: filter states cannot creep or sit on a limit cycle. */
static inline int32_t tx_mz14(int32_t a, int32_t b) { int32_t p = a * b; return p >= 0 ? p >> 14 : -((-p) >> 14); }

/* Low-frequency resonator (band-pass) with 6 extra state bits (from fx/rnr/021_water_tank.c lowres):
 * f <= 511 (Q14, 2 sin(pi fc / fs)), q = 1/Q (Q14). Snaps to exact zero once silent. */
static inline int32_t tx_lowres(int32_t *lp, int32_t *bp, int32_t x, int32_t f, int32_t q) {
    if (!x && (uint32_t)(*bp + 1024) < 2048u && (uint32_t)(*lp + 1024) < 2048u) { *lp = *bp = 0; return 0; }
    *lp += tx_mz14(f, *bp);
    if (*lp > 4000000) *lp = 4000000;
    if (*lp < -4000000) *lp = -4000000;
    int32_t hp = (x << 6) - *lp - (tx_mz14(q, *bp >> 6) << 6);
    if (hp > 4000000) hp = 4000000;
    if (hp < -4000000) hp = -4000000;
    *bp += tx_mz14(f, hp);
    if (*bp > 4000000) *bp = 4000000;
    if (*bp < -4000000) *bp = -4000000;
    return *bp >> 6;
}

/* Mid/high band-pass state-variable filter, 16-bit states, f and q Q14 (f up to ~12000). Snaps to zero. */
static inline int32_t tx_svf_bp(int32_t *lp, int32_t *bp, int32_t x, int32_t f, int32_t q) {
    if (!x && (uint32_t)(*bp + 4) < 8u && (uint32_t)(*lp + 4) < 8u) { *lp = *bp = 0; return 0; }
    *lp += tx_mz14(f, *bp);
    int32_t hp = x - *lp - tx_mz14(q, *bp);
    hp = hp > 98303 ? 98303 : (hp < -98304 ? -98304 : hp);
    *bp += tx_mz14(f, hp);
    if (*bp > 98303) *bp = 98303;
    if (*bp < -98304) *bp = -98304;
    return *bp;
}

/* dry (Q23, untouched precision) + wet (16-bit) x mix (Q15). mix 0 returns dry bit for bit. */
static inline int32_t tx_add(int32_t dry23, int32_t wet16, int32_t mix) {
    if (!mix) return dry23;
    int32_t y = dry23 + ((m_sat16(wet16) * mix) >> 15) * 256;
    return y > 8388607 ? 8388607 : (y < -8388608 ? -8388608 : y);
}

/* AMOUNT -> wet mix with an equal-loudness-ish curve: 0 at 0, ~0.5 at half, full at the top. */
static inline int32_t tx_mix_curve(uint16_t k) {
    uint32_t a = k >> 1;                         /* 0..32767 */
    return (int32_t)((a * (98304u - a)) >> 16);  /* a (3 - a)/2 in Q15 -> 0 .. 32767 */
}
#endif
