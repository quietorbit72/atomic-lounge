/* SPDX-License-Identifier: AGPL-3.0-only
 * Vintage Chonk step sequencer: the calls of the control task (dsp/include/mfe_seq.h). The audio side is
 * mfe_seq.c; this file is kept apart so the FM1 build can leave it in Flash (only mfe_seq.c needs RAM). */
#include "mfe_seq.h"

void mfe_seq_pattern_clear(mfe_seq_pattern_t *p)
{
    uint8_t *b = (uint8_t *)p;
    for (unsigned i = 0; i < sizeof *p; i++) b[i] = 0;
    p->length = 16;
    p->rate = MFE_SEQ_RATE_16;
    p->swing = 50;
    for (unsigned i = 0; i < MFE_SEQ_STEPS; i++) { p->step[i].note = 48; p->step[i].gate = MFE_SEQ_GATE_REST; }
}

int mfe_seq_pattern_valid(const mfe_seq_pattern_t *p)
{
    if (p->length < 1 || p->length > MFE_SEQ_STEPS || p->rate >= MFE_SEQ_RATE_COUNT || p->swing < 50 || p->swing > 75)
        return 0;
    for (unsigned i = 0; i < MFE_SEQ_STEPS; i++) {
        unsigned g = mfe_seq_gate(p->step[i]);
        if (g > 100 && g != MFE_SEQ_GATE_TIE) return 0;
    }
    return 1;
}

void mfe_seq_init(mfe_seq_t *s)
{
    uint8_t *b = (uint8_t *)s;
    for (unsigned i = 0; i < sizeof *s; i++) b[i] = 0;
    for (unsigned i = 0; i < MFE_SEQ_SLOTS; i++) mfe_seq_pattern_clear(&s->pat[i]);
    s->cur = -1;
    s->sounding = 0xFFu;
    s->clock_in = 1;
    s->off_at = 0xFFFFFFFFu;
    s->tempo_q8 = 120u << 8;                 /* as set_tempo() in mfe_seq.c */
    s->inc_q = (120u << 24) / 55125u;
    s->inc_r = (120u << 24) % 55125u;
}

int mfe_seq_cmd(mfe_seq_t *s, uint8_t type, uint8_t a, int16_t b)
{
    uint8_t w = s->cmd_w;
    if ((uint8_t)(w - s->cmd_r) >= 16u) return -1;
    s->cmd[w & 15u].type = type;
    s->cmd[w & 15u].a = a;
    s->cmd[w & 15u].b = b;
    __asm__ volatile("" ::: "memory");
    s->cmd_w = (uint8_t)(w + 1u);
    return 0;
}
