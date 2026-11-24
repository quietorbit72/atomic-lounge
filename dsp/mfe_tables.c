/* SPDX-License-Identifier: AGPL-3.0-only
 * Vintage Chonk Engine: lookup tables and the original wavetable set.
 * Init-time code uses single-precision float only (the FM1's FPU does float in hardware, double in
 * software); the tables are built at boot into RAM, costing no Flash. The audio path is integer. */
#include <math.h>
#include <string.h>
#include "mfe_internal.h"

#define PI 3.14159265f

int16_t mfe_sine_tab[1025];
int16_t mfe_wt[MFE_WT_STORE][MFE_WT_FRAMES][MFE_WT_STRIDE];
static uint32_t inc_tab[193];        /* note 120 + i/16 semitone */
static int32_t g_tab[545];           /* cutoff g, quarter semitones 0..136, Q16 */
static uint32_t ticks_tab[257];
static int32_t coef_tab[257];
static uint32_t lfo_tab[257];
static int16_t clip_tab[1025];       /* tanh over -4..4 */
int16_t mfe_perf_tab[MFE_PERF_SHAPES][65];

/* Performer shapes. 0-15 recreate the 16 curves of Massive's Performer library from the developer's reference
 * picture (16 Nov 2026), drawn from simple formulas (no NI data) and corrected against Massive's measured
 * output (docs/MASSIVE_MODULATORS.md); 16-31 are Vintage Chonk's own. */
static const char *const PERF_NAMES[MFE_PERF_SHAPES] = {
    "RAMP UP", "RAMP DOWN", "EXP UP", "EXP DOWN", "LOG UP", "HOLD FALL", "S UP", "S DOWN",
    "HIGH", "SQUARE", "TRIANGLE", "BUMP", "DECAY", "DECAY X2", "DECAY X3", "DECAY X4",
    "ZERO", "FULL", "HALF SINE", "SINE", "BUMP", "STAIRS UP", "STAIRS DN", "SHORT GATE",
    "RAMP X2", "RAMP DN X2", "TRI X2", "WOBBLE", "BOUNCE", "LATE RISE", "GATE 3/4", "SPIKE"};
const char *mfe_perf_shape_name(unsigned id) { return id < MFE_PERF_SHAPES ? PERF_NAMES[id] : "?"; }

static float perf_shape(unsigned id, float x)
{
    float f, e4 = expf(4.0f) - 1.0f;
    switch (id) {
    case 0: return x;
    case 1: return 1 - x;
    case 2: f = x * x; return f * f;                              /* measured: about x^4 */
    case 3: return (expf(4 * (1 - x)) - 1) / e4;                  /* measured: exponential fall to 0 */
    case 4: f = 1 - x; return 1 - f * f * f;
    case 5: return 1 - x * x * x;
    case 6: return x * x * (3 - 2 * x);
    case 7: return 1 - x * x * (3 - 2 * x);
    case 8: return 1.0f;                                          /* measured: full for the whole step */
    case 9: return x < 0.5f ? 1.0f : 0.0f;
    case 10: return 1 - fabsf(2 * x - 1);
    case 11: return 0.5f - 0.5f * cosf(2 * PI * x);               /* measured: raised cosine */
    case 12: case 13: case 14: case 15: {
        float n = (float)(id - 11), u = x * n;                    /* measured: (1-u)^2, 1-4 times a step */
        u -= floorf(u);
        return (1 - u) * (1 - u);
    }
    case 16: return 0;
    case 17: return 1;
    case 18: return sinf(PI * x);
    case 19: return 0.5f + 0.5f * sinf(2 * PI * x);
    case 20: return 0.5f - 0.5f * cosf(2 * PI * x);
    case 21: f = floorf(x * 4); return (f > 3 ? 3 : f) / 3.0f;
    case 22: f = floorf(x * 4); return 1 - (f > 3 ? 3 : f) / 3.0f;
    case 23: return x < 0.15f ? 1.0f : 0.0f;
    case 24: f = 2 * x; return f - floorf(f);
    case 25: f = 2 * x; return 1 - (f - floorf(f));
    case 26: f = 2 * x; f -= floorf(f); return 1 - fabsf(2 * f - 1);
    case 27: return 0.5f + 0.5f * sinf(4 * PI * x);
    case 28: return fabsf(sinf(3 * PI * x)) * (1 - x);
    case 29: f = x * x; return f * f;
    case 30: return x < 0.75f ? 1.0f : 0.0f;
    case 31: return x < 0.1f ? x / 0.1f : expf(-8.0f * (x - 0.1f));
    }
    return 0;
}

int32_t mfe_preview_wave(unsigned table, int32_t pos, unsigned i)
{
    if (table >= MFE_WT_COUNT) table = 0;
    int32_t fp = MFE_CLAMP(pos, 0, 32767) * (MFE_WT_FRAMES - 1), fi = fp >> 15, ff = fp & 32767;
    if (fi >= MFE_WT_FRAMES - 1) { fi = MFE_WT_FRAMES - 2; ff = 32767; }
    const int16_t *a = mfe_wt_frame(table, (unsigned)fi, 0), *b = mfe_wt_frame(table, (unsigned)fi + 1, 0);
    i &= MFE_WT_FRAME - 1;
    return a[i] + (((b[i] - a[i]) * ff) >> 15);
}
int32_t mfe_preview_shape(unsigned id, uint32_t x) { return mfe_perf_shape(id, x > 65535 ? 65535 : x); }

/* tempo divisions: beats per cycle (or per Performer/Stepper step) */
static const uint8_t SYNC_NUM[MFE_SYNC_DIVS] = {1, 1, 1, 1, 3, 1, 2, 3, 1, 3, 2, 4, 8};
static const uint8_t SYNC_DEN[MFE_SYNC_DIVS] = {8, 6, 4, 3, 8, 2, 3, 4, 1, 2, 1, 1, 1};
void mfe_sync_beats(unsigned d, uint32_t *num, uint32_t *den)
{
    if (d >= MFE_SYNC_DIVS) d = 8;
    *num = SYNC_NUM[d];
    *den = SYNC_DEN[d];
}
static int tables_ready;

static const char *const WT_NAMES[MFE_WT_COUNT] = {"BASIC", "PWM", "SYNC", "FORMANT", "ORGAN", "SQUASH", "MOOG"};
const char *mfe_wavetable_name(unsigned t) { return t < MFE_WT_COUNT ? WT_NAMES[t] : "?"; }

/* ---- original wavetable shapes: x(u) for phase u in [0,1), frame position t in [0,1] ---- */
static float shape(unsigned table, float u, float t)
{
    switch (table) {
    case MFE_WT_BASIC: {                 /* sine -> triangle -> saw -> square */
        float s = t * 3.0f, f;
        int seg = (int)s;
        float w[4];
        if (seg > 2) seg = 2;
        f = s - seg;
        w[0] = sinf(2 * PI * u);
        w[1] = u < 0.25f ? 4 * u : u < 0.75f ? 2 - 4 * u : 4 * u - 4;
        w[2] = 2 * (u < 0.5f ? u + 0.5f : u - 0.5f) - 1;
        w[3] = u < 0.5f ? 1 : -1;
        return w[seg] * (1 - f) + w[seg + 1] * f;
    }
    case MFE_WT_PWM: {
        float w = 0.5f - 0.45f * t;
        return u < w ? 1.0f : -1.0f;
    }
    case MFE_WT_SYNC: {
        float r = 1.0f + 7.0f * t, v = r * u;
        return 2.0f * (v - floorf(v)) - 1.0f;
    }
    case MFE_WT_FORMANT: {               /* a decaying burst at a moving formant harmonic */
        float m = 2.0f + 14.0f * t, e = (1 - u) * (1 - u);
        return sinf(2 * PI * m * u) * e;
    }
    case MFE_WT_ORGAN: {                 /* drawbars rising with t */
        static const float h[6] = {1, 2, 3, 4, 6, 8};
        float x = 0;
        for (int i = 0; i < 6; i++) {
            float g = i == 0 ? 1.0f : fminf(1.0f, fmaxf(0.0f, t * 6.0f - (i - 1)));
            x += g * sinf(2 * PI * h[i] * u) / (1 + 0.3f * i);
        }
        return x;
    }
    case MFE_WT_MOOG: {                  /* classic analogue-style waveforms, gently imperfect */
        int fr = (int)(t * 7.0f + 0.5f);
        float cap = 0.35f;               /* exponential capacitor charge: ramps are not quite straight */
        switch (fr) {
        case 0:                          /* triangle, its peak a little early */
            return u < 0.47f ? -1 + 2 * u / 0.47f : 1 - 2 * (u - 0.47f) / 0.53f;
        case 1: {                        /* shark: a slow curved rise, a quicker fall */
            if (u < 0.78f) return -1 + 2 * (1 - expf(-2.2f * u / 0.78f)) / (1 - expf(-2.2f));
            return 1 - 2 * (u - 0.78f) / 0.22f;
        }
        case 2:                          /* reverse (falling) saw */
            return 1 - 2 * (1 - expf(-cap * u * 3)) / (1 - expf(-cap * 3));
        case 3:                          /* rising saw */
            return -1 + 2 * (1 - expf(-cap * u * 3)) / (1 - expf(-cap * 3));
        default: {                       /* square and pulses with a little droop on each level */
            static const float W[] = {0.5f, 0.30f, 0.12f, 0.06f};
            float w = W[fr - 4], droop = 0.06f;
            return u < w ? 1 - droop * u / w : -1 + droop * (u - w) / (1 - w);
        }
        }
    }
    case MFE_WT_SQUASH: {                /* a sine through a wavefolder */
        float g = 1.0f + 7.0f * t;
        return sinf(g * sinf(2 * PI * u) * PI / 2);
    }
    }
    return 0;
}

/* one frame of one table (all its band-limited levels): about 1.3 M float operations */
static void gen_frame(unsigned tb, unsigned fr)
{
    enum { N = 1024, H = 128 };
    static float re[H + 1], im[H + 1], x[N];
        {
            float t = (float)fr / (MFE_WT_FRAMES - 1), peak = 0;
            for (int n = 0; n < N; n++) x[n] = shape(tb, (n + 0.5f) / N, t);
            for (int k = 1; k <= H; k++) {      /* DFT up to harmonic 128 (DC dropped), phasor recurrence */
                float c = 1, sn = 0, dc = cosf(2 * PI * k / N), ds = sinf(2 * PI * k / N), a = 0, b = 0;
                for (int n = 0; n < N; n++) {
                    a += x[n] * c;
                    b += x[n] * sn;
                    float c2 = c * dc - sn * ds;
                    sn = sn * dc + c * ds;
                    c = c2;
                }
                re[k] = 2 * a / N;
                im[k] = 2 * b / N;
            }
            for (unsigned L = 0; L < MFE_WT_LEVELS; L++) {
                int top = H >> L, len = 1 << MFE_WT_BITS[L];
                int16_t *dst = &mfe_wt[MFE_WT_MOOG_ONLY ? 0 : tb][fr][MFE_WT_OFF[L]];
                static float y[MFE_WT_FRAME];
                for (int n = 0; n < len; n++) y[n] = 0;
                for (int k = 1; k <= top; k++) {
                    float c = 1, sn = 0, dc = cosf(2 * PI * k / len), ds = sinf(2 * PI * k / len);
                    for (int n = 0; n < len; n++) {
                        y[n] += re[k] * c + im[k] * sn;
                        float c2 = c * dc - sn * ds;
                        sn = sn * dc + c * ds;
                        c = c2;
                    }
                }
                if (L == 0) {
                    for (int n = 0; n < len; n++) if (fabsf(y[n]) > peak) peak = fabsf(y[n]);
                    if (peak < 1e-9f) peak = 1;
                }
                for (int n = 0; n < len; n++) {
                    float v = y[n] / peak * 32000.0f;   /* every level scaled as level 0 */
                    v = v > 32767 ? 32767 : v < -32767 ? -32767 : v;
                    dst[n] = (int16_t)(v < 0 ? v - 0.5f : v + 0.5f);
                }
            }
        }
}

static int32_t rnd(float v) { return (int32_t)(v < 0 ? v - 0.5f : v + 0.5f); }
static uint32_t rndu(float v) { return (uint32_t)(v + 0.5f); }

static unsigned wt_next;              /* next (table, frame) to build */

int mfe_init_tables_step(void)
{
    if (wt_next >= MFE_WT_STORE * MFE_WT_FRAMES) return 1;
    gen_frame(MFE_WT_MOOG_ONLY ? MFE_WT_MOOG : wt_next / MFE_WT_FRAMES, wt_next % MFE_WT_FRAMES);
    wt_next++;
    return wt_next >= MFE_WT_STORE * MFE_WT_FRAMES;
}

int mfe_tables_ready(void) { return tables_ready && wt_next >= MFE_WT_STORE * MFE_WT_FRAMES; }

void mfe_init_tables_small(void)
{
    if (tables_ready) return;
    for (int i = 0; i <= 1024; i++) mfe_sine_tab[i] = (int16_t)rnd(32767.0f * sinf(2 * PI * i / 1024));
    for (int i = 0; i <= 192; i++) {
        float f = 440.0f * powf(2.0f, (120.0f + i / 16.0f - 69.0f) / 12.0f);
        inc_tab[i] = rndu(f / MFE_SR * 4294967296.0f);
    }
    for (int i = 0; i <= 544; i++) {
        float f = 440.0f * powf(2.0f, (i / 4.0f - 69.0f) / 12.0f);
        if (f > 0.49f * MFE_SR) f = 0.49f * MFE_SR;
        g_tab[i] = rnd(tanf(PI * f / MFE_SR) * 65536.0f);
    }
    for (int i = 0; i <= 256; i++) {
        float p = i / 256.0f, t = 0.0005f * powf((float)MFE_TIME_RANGE, p);
        float ticks = t * MFE_SR / MFE_CTL;
        ticks_tab[i] = ticks < 1 ? 1 : rndu(ticks);
        coef_tab[i] = rnd(expf(-6.9f / (ticks < 1 ? 1 : ticks)) * 1073741824.0f);
        float hz = 0.02f * powf(2000.0f, p);
        lfo_tab[i] = rndu(hz * MFE_CTL / MFE_SR * 4294967296.0f);
    }
    for (int i = 0; i <= 1024; i++) clip_tab[i] = (int16_t)rnd(32767.0f * tanhf((i - 512) / 128.0f));
    for (unsigned s = 0; s < MFE_PERF_SHAPES; s++)
        for (int i = 0; i <= 64; i++) {
            float y = perf_shape(s, i < 64 ? i / 64.0f : 0.9999f);
            mfe_perf_tab[s][i] = (int16_t)rnd(32767.0f * (y < 0 ? 0 : y > 1 ? 1 : y));
        }
    tables_ready = 1;
}

void mfe_init_tables(void)
{
    mfe_init_tables_small();
    while (!mfe_init_tables_step()) { }
}

/* ---- integer accessors used by the audio path ---- */
uint32_t mfe_pitch_inc(int32_t p)
{
    p = MFE_CLAMP(p, -48 * 256, 135 * 256);                /* down to note -48 (about 0.5 Hz) for LO */
    int32_t oct = p >= 0 ? p / 3072 : -((-p + 3071) / 3072), rem = p - oct * 3072;
    uint32_t i = (uint32_t)rem >> 4, f = (uint32_t)rem & 15u;
    uint64_t v = inc_tab[i] + (((uint64_t)(inc_tab[i + 1] - inc_tab[i]) * f) >> 4);
    return oct >= 10 ? (uint32_t)(v << (oct - 10)) : (uint32_t)(v >> (10 - oct));   /* oct >= -4 */
}

int32_t mfe_cutoff_g(int32_t p)
{
    p = MFE_CLAMP(p, 0, 136 * 256 - 1);
    int32_t i = p >> 6, f = p & 63;
    return g_tab[i] + (int32_t)(((int64_t)(g_tab[i + 1] - g_tab[i]) * f) >> 6);
}

static int32_t interp256(const int32_t *t, int32_t q15)
{
    q15 = MFE_CLAMP(q15, 0, 32767);
    int32_t i = q15 >> 7, f = q15 & 127;
    return t[i] + (int32_t)(((int64_t)(t[i + 1] - t[i]) * f) >> 7);
}
uint32_t mfe_time_ticks(int32_t q15) { return (uint32_t)interp256((const int32_t *)ticks_tab, q15); }
int32_t mfe_time_coef(int32_t q15) { return interp256(coef_tab, q15); }
uint32_t mfe_lfo_inc(int32_t q15)
{
    q15 = MFE_CLAMP(q15, 0, 32767);
    uint32_t i = (uint32_t)q15 >> 7, f = (uint32_t)q15 & 127u;
    return lfo_tab[i] + (uint32_t)(((uint64_t)(lfo_tab[i + 1] - lfo_tab[i]) * f) >> 7);
}

int32_t mfe_softclip(int32_t x)
{
    /* index: x Q15 / 256 + 512 covers -4.0 .. 4.0 */
    int32_t s = (x >> 8) + 512, f = x & 255;
    if (s < 0) return clip_tab[0];
    if (s >= 1024) return clip_tab[1024];
    return clip_tab[s] + (((clip_tab[s + 1] - clip_tab[s]) * f) >> 8);
}
