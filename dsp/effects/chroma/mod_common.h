/* Copied into Vintage Chonk on 17 Nov 2026 from chroma-mod/src/mod_common.h
 * (the developer's own Chroma Console effects project). MFE changes, each marked "MFE:": the CHROMA_SR rescale macros CH_T / CH_INC. */
/* Shared helpers for the Chroma Console Mod-bank delays (13 Nov 2026).
 * Audio: 48 kHz, 16-frame planar stereo blocks, signed 24-bit samples in int32 (Q23),
 * L in [0..15], R in [16..31]. The wet path runs at 16-bit resolution (Q15), like Codex's kernels.
 * Rules: integer only, no division or allocation in the audio loop, every product fits in 32 bits. */
#ifndef MOD_COMMON_H
#define MOD_COMMON_H
#include <stdint.h>

enum { MOD_FRAMES = 16, MOD_ONE = 32768 };

/* MFE: the Chroma code is written for 48 kHz; the engine runs at 44.1 kHz. CH_T rescales a sample count
 * (delay length, RT60, wobble depth) and CH_INC a per-sample phase increment, so times and LFO rates stay
 * right. Filter coefficients are NOT rescaled: their corners sit about 8 % lower at 44.1 kHz.
 * -DCHROMA_SR=48000 gives the original code bit for bit (checked against chroma-mod, 17 Nov 2026). */
#ifndef CHROMA_SR
#define CHROMA_SR 44100
#endif
#if CHROMA_SR == 48000
#define CH_T(x) ((uint32_t)(x))
#define CH_INC(x) ((uint32_t)(x))
#else
#define CH_T(x) ((uint32_t)(((uint64_t)(x) * (CHROMA_SR / 300u)) / 160u))    /* x * fs / 48000 */
#define CH_INC(x) ((uint32_t)(((uint64_t)(x) * 160u) / (CHROMA_SR / 300u)))  /* x * 48000 / fs */
#endif

typedef struct {
    uint16_t time;   /* TIME knob, 0..65535 */
    uint16_t amount; /* AMOUNT knob, 0..32768 */
    uint16_t drift;  /* DRIFT secondary, 0..32768 */
    uint16_t hold;   /* footswitch hold (switch still to be chosen), 0 or 1 */
    uint32_t tap;    /* tapped delay in samples; 0 = use TIME */
} ModControls;

static inline int32_t m_sat16(int32_t x) {
    if (x > 32767) return 32767;
    if (x < -32768) return -32768;
    return x;
}

static inline int32_t m_in16(int32_t q23) {
    if (q23 > 8388607) q23 = 8388607;
    if (q23 < -8388608) q23 = -8388608;
    return q23 >> 8;
}

static inline int32_t m_out23(int32_t s16) { return m_sat16(s16) * 256; }

static inline uint32_t m_rand(uint32_t *s) {
    *s = *s * 1664525u + 1013904223u;
    return *s;
}

/* Uniform noise, -32768..32767. */
static inline int32_t m_noise(uint32_t *s) { return (int32_t)(m_rand(s) >> 16) - 32768; }

/* One-pole low-pass. x and *y are 16-bit, a is Q15 (1..32767). */
static inline int32_t m_lp(int32_t *y, int32_t x, int32_t a) {
    *y += ((x - *y) * a + 16384) >> 15;   /* rounded, so no one-sided DC creeps in */
    return *y;
}

/* One-pole low-pass for low cutoffs (a <= 2047) with an extra 8 bits of state, so it has no dead band.
 * *y8 is Q8. Used for every high-pass (x - lowpass) so DC cannot stick. */
static inline int32_t m_lpq(int32_t *y8, int32_t x, int32_t a) {
    int32_t d = (x << 8) - *y8;
    *y8 += ((d >> 4) * a) >> 11;
    return *y8 >> 8;
}

/* Soft knee: slope 1/4 above |k|. */
static inline int32_t m_knee(int32_t x, int32_t k) {
    if (x > k) return k + ((x - k) >> 2);
    if (x < -k) return -k - ((-k - x) >> 2);
    return x;
}

/* Parabolic approximation of MINUS COSINE of a 32-bit phase (phase 0 gives -16384), about +/-16384.
 * Fine for LFOs; offset the phase by 0x40000000 when a true sine/cosine pair matters. Not renamed, to keep existing digests. */
static inline int32_t m_sin(uint32_t phase) {
    int32_t p = (int32_t)(phase >> 16);
    int32_t t = (p < 32768 ? p : 65535 - p) - 16384;
    int32_t a = t < 0 ? -t : t;
    return (t * (32768 - a)) >> 14;
}

/* Linear-interpolated read, delay in Q8 samples (at least 2 samples). */
static inline int32_t m_read(const int16_t *ring, uint32_t mask, uint32_t w, uint32_t delay_q8) {
    uint32_t d = delay_q8 >> 8;
    int32_t f = (int32_t)(delay_q8 & 255u);
    int32_t a = ring[(w - d) & mask];
    int32_t b = ring[(w - d - 1u) & mask];
    return a + (((b - a) * f) >> 8);
}

/* Knob to a range with a squared (audio-taper) curve. hi - lo must stay below 2^20. */
static inline uint32_t m_span(uint32_t lo, uint32_t hi, uint16_t knob) {
    uint32_t k2 = ((uint32_t)knob * knob) >> 16;
    return lo + ((((hi - lo) >> 4) * k2) >> 12);
}

/* Move cur toward target by about 1/2^shift per call, never stalling. */
static inline int32_t m_slew(int32_t cur, int32_t target, int shift) {
    int32_t diff = target - cur;
    int32_t step = diff >> shift;
    if (!step) step = (diff > 0) - (diff < 0);
    return cur + step;
}

/* Peak follower with a linear tail so it reaches exactly zero. */
static inline int32_t m_env(int32_t *env, int32_t x) {
    int32_t a = x < 0 ? -x : x;
    if (a > *env) *env += (a - *env) >> 4;
    else if (*env > 0) *env -= (*env >> 11) + 1;
    if (*env < 0) *env = 0;
    return *env;
}

static inline int m_pow2(uint32_t n) { return n && !(n & (n - 1u)); }

#endif
