/* Copied into Vintage Chonk on 17 Nov 2026 from chroma-mod/fx/drives/103_blown_octave_velcro.c
 * (the developer's own Chroma Console effects project). MFE changes, each marked "MFE:": Velcro.octg octave level (32768 = the original mix). Filter corners not rescaled (about 8 % lower at 44.1 kHz). */
/* No. 103 Blown Octave Velcro (Mod Character slot 3): an analog octave-down, into a starved "velcro" fuzz,
 * into a blown speaker, with the clean guitar blended back in.
 * 1. Octave down, the analog way (the classic divider pedals): the guitar is low-passed to find its
 *    fundamental, a Schmitt trigger with envelope-scaled hysteresis squares it, a flip-flop halves that,
 *    and the divided square switches the polarity of the (low-passed) guitar. The result sits an octave
 *    down and follows the playing dynamics; like the originals it tracks single notes best and warbles on
 *    chords, which is part of the sound.
 * 2. Velcro fuzz: a transistor stage starved and biased near cut-off. Small signals fall in its dead zone,
 *    so notes sputter and gate as they decay; what gets through is clipped lopsidedly (the starved side
 *    gives up early). Runs at 2x (23-tap half-band) to keep the hard edges from aliasing.
 * 3. Blown speaker: the low band (the cone's excursion) is hard-limited asymmetrically, as a torn surround
 *    bottoms out, and the part that overshoots the limit is turned into a band-passed (1.5-4 kHz) rasp, the
 *    flapping-paper buzz of a ripped cone. Then a small dead speaker's roll-off.
 * TILT: tone, dark .. bright (the speaker's roll-off from about 1.8 to 6 kHz).
 * AMOUNT: fuzz gain (low = sputtery and gated, high = fat and sustaining).
 * SENS (secondary TILT): clean blend, from none to the full dry guitar under the fuzz.
 * Secondary AMOUNT stays the factory Effect Vol. No HOLD. */
#include "fx_api.h"

static const int32_t bv_h[6] = {-281, 692, -1496, 2975, -6173, 20590};

typedef struct {
    int32_t xin[16], q0[16], q1[16];
    unsigned i;
    /* octave divider */
    int32_t thp8, tl1, tl2, tenv, olp, olp2;
    int sgn, ff;
    /* fuzz */
    int32_t dc8;
    int32_t g, gs;                 /* Q8 pre-gain */
    /* speaker */
    int32_t ex1, ex2, r_hp8, r_lp, s_lp1, s_lp2, s_hp8;
    int32_t tone, tones;           /* Q15 roll-off coefficient */
    int32_t blend, blends;         /* Q15 */
    int32_t mk, mks;               /* Q12 */
    int32_t octg;                  /* MFE: octave level Q15, 32768 = original */
} Velcro;

static const int16_t BV_MK[9] = {3253, 2009, 1155, 888, 782, 729, 705, 687, 672};

static int bv_attach(void *v, int16_t *mem, uint32_t words) {
    Velcro *s = (Velcro *)v;
    (void)mem; (void)words;
    s->sgn = 1;
    s->g = s->gs = 2048;
    s->tone = s->tones = 12000;
    s->mk = s->mks = 4096;
    s->octg = 32768;   /* MFE */
    return 1;
}

static void bv_control(void *v, const FxControls *c) {
    Velcro *s = (Velcro *)v;
    s->g = (int32_t)m_span(1024, 32768, c->k2);                      /* x4 .. x128 */
    s->tone = 7300 + (int32_t)(((uint32_t)c->k1 * 15000u) >> 16);  /* about 1.8 .. 6 kHz */
    s->blend = (int32_t)(((uint32_t)c->k3 * 32767u) >> 16);
    int i = c->k2 >> 13;
    int32_t f = c->k2 & 8191;
    s->mk = i >= 8 ? BV_MK[8] : BV_MK[i] + (((BV_MK[i + 1] - BV_MK[i]) * f) >> 13);
}

/* Starved transistor stage, one 96 kHz sample: smooth dead zone (|u| < 2048 gives 0, a quadratic onset to
 * 6144, then linear), then a lopsided clip (+22000 / -12000) with a quadratic knee. u clamped to 2^19. */
static inline int32_t bv_nl(int32_t u) {
    if (u > 524287) u = 524287;
    if (u < -524287) u = -524287;
    int32_t a = u < 0 ? -u : u, z;
    if (a < 2048) return 0;
    if (a < 6144) z = ((a - 2048) * (a - 2048)) >> 13;
    else z = a - 4096;
    if (u > 0) {
        if (z <= 18000) return z;
        int32_t e = z - 18000;
        return e >= 8000 ? 22000 : 18000 + e - ((e * e) >> 14) - ((e * e) >> 16);
    }
    if (z <= 8000) return -z;
    int32_t e = z - 8000;
    return e >= 8000 ? -12000 : -(8000 + e - ((e * e) >> 14) - ((e * e) >> 16));
}

static void bv_process(void *v, int32_t pcm[32]) {
    Velcro *s = (Velcro *)v;
    for (unsigned n = 0; n < MOD_FRAMES; ++n) {
        s->gs += (s->g - s->gs) >> 6;
        s->tones += (s->tone - s->tones) >> 6;
        s->blends += (s->blend - s->blends) >> 6;
        s->mks += (s->mk - s->mks) >> 6;
        int32_t x = (m_in16(pcm[n]) + m_in16(pcm[MOD_FRAMES + n])) >> 1;

        /* --- octave divider (48 kHz) --- */
        int32_t t = x - m_lpq(&s->thp8, x, 300);                 /* 70 Hz high-pass */
        t = m_lp(&s->tl1, t, 2077);                              /* two poles at 500 Hz: the fundamental */
        t = m_lp(&s->tl2, t, 2077);
        int32_t at = t < 0 ? -t : t;
        if (at > s->tenv) s->tenv += (at - s->tenv) >> 3;
        else if (s->tenv > 0) s->tenv -= (s->tenv >> 10) + 1;
        if (s->tenv < 0) s->tenv = 0;
        int32_t hy = (s->tenv >> 2) + 24;                        /* hysteresis follows the level */
        if (s->sgn > 0 && t < -hy) s->sgn = -1;
        else if (s->sgn < 0 && t > hy) { s->sgn = 1; s->ff ^= 1; }
        int32_t xo = m_lp(&s->olp, x, 6000);                     /* the divider switches a 1.5 kHz low-passed copy */
        xo = m_lp(&s->olp2, xo, 6000);
        int32_t oct = s->ff ? xo : -xo;
        if (s->octg < 32768) oct = (oct * s->octg) >> 15;        /* MFE: octave blend (|oct| <= 32768) */
        int32_t fin = (x >> 1) + oct;                            /* octave-heavy into the fuzz, |fin| <= 49152 */

        /* --- velcro fuzz at 96 kHz --- */
        unsigned i = s->i = (s->i + 1u) & 15u;
        s->xin[i] = m_sat16(fin);
        int32_t acc = 0;
        for (unsigned k = 0; k < 6; ++k)
            acc += bv_h[k] * (s->xin[(i - k) & 15u] + s->xin[(i - 11u + k) & 15u]);
        int32_t mid = m_sat16(acc >> 15);
        int32_t on = s->xin[(i - 5u) & 15u];
        s->q0[i] = bv_nl((mid * s->gs) >> 8);                     /* |mid| <= 32767, g <= 32768: inside 2^30 */
        s->q1[i] = bv_nl((on * s->gs) >> 8);
        acc = s->q0[(i - 5u) & 15u] * 16384;
        for (unsigned k = 0; k < 6; ++k)
            acc += bv_h[k] * ((s->q1[(i - k) & 15u] + s->q1[(i - 11u + k) & 15u]) >> 1);
        int32_t y = acc >> 15;                                   /* |y| <= about 23000 */
        y -= m_lpq(&s->dc8, y, 107);                             /* the lopsided clip leaves DC */

        /* --- blown speaker (48 kHz) --- */
        int32_t ex = m_lp(&s->ex1, y, 1070);                     /* cone excursion: two poles at 250 Hz */
        ex = m_lp(&s->ex2, ex, 1070);
        int32_t exc = ex > 7000 ? 7000 : ex < -4500 ? -4500 : ex;   /* the torn surround bottoms out */
        int32_t over = ex - exc;
        int32_t r = over < 0 ? -over : over;                     /* flapping: the overshoot, rectified ... */
        r -= m_lpq(&s->r_hp8, r, 1600);                          /* ... high-passed at 1.5 kHz */
        r = m_lp(&s->r_lp, r, 13600);                            /* ... and low-passed at 4 kHz: the rasp */
        int32_t sp = (y - ex) + exc + r;
        sp = m_lp(&s->s_lp1, sp, s->tones);                      /* dead small speaker roll-off (TILT) */
        sp = m_lp(&s->s_lp2, sp, s->tones);
        sp -= m_lpq(&s->s_hp8, sp, 256);                         /* and no real bass below 60 Hz */

        int32_t wet = (sp * s->mks) >> 12;
        int32_t o = m_out23(wet + ((x * s->blends) >> 15));
        pcm[n] = o;
        pcm[MOD_FRAMES + n] = o;
    }
}

const FxDesc fx_blown_octave_velcro = {
    103, "blown_octave_velcro", "Blown Octave Velcro", FX_CHAR, 0, sizeof(Velcro), 0,
    bv_attach, bv_control, bv_process,
    30000, 40000, 26000, 0,
    "TILT tone (dead speaker roll-off) · AMOUNT fuzz gain (sputtery to fat) · SENS clean blend · no HOLD"
};
