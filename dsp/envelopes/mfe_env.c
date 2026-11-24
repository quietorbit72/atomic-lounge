/* SPDX-License-Identifier: AGPL-3.0-only
 * Vintage Chonk Engine: envelope (delay, linear attack to a level, exponential decay to a level, a move to sustain,
 * exponential release; optional loop of attack+decay while held). Control rate. Level Q30. */
#include "mfe_internal.h"

#define ONE_Q30 (1 << 30)
#define FLOOR_Q30 (ONE_Q30 >> 14)          /* about -84 dB: the envelope is finished */

void mfe_env_gate(mfe_env_state_t *s, const mfe_env_t *p, int on, int legato)
{
    if (on) {
        if (legato && s->stage != MFE_ES_OFF && s->stage != MFE_ES_RELEASE) return;
        s->count = 0;
        s->stage = p->delay > 0 ? MFE_ES_DELAY : MFE_ES_ATTACK;
    } else if (s->stage != MFE_ES_OFF) {
        s->stage = MFE_ES_RELEASE;
    }
}

int32_t mfe_env_tick(mfe_env_state_t *s, const mfe_env_t *p)
{
    int32_t sus = (int32_t)MFE_CLAMP(p->sustain, 0, 32767) << 15;
    int32_t peak = p->att_level > 0 ? (int32_t)MFE_CLAMP(p->att_level, 1, 32767) << 15 : ONE_Q30;
    int32_t dlev = p->dec_level < 0 ? sus : (int32_t)MFE_CLAMP(p->dec_level, 0, 32767) << 15;
    switch (s->stage) {
    case MFE_ES_DELAY:
        if (++s->count >= mfe_time_ticks(p->delay)) { s->stage = MFE_ES_ATTACK; s->count = 0; }
        break;
    case MFE_ES_ATTACK: {
        int32_t step = (int32_t)(peak / (int32_t)mfe_time_ticks(p->attack));
        if (step < 1) step = 1;
        if (s->level >= peak - step) { s->level = peak; s->stage = MFE_ES_DECAY; }
        else s->level += step;
        break;
    }
    case MFE_ES_DECAY: {
        int32_t c = mfe_time_coef(p->decay);
        s->level = dlev + (int32_t)(((int64_t)(s->level - dlev) * c) >> 30);
        int32_t d = s->level - dlev;
        if (d < FLOOR_Q30 && d > -FLOOR_Q30) {
            if (p->flags & MFE_ENV_LOOP) s->stage = MFE_ES_ATTACK;
            else { s->level = dlev; s->stage = dlev == sus ? MFE_ES_SUSTAIN : MFE_ES_SUSMOVE; }
        }
        break;
    }
    case MFE_ES_SUSMOVE: {                  /* Massive's sustain time: from the decay level to sustain */
        int32_t c = mfe_time_coef(p->sust_time);
        s->level = sus + (int32_t)(((int64_t)(s->level - sus) * c) >> 30);
        int32_t d = s->level - sus;
        if (d < FLOOR_Q30 && d > -FLOOR_Q30) { s->level = sus; s->stage = MFE_ES_SUSTAIN; }
        break;
    }
    case MFE_ES_SUSTAIN:
        s->level = sus;                     /* follows live sustain edits */
        break;
    case MFE_ES_RELEASE: {
        int32_t c = mfe_time_coef(p->release);
        s->level = (int32_t)(((int64_t)s->level * c) >> 30);
        if (s->level < FLOOR_Q30) { s->level = 0; s->stage = MFE_ES_OFF; }
        break;
    }
    default:
        s->level = 0;
    }
    return s->level >> 15;
}
