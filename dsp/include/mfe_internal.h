/* SPDX-License-Identifier: AGPL-3.0-only
 * Vintage Chonk Engine: shared tables and fixed-point helpers (internal). */
#ifndef MFE_INTERNAL_H
#define MFE_INTERNAL_H
#include "mfe.h"

#define MFE_Q15 32768
#define MFE_CLAMP(x, lo, hi) ((x) < (lo) ? (lo) : (x) > (hi) ? (hi) : (x))

static inline int32_t mfe_mul15(int32_t a, int32_t b) { return (int32_t)(((int64_t)a * b) >> 15); }
static inline int32_t mfe_sat16(int32_t x) { return MFE_CLAMP(x, -32768, 32767); }

/* sine: 1024-point table, Q15, linear interpolation on the top bits of the phase */
extern int16_t mfe_sine_tab[1025];
static inline int32_t mfe_sin(uint32_t ph)
{
    uint32_t i = ph >> 22, f = (ph >> 7) & 0x7FFFu;
    int32_t a = mfe_sine_tab[i], b = mfe_sine_tab[i + 1];
    return a + (((b - a) * (int32_t)f) >> 15);
}

/* pitch in 1/256 semitone (MIDI note << 8) -> phase increment; clamped to notes -48..135 */
uint32_t mfe_pitch_inc(int32_t pitch_q8);
/* filter g = tan(pi f / fs) in Q16 for a cutoff pitch (MIDI note << 8), f clamped below 0.49 fs */
int32_t mfe_cutoff_g(int32_t pitch_q8);
/* envelope/glide time: Q15 param -> control ticks (>= 1) */
uint32_t mfe_time_ticks(int32_t q15);
/* exponential decay factor per control tick for a time param, Q30 */
int32_t mfe_time_coef(int32_t q15);
/* LFO rate: Q15 param -> phase increment per control tick */
uint32_t mfe_lfo_inc(int32_t q15);
/* soft clip: Q15 in (any range) -> Q15 out in about +-1.0 (tanh-like) */
int32_t mfe_softclip(int32_t x);

/* wavetables: [table][frame][level][sample] */
/* Compact layout (16 Nov 2026): level L holds 128 >> L harmonics in max(8, 256 >> L) samples, so a
 * frame is 256+128+64+32+16+8+8+8 = 520 samples instead of 8 x 256. Built at boot (no Flash cost). */
#define MFE_WT_STRIDE 520
static const uint16_t MFE_WT_OFF[MFE_WT_LEVELS] = {0, 256, 384, 448, 480, 496, 504, 512};
static const uint8_t MFE_WT_BITS[MFE_WT_LEVELS] = {8, 7, 6, 5, 4, 3, 3, 3};   /* log2 of each length */
/* MFE_WT_MOOG_ONLY (FM1 build, 17 Nov 2026): only the classic-Moog table is built and stored (8 KB instead
 * of 58 KB of RAM; the FM1 boot-looped with too little heap left for the SDK). Every table index plays it:
 * the developer's oscillators are the Moog ones, and the converter maps every Massive wavetable onto it. */
#ifndef MFE_WT_MOOG_ONLY
#define MFE_WT_MOOG_ONLY 0
#endif
#define MFE_WT_STORE (MFE_WT_MOOG_ONLY ? 1 : MFE_WT_COUNT)
extern int16_t mfe_wt[MFE_WT_STORE][MFE_WT_FRAMES][MFE_WT_STRIDE];
static inline const int16_t *mfe_wt_frame(unsigned t, unsigned f, unsigned L)
{
    return &mfe_wt[MFE_WT_MOOG_ONLY ? 0 : t][f][MFE_WT_OFF[L]];
}
/* band-limit level for a phase increment: the copy whose top harmonic stays under Nyquist */
static inline unsigned mfe_wt_level(uint32_t inc)
{
    /* level L holds 128 >> L harmonics; need (128 >> L) * inc < 2^31 */
    unsigned L = 0;
    uint64_t top = (uint64_t)inc * 128u;
    while (L < MFE_WT_LEVELS - 1 && top >= 0x80000000ull) { top >>= 1; L++; }
    return L;
}

/* random: xorshift32 */
static inline uint32_t mfe_rand(uint32_t *s) { uint32_t x = *s; x ^= x << 13; x ^= x >> 17; x ^= x << 5; return *s = x; }

/* ---- filter (TPT state-variable, Simper), coefficients Q30, signal Q15 in int32 ---- */
typedef struct { int32_t ic1, ic2; } mfe_svf_state_t;
typedef struct { int32_t a1, a2, a3, k; uint8_t type; } mfe_svf_coef_t;
void mfe_svf_coef(mfe_svf_coef_t *c, uint8_t type, int32_t cutoff_pitch_q8, int32_t res_q15);
int32_t mfe_svf_tick(mfe_svf_state_t *s, const mfe_svf_coef_t *c, int32_t x);

/* ---- ladder (4 TPT one-pole stages, feedback from the last output, tanh-like input stage) ---- */
typedef struct { int32_t s[4], y4, hp; } mfe_ladder_state_t;
typedef struct { int32_t G, G4, Ghp, k; uint8_t acid; } mfe_ladder_coef_t;  /* G = g/(1+g) Q30, k feedback Q15 */
void mfe_acid_coef(mfe_ladder_coef_t *c, int32_t cutoff_pitch_q8, int32_t res_q15);
void mfe_ladder_coef(mfe_ladder_coef_t *c, int32_t cutoff_pitch_q8, int32_t res_q15);
int32_t mfe_ladder_tick(mfe_ladder_state_t *s, const mfe_ladder_coef_t *c, int32_t x);

/* ---- envelope (DAHDSR-like: delay, attack, decay, sustain, release; optional loop) ---- */
typedef struct { int32_t level; uint32_t count; uint8_t stage; } mfe_env_state_t;   /* level Q30 */
enum { MFE_ES_OFF, MFE_ES_DELAY, MFE_ES_ATTACK, MFE_ES_DECAY, MFE_ES_SUSTAIN, MFE_ES_RELEASE, MFE_ES_SUSMOVE };
void mfe_env_gate(mfe_env_state_t *s, const mfe_env_t *p, int on, int legato);
int32_t mfe_env_tick(mfe_env_state_t *s, const mfe_env_t *p);   /* returns Q15 */

/* ---- LFO ---- */
typedef struct { uint32_t ph; int32_t sh; uint32_t rng; uint8_t step; int32_t val; } mfe_lfo_state_t;
void mfe_lfo_reset(mfe_lfo_state_t *s, const mfe_lfo_t *p);
/* sync_inc: phase increment per control tick for a synced LFO (0 = use the rate) */
int32_t mfe_lfo_tick(mfe_lfo_state_t *s, const mfe_lfo_t *p, int32_t rate_mod_q15, uint32_t sync_inc);   /* Q15 */
/* Performer shapes: 64 points each over a step, Q15 0..32767 (built at boot) */
extern int16_t mfe_perf_tab[MFE_PERF_SHAPES][65];
static inline int32_t mfe_perf_shape(unsigned id, uint32_t x /* 0..65535 across the step */)
{
    const int16_t *t = mfe_perf_tab[id < MFE_PERF_SHAPES ? id : 0];
    uint32_t i = x >> 10, f = x & 1023u;
    return t[i] + (((int32_t)(t[i + 1] - t[i]) * (int32_t)f) >> 10);
}

/* ---- effects ---- */
typedef struct mfe_fx_state mfe_fx_state_t;
size_t mfe_fx_size(void);
void mfe_fx_init(mfe_fx_state_t *f);
void mfe_fx_set_tempo(mfe_fx_state_t *f, uint32_t bpm_q8);   /* synced BBD ECHO / OIL CAN (from mfe_set_tempo) */
void mfe_fx_process(mfe_fx_state_t *f, const mfe_fx_t *p, int32_t *lr, unsigned frames);

#endif
