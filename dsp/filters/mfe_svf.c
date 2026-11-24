/* SPDX-License-Identifier: AGPL-3.0-only
 * Vintage Chonk Engine: trapezoidal (TPT) state-variable filter in fixed point.
 * Topology after Andrew Simper's published SVF derivation (Cytomic technical paper, 2013);
 * independent integer implementation. Coefficients Q30, signal Q15 in int32.
 * Not a model of Massive's filters: their character is a later, measured task (DSP_DESIGN.md). */
#include "mfe_internal.h"

#define STATE_LIM (4 * 32768)     /* integrator clamp: keeps self-oscillation bounded */

void mfe_svf_coef(mfe_svf_coef_t *c, uint8_t type, int32_t pitch_q8, int32_t res_q15)
{
    int64_t g = mfe_cutoff_g(pitch_q8);                  /* Q16 */
    res_q15 = MFE_CLAMP(res_q15, 0, 32767);
    /* k = 2 - 1.98 res: k = 2 is no resonance, k -> 0.02 near self-oscillation */
    int64_t k = 131072 - (((int64_t)res_q15 * 129761) >> 15);   /* Q16 */
    int64_t den = 65536 + ((g * (g + k)) >> 16);         /* Q16 */
    int64_t a1 = ((int64_t)1 << 46) / den;               /* Q30 */
    int64_t a2 = (a1 * g) >> 16;
    int64_t a3 = (a2 * g) >> 16;
    c->a1 = (int32_t)a1;
    c->a2 = (int32_t)a2;
    c->a3 = (int32_t)a3;
    c->k = (int32_t)k;
    c->type = type;
}

int32_t mfe_svf_tick(mfe_svf_state_t *s, const mfe_svf_coef_t *c, int32_t x)
{
    int32_t v3 = x - s->ic2;
    int32_t v1 = (int32_t)(((int64_t)c->a1 * s->ic1 + (int64_t)c->a2 * v3) >> 30);
    int32_t v2 = s->ic2 + (int32_t)(((int64_t)c->a2 * s->ic1 + (int64_t)c->a3 * v3) >> 30);
    int32_t ic1 = 2 * v1 - s->ic1, ic2 = 2 * v2 - s->ic2;
    s->ic1 = MFE_CLAMP(ic1, -STATE_LIM, STATE_LIM);
    s->ic2 = MFE_CLAMP(ic2, -STATE_LIM, STATE_LIM);
    switch (c->type) {
    case MFE_F_BP: return v1;
    case MFE_F_HP: return x - (int32_t)(((int64_t)c->k * v1) >> 16) - v2;
    case MFE_F_NOTCH: return x - (int32_t)(((int64_t)c->k * v1) >> 16);
    default: return v2;                                  /* LP2, LP4 stages */
    }
}
