/* Copied into Vintage Chonk on 17 Nov 2026 from chroma-mod/fx/texture_reverbs/_spring_core.h
 * (the developer's own Chroma Console effects project). MFE changes, each marked "MFE:": CH_T on the loop delays, CH_INC on the wobble rates (the tank runs at fs/2 = 22.05 kHz). */
/* Texture-module reverbs 141-143: the shared spring tank core (17 Nov 2026).
 * The core below is copied verbatim from fx/surf/002_self_osc_spring.c (Valimaki/Parker/Abel parametric
 * spring: stretched first-order all-pass chains inside a delay loop, run mono at 24 kHz with Blackfin dual
 * 16-bit ops). It is shared here as one header so the three Texture springs use exactly the same core.
 * Added after it: the two TRANSDUCERS of a real tank (estimated values, see TEXTURE-NOTES.md):
 *   - driver: the input coil/magnet suspension resonance, a low "boing" bump near 110 Hz (Q about 1.4);
 *   - pickup: the output transducer's upper resonance, a peak near 3.8 kHz (Q about 2) before roll-off,
 *     applied at the tank's 24 kHz rate. */
#ifndef TX_SPRING_CORE_H
#define TX_SPRING_CORE_H
#include "fx_api.h"
#include "_tx_common.h"

/* ===== Spring tank core, shared by the surf effects (copied into each file, not linked) =====
 * Cost-designed: a mono tank run at 24 kHz (a spring passes almost nothing above 5 kHz), first-order
 * all-pass sections stretched by K = 4 (chirp region up to 3 kHz) processed block-wise two samples at a
 * time with the Blackfin dual 16-bit fractional ops (host: exact C equivalents), a short pre-cascade for
 * the first arrival and a longer cascade inside each spring loop, so each echo is more dispersed than the
 * last. Loop: delay (wobbling) -> chirp cascade -> damping -> low cut -> gain. */
enum { SP_N = MOD_FRAMES / 2, SP_K = 4, SP_NP = 8, SP_NL = 16, SP_W = SP_K + SP_N, SP_MAXS = 3,
       SP_RING = 2048 };
static const int16_t sp_apre[SP_NP] = {20316, 21627, 19988, 22282, 20972, 21954, 19661, 22610};
static const int16_t sp_aloop[SP_NL] = {21299, 20316, 22610, 19988, 21954, 20644, 22938, 20972,
                                        21627, 19661, 22282, 21299, 20644, 22610, 19988, 21627};

typedef short sp_v2 __attribute__((vector_size(4)));
#ifdef __bfin__
#define SP_VSUB(a, b) __builtin_bfin_sub_fr2x16(a, b)     /* saturating */
#define SP_VADD(a, b) __builtin_bfin_add_fr2x16(a, b)     /* saturating */
#define SP_VMUL(a, b) __builtin_bfin_multr_fr2x16(a, b)   /* (a b) >> 15, rounded half to even */
#else
static inline sp_v2 SP_VSUB(sp_v2 a, sp_v2 b) { sp_v2 r; r[0] = (short)m_sat16(a[0] - b[0]); r[1] = (short)m_sat16(a[1] - b[1]); return r; }
static inline sp_v2 SP_VADD(sp_v2 a, sp_v2 b) { sp_v2 r; r[0] = (short)m_sat16(a[0] + b[0]); r[1] = (short)m_sat16(a[1] + b[1]); return r; }
static inline short sp_rnd(int32_t p) { int32_t r = (p + 16384) >> 15; if ((p & 0x7fff) == 0x4000) r &= ~1; return (short)m_sat16(r); }
static inline sp_v2 SP_VMUL(sp_v2 a, sp_v2 b) { sp_v2 r; r[0] = sp_rnd(a[0] * b[0]); r[1] = sp_rnd(a[1] * b[1]); return r; }
#endif

typedef struct {
    int16_t pre[SP_NP + 1][SP_W] __attribute__((aligned(4)));
    int16_t lb[SP_MAXS][SP_NL + 1][SP_W] __attribute__((aligned(4)));
    int16_t *ring[SP_MAXS];
    uint32_t w, ph[SP_MAXS], inc[SP_MAXS];
    int ns, nl;                                    /* springs, loop stages in use (<= SP_NL) */
    int32_t d[SP_MAXS], dt[SP_MAXS], dts[SP_MAXS];  /* loop delay Q8 (24 kHz samples): now, slewed, target */
    int32_t fb[SP_MAXS], fbt[SP_MAXS];             /* loop gain Q15 */
    int32_t damp_a, damp[SP_MAXS], hp8[SP_MAXS];
    int32_t wob, wob_t;                            /* wobble depth, Q8 samples */
    int32_t out[SP_MAXS][SP_N];                    /* each spring's pickup, 24 kHz */
    int32_t aa[4];                                 /* 4th-order 5 kHz low-pass before the 2:1 decimation */
} SpTank;

/* Chamberlin state-variable low-pass section; f, q in Q15/Q14. States 32-bit. */
static inline int32_t sp_svf(int32_t *lp, int32_t *bp, int32_t x, int32_t f, int32_t q) {
    *lp += (f * (*bp >> 1)) >> 14;
    int32_t hp = x - *lp - ((q * (*bp >> 1)) >> 13);
    hp = hp > 98303 ? 98303 : (hp < -98304 ? -98304 : hp);
    *bp += (f * (hp >> 1)) >> 14;
    return *lp;
}

static void sp_cascade(int16_t (*b)[SP_W], const int16_t *a, int nst) {
    for (int s = 0; s < nst; ++s) {
        const sp_v2 *x = (const sp_v2 *)b[s];
        sp_v2 *y = (sp_v2 *)b[s + 1];
        sp_v2 c = {a[s], a[s]};
        for (int n = 0; n < SP_N / 2; ++n)          /* y[n] = a (x[n] - y[n-K]) + x[n-K] */
            y[SP_K / 2 + n] = SP_VADD(SP_VMUL(SP_VSUB(x[SP_K / 2 + n], y[n]), c), x[n]);
    }
    for (int s = 0; s <= nst; ++s) {
        sp_v2 *z = (sp_v2 *)b[s];
        for (int k = 0; k < SP_K / 2; ++k) z[k] = z[SP_N / 2 + k];
    }
}

/* x * g (Q15), truncated toward zero so integer feedback loops cannot hold a limit cycle. */
static inline int32_t sp_g(int32_t x, int32_t g) { return (x * g + ((x >> 31) & 32767)) >> 15; }

static void sp_init(SpTank *t, int16_t *mem, int ns, const uint32_t *delay_samples) {
    t->ns = ns;
    t->nl = SP_NL;
    for (int s = 0; s < ns; ++s) {
        t->ring[s] = mem + s * SP_RING;
        t->d[s] = t->dt[s] = t->dts[s] = (int32_t)(CH_T(delay_samples[s]) << 8);   /* MFE: CH_T */
        t->fb[s] = t->fbt[s] = 26000;
        t->inc[s] = CH_INC(160000u + 62000u * (uint32_t)s);   /* about 0.9, 1.25, 1.6 Hz swing (MFE: CH_INC) */
        t->ph[s] = 0x55555555u * (uint32_t)s;
    }
    t->damp_a = 19700;                                /* about 3.5 kHz per pass */
}

/* Decay: RT60 in 24 kHz samples -> loop gain for each spring (block-rate division). */
static void sp_decay(SpTank *t, uint32_t rt60) {
    for (int s = 0; s < t->ns; ++s) {
        uint32_t d = (uint32_t)t->dts[s] >> 8;
        int32_t g = 32768 - (int32_t)((226099u * d) / rt60);   /* 1 - 6.9 D / RT60 */
        t->fbt[s] = g < 0 ? 0 : (g > 32440 ? 32440 : g);
    }
}

/* 48 kHz drive -> 24 kHz: 4th-order Butterworth low-pass at 5 kHz, then 2:1. */
static void sp_down(SpTank *t, const int32_t *in48, int32_t *d24) {
    for (int n = 0; n < SP_N; ++n) {
        int32_t a = sp_svf(&t->aa[0], &t->aa[1], in48[2 * n], 21140, 30277);
        a = sp_svf(&t->aa[2], &t->aa[3], a, 21140, 12534);
        int32_t b = sp_svf(&t->aa[0], &t->aa[1], in48[2 * n + 1], 21140, 30277);
        b = sp_svf(&t->aa[2], &t->aa[3], b, 21140, 12534);
        d24[n] = (a + b) >> 1;
    }
}

/* One block of the tank at 24 kHz. in24[8]: drive; inj[8] (or NULL): shaken straight into every spring.
 * Results in t->out. */
static void sp_tank(SpTank *t, const int32_t *in24, const int32_t *inj) {
    for (int n = 0; n < SP_N; ++n) t->pre[0][SP_K + n] = (int16_t)m_sat16(in24[n]);
    sp_cascade(t->pre, sp_apre, SP_NP);
    const int16_t *p = &t->pre[SP_NP][SP_K];
    t->wob = m_slew(t->wob, t->wob_t, 4);
    for (int s = 0; s < t->ns; ++s) {
        int16_t (*b)[SP_W] = t->lb[s];
        int16_t *r = t->ring[s];
        t->ph[s] += t->inc[s] * SP_N;
        t->dt[s] = m_slew(t->dt[s], t->dts[s], 7);
        t->fb[s] = m_slew(t->fb[s], t->fbt[s], 5);
        int32_t target = t->dt[s] + ((m_sin(t->ph[s]) * t->wob) >> 14);
        int32_t step = (target - t->d[s]) >> 3, dq = t->d[s];
        for (int n = 0; n < SP_N; ++n, dq += step)
            b[0][SP_K + n] = (int16_t)m_read(r, SP_RING - 1, t->w + (uint32_t)n, (uint32_t)dq);
        t->d[s] = dq;
        sp_cascade(b, sp_aloop, t->nl);
        int32_t fb = t->fb[s];
        const int16_t *o = &b[t->nl][SP_K];
        for (int n = 0; n < SP_N; ++n) {
            int32_t y = m_lp(&t->damp[s], o[n], t->damp_a);
            y -= m_lpq(&t->hp8[s], y, 686);                      /* springs pass little below ~80 Hz */
            t->out[s][n] = y;
            int32_t v = p[n] + sp_g(y, fb) + (inj ? inj[n] : 0);
            r[(t->w + (uint32_t)n) & (SP_RING - 1)] = (int16_t)m_sat16(v);
        }
    }
    t->w += SP_N;
}

static void sp_run(SpTank *t, const int32_t *in48, const int32_t *inj) {
    int32_t d24[SP_N];
    sp_down(t, in48, d24);
    sp_tank(t, d24, inj);
}

/* 24 kHz -> 48 kHz, linear interpolation (the tank is dark, so images are small; callers low-pass after). */
static inline void sp_up(int32_t *prev, const int32_t *x, int32_t *y) {
    for (int n = 0; n < SP_N; ++n) {
        y[2 * n] = (*prev + x[n]) >> 1;
        y[2 * n + 1] = *prev = x[n];
    }
}
/* ===== end of spring tank core ===== */

/* ===== transducers ===== */
typedef struct { int32_t dl, db, pl[2], pb[2]; } SpXdcr;

/* Driver resonance: drive + 0.5 x band-pass(110 Hz, Q 1.4), applied in place to the 8 decimated 24 kHz drive
 * samples (f = 2 sin(pi 110 / 24000) = 472 in Q14). */
static inline void sp_driver24(SpXdcr *x, int32_t *d24) {
    for (int n = 0; n < SP_N; ++n) d24[n] += tx_lowres(&x->dl, &x->db, d24[n], 472, 11703) >> 1;
}

/* sp_run with the driver transducer between the decimator and the tank. */
static void sp_run_x(SpTank *t, SpXdcr *x, const int32_t *in48, const int32_t *inj) {
    int32_t d24[SP_N];
    sp_down(t, in48, d24);
    sp_driver24(x, d24);
    sp_tank(t, d24, inj);
}

/* Pickup resonance: y + 0.35 x band-pass(3.8 kHz, Q 2), run on the 24 kHz tank output (8 samples per block,
 * before the 2x interpolation), floor-rounded products (cheap); exact-zero snap once silent. ch = 0/1. */
static inline int32_t sp_pickup(SpXdcr *x, int ch, int32_t v) {
    int32_t *lp = &x->pl[ch], *bp = &x->pb[ch];
    if (!v && (uint32_t)(*bp + 4) < 8u && (uint32_t)(*lp + 4) < 8u) { *lp = *bp = 0; return 0; }
    *lp += (15788 * *bp) >> 14;                                     /* f = 2 sin(pi 3.8k / 24k), Q14 */
    int32_t hp = v - *lp - (*bp >> 1);                              /* q = 1/Q = 0.5 */
    hp = hp > 98303 ? 98303 : (hp < -98304 ? -98304 : hp);
    *bp += (15788 * hp) >> 14;
    if (*bp > 98303) *bp = 98303;
    if (*bp < -98304) *bp = -98304;
    return v + ((*bp * 11469) >> 15);
}
#endif
