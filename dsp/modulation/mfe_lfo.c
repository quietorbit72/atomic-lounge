/* SPDX-License-Identifier: AGPL-3.0-only
 * Vintage Chonk Engine: LFO at control rate. Output Q15, bipolar unless MFE_LFO_UNIPOLAR. */
#include "mfe_internal.h"

static void loop_range(const mfe_lfo_t *p, unsigned *a, unsigned *b)
{
    unsigned x = p->loop_start & 15u, y = p->loop_end & 15u;
    if (x > y) { unsigned t = x; x = y; y = t; }
    *a = x;
    *b = y;
}

void mfe_lfo_reset(mfe_lfo_state_t *s, const mfe_lfo_t *p)
{
    unsigned a, b;
    loop_range(p, &a, &b);
    s->step = (uint8_t)a;
    s->val = p->step_level[a] * 258;
    s->ph = (uint32_t)MFE_CLAMP(p->phase, 0, 32767) << 17;
    if (!s->rng) s->rng = 0x9E3779B9u;
    s->sh = (int32_t)(mfe_rand(&s->rng) >> 17) - 16384;
    s->sh *= 2;
}

int32_t mfe_lfo_tick(mfe_lfo_state_t *s, const mfe_lfo_t *p, int32_t rate_mod, uint32_t sync_inc)
{
    int32_t r = MFE_CLAMP(p->rate + rate_mod, 0, 32767);
    uint32_t prev = s->ph;
    s->ph += sync_inc ? sync_inc : mfe_lfo_inc(r);
    if (p->wave == MFE_W_PERFORMER || p->wave == MFE_W_STEPPER) {
        unsigned a, b;
        loop_range(p, &a, &b);
        if (s->ph < prev) s->step = (uint8_t)(s->step >= b || s->step < a ? a : s->step + 1u);
        unsigned st = s->step;
        int32_t lev = p->step_level[st];                         /* -127..127 */
        int32_t v;
        if (p->wave == MFE_W_PERFORMER) {
            v = (mfe_perf_shape(p->step_shape[st], s->ph >> 16) * lev) / 127;
        } else {
            int32_t target = lev * 258;
            int32_t g = 32767 - (int32_t)((MFE_CLAMP(p->glide, 0, 32767) * 31) >> 5);   /* glide 0: jump */
            s->val += (int32_t)(((int64_t)(target - s->val) * g) >> 15);
            v = s->val;
        }
        v = MFE_CLAMP(v, -32768, 32767);
        if (p->flags & MFE_LFO_UNIPOLAR) v = v < 0 ? 0 : v;
        return v;
    }
    int32_t v;
    uint32_t ph = s->ph;
    switch (p->wave) {
    case MFE_W_TRI: {
        uint32_t q = ph + 0x40000000u;                 /* starts at 0, rising */
        int32_t t = (int32_t)(q >> 16);                /* 0..65535 */
        v = t < 32768 ? (t * 2 - 32768) : (98303 - t * 2);
        break;
    }
    case MFE_W_SAW_UP: v = (int32_t)(ph >> 16) - 32768; break;
    case MFE_W_SAW_DOWN: v = 32767 - (int32_t)(ph >> 16); break;
    case MFE_W_SQUARE: v = ph < 0x80000000u ? 32767 : -32768; break;
    case MFE_W_SH:
        if (ph < prev) { if (!s->rng) s->rng = 1; s->sh = (int32_t)(mfe_rand(&s->rng) >> 16) - 32768; }
        v = s->sh;
        break;
    default: v = mfe_sin(ph); break;
    }
    v = MFE_CLAMP(v, -32768, 32767);
    if (p->flags & MFE_LFO_UNIPOLAR) v = (v + 32768) >> 1;
    return v;
}
