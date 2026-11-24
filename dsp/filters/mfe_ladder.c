/* SPDX-License-Identifier: AGPL-3.0-only
 * Vintage Chonk Engine: a classic Moog-style 24 dB/oct transistor-ladder low-pass, in fixed point.
 * Topology: four trapezoidal one-pole low-passes in series, the output fed back (one sample late)
 * against the input, the input through a tanh-like saturation; the classic ladder behaviour follows
 * the published analyses of the circuit (Stilson & Smith 1996, Huovilainen 2004). Independent integer
 * implementation. Resonance 0..1 maps to feedback k = 0..4.2 (self-oscillation from about k = 4). */
#include "mfe_internal.h"

#define LIM (4 * 32768)

void mfe_ladder_coef(mfe_ladder_coef_t *c, int32_t pitch_q8, int32_t res_q15)
{
    int64_t g = mfe_cutoff_g(pitch_q8);                         /* Q16 */
    c->G = (int32_t)((g << 30) / (65536 + g));                  /* Q30 */
    res_q15 = MFE_CLAMP(res_q15, 0, 32767);
    c->k = (int32_t)(((int64_t)res_q15 * 137626) >> 15);       /* 0 .. 4.2 in Q15 */
    c->G4 = c->G;
    c->Ghp = 0;
    c->acid = 0;
}

/* TB-303-style "acid" variant of the same structure (an approximation of the diode ladder, not a
 * circuit model): the last pole sits three octaves above the cutoff, which gives the gentler ~18 dB/oct
 * slope; the feedback passes a one-pole high-pass (~150 Hz) so low notes keep their weight at high
 * resonance; the input stage is driven harder. Resonance 0..1 -> k 0..3.6. */
void mfe_acid_coef(mfe_ladder_coef_t *c, int32_t pitch_q8, int32_t res_q15)
{
    int64_t g = mfe_cutoff_g(pitch_q8), g4 = mfe_cutoff_g(pitch_q8 + 36 * 256);
    int64_t ghp = mfe_cutoff_g(52 * 256);                       /* about 147 Hz */
    c->G = (int32_t)((g << 30) / (65536 + g));
    c->G4 = (int32_t)((g4 << 30) / (65536 + g4));
    c->Ghp = (int32_t)((ghp << 30) / (65536 + ghp));
    res_q15 = MFE_CLAMP(res_q15, 0, 32767);
    c->k = (int32_t)(((int64_t)res_q15 * 117965) >> 15);       /* 0 .. 3.6 in Q15 */
    c->acid = 1;
}

int32_t mfe_ladder_tick(mfe_ladder_state_t *s, const mfe_ladder_coef_t *c, int32_t x)
{
    /* feedback, then the input stage's soft saturation (the ladder's differential pair) */
    int32_t fb = s->y4;
    if (c->acid) {                                       /* high-pass the feedback: keep the bass */
        int32_t v = (int32_t)(((int64_t)(fb - s->hp) * c->Ghp) >> 30);
        int32_t lp = v + s->hp;
        s->hp = MFE_CLAMP(lp + v, -LIM, LIM);
        fb -= lp;
        x += x >> 1;                                     /* hotter into the saturating input */
    }
    int32_t u = x - (int32_t)(((int64_t)fb * c->k) >> 15);
    u = mfe_softclip(u);
    for (int i = 0; i < 4; i++) {
        int32_t G = i == 3 ? c->G4 : c->G;
        int32_t v = (int32_t)(((int64_t)(u - s->s[i]) * G) >> 30);
        int32_t y = v + s->s[i];
        s->s[i] = MFE_CLAMP(y + v, -LIM, LIM);
        u = y;
    }
    s->y4 = u;
    /* the ladder loses level as resonance rises: give back half of it, as many ladders do */
    return u + (int32_t)(((int64_t)u * c->k) >> 17);
}
