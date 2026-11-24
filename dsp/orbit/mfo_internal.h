/* SPDX-License-Identifier: AGPL-3.0-only
 * Vintage Chonk ORBIT: internal helpers shared by the dsp/orbit sources */
#ifndef MFO_INTERNAL_H
#define MFO_INTERNAL_H
#include <math.h>
#include <stdint.h>
#include <string.h>
#include "mfo.h"

#ifndef MFO_PI
#define MFO_PI 3.14159265358979323846f
#endif
#define MFO_TWO_PI 6.28318530717958647692f

static inline float mfo_clampf(float x, float lo, float hi) { return x < lo ? lo : x > hi ? hi : x; }
static inline float mfo_lerp(float a, float b, float t) { return a + (b - a) * t; }

/* fast log2 / exp2 (relative error below 2e-5), for spectral envelopes. x > 0, normal floats. */
static inline float mfo_log2f(float x)
{
    union { float f; uint32_t i; } v = { x };
    float e = (float)(int)((v.i >> 23) & 255) - 127.0f;
    v.i = (v.i & 0x007FFFFFu) | 0x3F800000u;          /* mantissa in [1,2) */
    float m = v.f;
    if (m > 1.41421356f) { m *= 0.5f; e += 1.0f; }    /* m in [0.707, 1.414) */
    float t = (m - 1.0f) / (m + 1.0f), t2 = t * t;
    return e + t * (2.88539008f + t2 * (0.96179669f + t2 * (0.57707802f + t2 * 0.41219858f)));
}
static inline float mfo_exp2f(float x)
{
    if (x < -125.0f) return 0.0f;
    if (x > 125.0f) x = 125.0f;
    float fi = floorf(x + 0.5f), f = x - fi;           /* f in [-0.5, 0.5] */
    float p = 1.0f + f * (0.69314718f + f * (0.24022651f + f * (0.05550411f + f * (0.00961813f + f * 0.00133336f))));
    union { float f; uint32_t i; } v;
    v.i = (uint32_t)((int)fi + 127) << 23;
    return p * v.f;
}

/* soft saturation, tanh-like (rational), unity slope at 0, limit +-1 */
static inline float mfo_sat(float x)
{
    if (x > 3.0f) return 1.0f;
    if (x < -3.0f) return -1.0f;
    float x2 = x * x;
    return x * (27.0f + x2) / (27.0f + 9.0f * x2);
}

static inline uint32_t mfo_rand(uint32_t *s) { *s = *s * 1664525u + 1013904223u; return *s; }
static inline float mfo_randf(uint32_t *s) { return (float)(mfo_rand(s) >> 8) * (1.0f / 8388608.0f) - 1.0f; } /* -1..1 */

/* ---- FFT internals ---- */
struct mfo_fft {
    int n, m;                    /* real size, complex size m = n/2 */
    int *rev;                    /* bit reversal of m */
    float *twr, *twi;            /* per-stage twiddles, stage with half h at offset h-1 */
    float *pr, *pi;              /* real post-processing twiddles, m entries: exp(-2 pi i k / n) */
    float *wr, *wi;              /* work, m entries */
};

/* ---- load spreading: start a convolver / STFT at a phase of its own (a multiple of MFO_BLOCK), so
 * several instances do not all hit their large FFT boundaries on the same sample. Latency unchanged. */
void mfo_conv_set_stagger(mfo_conv_t *c, int offset);     /* applies now and at every reset */
void mfo_xsyn_set_stagger(mfo_xsyn_t *x, int offset);     /* 0 .. hop-MFO_BLOCK, multiple of MFO_BLOCK */

/* ---- filters ---- */
typedef struct { float ic1[2], ic2[2]; } mfo_svf_t;
typedef struct { float s[2][4]; } mfo_ladder_t;
void mfo_svf_run(mfo_svf_t *f, int type, float cutoff, float reso, float sr, float *const *io, int nch, int n);
void mfo_ladder_run(mfo_ladder_t *f, float cutoff, float reso, float drive, float sr, float *const *io, int nch, int n);

/* ADSR (times in seconds, linear attack, exponential decay/release) */
typedef struct { int stage; float level, rel_start; } mfo_env_t;
enum { MFO_ENV_IDLE, MFO_ENV_ATTACK, MFO_ENV_DECAY, MFO_ENV_SUSTAIN, MFO_ENV_RELEASE };
void mfo_env_gate(mfo_env_t *e, int on);
/* advance n samples, write levels into out (may be NULL), return the last level */
float mfo_env_run(mfo_env_t *e, float a, float d, float s, float r, float sr, float *out, int n);

/* ---- streaming resampler (polyphase windowed sinc) ---- */
#define MFO_RS_TAPS 32
#define MFO_RS_PHASES 128
typedef struct {
    double step;                 /* input samples per output sample */
    double pos;                  /* fractional read position relative to hist start */
    float *kernel;               /* (PHASES+1) * TAPS */
    float hist[2][MFO_RS_TAPS * 8];
    int fill;                    /* samples in hist */
} mfo_resampler_t;
int mfo_resampler_init(mfo_resampler_t *r, double in_rate, double out_rate);
void mfo_resampler_free(mfo_resampler_t *r);
/* push input frames; returns how many could be accepted */
int mfo_resampler_space(const mfo_resampler_t *r);
void mfo_resampler_push(mfo_resampler_t *r, const float *l, const float *rr, int n);
/* pull up to n output frames; returns how many produced (stops when input runs short) */
int mfo_resampler_pull(mfo_resampler_t *r, float *l, float *rr, int n);

/* offline resample of a whole buffer (cubic-free windowed sinc), returns allocated buffer */
float *mfo_resample_buffer(const float *in, int len, double ratio /* out/in length */, int *out_len);

#endif
