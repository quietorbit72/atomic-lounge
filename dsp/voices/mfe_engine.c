/* SPDX-License-Identifier: AGPL-3.0-only
 * Vintage Chonk Engine: voices, modulation matrix, MIDI and the block render.
 *
 * Voice signal path (per sample):
 *   osc 1..3 (wavetable, position morph, phase warp) + noise
 *     -> split by each source's ROUTE into filter 1 / filter 2 inputs
 *     -> F1;  F2 input += F1 out * SERPAR  (0 parallel .. 1 serial, continuous)
 *     -> out = F1 * F1MIX * (1 - SERPAR) + F2 * F2MIX
 *     -> amplitude (ENV 4 x velocity x matrix) -> pan  (drive: the master TAPE effect, MFE_D_DRIVE below)
 * Control rate (every MFE_CTL samples): envelopes, LFOs, the 16-slot matrix, pitches, cutoffs. */
#include <string.h>
#include "mfe_internal.h"
#include "mfe_seq.h"

#define NOTE_STACK 16

typedef struct {
    uint8_t note, vel, gate, active, sustained;
    uint8_t slide;                       /* gliding to `target` because of a sequencer slide */
    uint32_t age;
    int32_t pitch, target;               /* 1/256 semitone */
    uint32_t ph[MFE_NOSC];
    uint32_t nrng;
    int32_t nlp;
    int32_t rnd;
    mfe_env_state_t env[MFE_NENV];
    mfe_lfo_state_t lfo[MFE_NLFO];
    mfe_svf_state_t svf[MFE_NFILT][2];
    mfe_svf_coef_t cf[MFE_NFILT][2];
    mfe_ladder_state_t lad[MFE_NFILT];
    mfe_ladder_coef_t lc[MFE_NFILT];
    /* per control tick */
    uint32_t inc[MFE_NOSC];
    int32_t drift[MFE_NOSC], drift_to[MFE_NOSC];   /* analogue drift, 1/(256*64) semitone */
    int32_t osc3_mod;                    /* osc 3 output at the last tick (MFE_S_OSC3), Q15 */
    uint32_t mph, minc;                  /* modulation oscillator phase and increment */
    uint32_t knee[MFE_NOSC];
    int32_t pos[MFE_NOSC], oamp[MFE_NOSC];
    int32_t namp, ncoef, serpar, drive;  /* drive: the MFE_D_DRIVE matrix sum (to the TAPE drive) */
    int32_t amp_prev, amp, gl, gr;
} voice_t;

struct mfe_engine {
    mfe_patch_t p;
    voice_t v[MFE_MAX_VOICES];
    uint8_t stack[NOTE_STACK], nstack;
    uint8_t sustain;
    int32_t modwheel, bend, aftertouch;  /* Q15 (bend bipolar) */
    int32_t macro[MFE_NMACRO];
    uint32_t age, rng;
    uint32_t bpm_q8;                     /* tempo for synced LFOs */
    uint8_t macros_live;                 /* set once a first patch has been applied */
    int32_t last_pitch;
    mfe_fx_state_t *fx;
    mfe_seq_t *seq;                      /* step sequencer, ticked at every control tick (NULL: none) */
    int32_t mix[MFE_BLOCK * 2];
    /* fx state follows the struct (mfe_engine_size) */
};

size_t mfe_engine_size(void)
{
    return (sizeof(mfe_engine_t) + 15u) / 16u * 16u + mfe_fx_size();
}

void mfe_patch_init(mfe_patch_t *p)
{
    memset(p, 0, sizeof *p);
    memcpy(p->name, "INIT", 5);
    p->voice_mode = MFE_POLY;
    p->max_voices = MFE_MAX_VOICES;
    p->bend_range = 2;
    p->vel_sens = 64;
    p->master = 8192;                   /* unity (Q13) */
    p->osc[0].flags = MFE_OSC_ON;
    p->osc[0].table = MFE_WT_MOOG;       /* 16 Nov 2026: classic Moog-style oscillators by default */
    p->osc[0].position = 14043;          /* frame 3 = saw (3/7) */
    p->osc[0].amp = 26000;
    for (int i = 1; i < MFE_NOSC; i++) { p->osc[i].table = MFE_WT_MOOG; p->osc[i].position = 14043; p->osc[i].amp = 26000; }
    p->filt[0].type = MFE_F_LP2;
    p->filt[0].cutoff = 30000;
    p->f1_mix = 32767;
    p->f2_mix = 32767;
    for (int i = 0; i < MFE_NENV; i++) {
        p->env[i].attack = 2000;
        p->env[i].decay = 16000;
        p->env[i].sustain = 32767;
        p->env[i].release = 12000;
        p->env[i].att_level = 32767;
        p->env[i].dec_level = -1;           /* plain ADSR: decay straight to sustain */
    }
    for (int i = 0; i < MFE_NLFO; i++) {
        p->lfo[i].rate = 16000;
        p->lfo[i].sync_div = 8;             /* a quarter note */
        p->lfo[i].loop_end = MFE_PERF_STEPS - 1;
        for (int k = 0; k < MFE_PERF_STEPS; k++) p->lfo[i].step_level[k] = 127;
    }
    for (int i = 0; i < MFE_NMACRO; i++) p->macro[i] = 0;
    mfe_fx_defaults(&p->fx);
}

void mfe_engine_init(mfe_engine_t *e)
{
    memset(e, 0, sizeof *e);
    e->fx = (mfe_fx_state_t *)((uint8_t *)e + (sizeof(mfe_engine_t) + 15u) / 16u * 16u);
    mfe_fx_init(e->fx);
    e->rng = 0x1234567u;
    e->last_pitch = 60 << 8;
    e->bpm_q8 = 120u << 8;
    mfe_patch_t p;
    mfe_patch_init(&p);
    mfe_engine_set_patch(e, &p);
}

void mfe_engine_set_patch(mfe_engine_t *e, const mfe_patch_t *p)
{
    int16_t before[MFE_NMACRO];
    memcpy(before, e->p.macro, sizeof before);
    e->p = *p;
    if (e->p.max_voices == 0 || e->p.max_voices > MFE_MAX_VOICES) e->p.max_voices = MFE_MAX_VOICES;
    /* a macro moved live (MIDI CC 16-23) keeps its value unless this patch changes that macro */
    for (int i = 0; i < MFE_NMACRO; i++)
        if (!e->macros_live || p->macro[i] != before[i]) e->macro[i] = p->macro[i];
    e->macros_live = 1;
}

void mfe_engine_attach_seq(mfe_engine_t *e, mfe_seq_t *s) { e->seq = s; }

void mfe_set_tempo(mfe_engine_t *e, uint32_t bpm_q8)
{
    e->bpm_q8 = MFE_CLAMP(bpm_q8, 20u << 8, 400u << 8);
    mfe_fx_set_tempo(e->fx, e->bpm_q8);          /* synced delays glide to the new time */
}

/* phase increment per control tick for one cycle (or step) of `div` at the current tempo */
static uint32_t sync_inc(const mfe_engine_t *e, unsigned div)
{
    uint32_t num, den;
    mfe_sync_beats(div, &num, &den);
    /* 2^32 * (bpm / 60) * CTL / SR * den / num, with bpm in Q8 */
    uint64_t n = ((uint64_t)e->bpm_q8 * MFE_CTL * den) << 24;
    return (uint32_t)(n / ((uint64_t)60 * MFE_SR * num));
}

unsigned mfe_active_voices(const mfe_engine_t *e)
{
    unsigned n = 0;
    for (unsigned i = 0; i < MFE_MAX_VOICES; i++) n += e->v[i].active;
    return n;
}

/* ------------------------------------------------------------- voices --- */
static void voice_start(mfe_engine_t *e, voice_t *v, uint8_t note, uint8_t vel, int legato)
{
    const mfe_patch_t *p = &e->p;
    int was_active = v->active;
    v->note = note;
    v->vel = vel;
    v->gate = 1;
    v->sustained = 0;
    v->slide = 0;
    v->age = ++e->age;
    v->target = note << 8;
    if (!was_active || p->glide == 0) v->pitch = p->glide ? e->last_pitch : v->target;
    if (!legato) {
        v->rnd = (int32_t)(mfe_rand(&e->rng) >> 17);
        for (int i = 0; i < MFE_NOSC; i++)
            if ((p->osc[i].flags & MFE_OSC_RESET) || !was_active) v->ph[i] = (p->osc[i].flags & MFE_OSC_RESET) ? 0 : mfe_rand(&e->rng);
        for (int i = 0; i < MFE_NLFO; i++)
            if ((p->lfo[i].flags & MFE_LFO_KEYSYNC) || !was_active) { v->lfo[i].rng = mfe_rand(&e->rng) | 1u; mfe_lfo_reset(&v->lfo[i], &p->lfo[i]); }
        if (!was_active) {
            memset(v->svf, 0, sizeof v->svf);
            memset(v->lad, 0, sizeof v->lad);
            v->nrng = mfe_rand(&e->rng) | 1u;
            v->amp_prev = v->amp = 0;
        }
    }
    for (int i = 0; i < MFE_NENV; i++) mfe_env_gate(&v->env[i], &p->env[i], 1, legato);
    v->active = 1;
    e->last_pitch = v->target;
}

static void voice_release(mfe_engine_t *e, voice_t *v)
{
    v->gate = 0;
    for (int i = 0; i < MFE_NENV; i++) mfe_env_gate(&v->env[i], &e->p.env[i], 0, 0);
}

static voice_t *alloc_voice(mfe_engine_t *e, uint8_t note)
{
    unsigned n = e->p.max_voices;
    voice_t *best = 0;
    for (unsigned i = 0; i < n; i++)                 /* the same note again: reuse its voice */
        if (e->v[i].active && e->v[i].note == note) return &e->v[i];
    for (unsigned i = 0; i < n; i++)
        if (!e->v[i].active) return &e->v[i];
    for (unsigned i = 0; i < n; i++)                 /* steal: oldest released, else oldest */
        if (!e->v[i].gate && !e->v[i].sustained && (!best || e->v[i].age < best->age)) best = &e->v[i];
    if (best) return best;
    for (unsigned i = 0; i < n; i++)
        if (!best || e->v[i].age < best->age) best = &e->v[i];
    return best;
}

static void stack_remove(mfe_engine_t *e, uint8_t note)
{
    unsigned j = 0;
    for (unsigned i = 0; i < e->nstack; i++)
        if (e->stack[i] != note) e->stack[j++] = e->stack[i];
    e->nstack = (uint8_t)j;
}

void mfe_note_on(mfe_engine_t *e, uint8_t note, uint8_t vel)
{
    note &= 127;
    if (vel == 0) { mfe_note_off(e, note); return; }
    if (e->p.voice_mode == MFE_POLY) {
        voice_start(e, alloc_voice(e, note), note, vel, 0);
        return;
    }
    stack_remove(e, note);
    if (e->nstack == NOTE_STACK) { memmove(e->stack, e->stack + 1, NOTE_STACK - 1); e->nstack--; }
    e->stack[e->nstack++] = note;
    voice_t *v = &e->v[0];
    int legato = e->p.voice_mode == MFE_LEGATO && v->active && v->gate;
    if (legato) {
        v->note = note;
        v->target = note << 8;
        if (!e->p.glide) v->pitch = v->target;
        e->last_pitch = v->target;
    } else {
        voice_start(e, v, note, vel, 0);
    }
}

void mfe_note_off(mfe_engine_t *e, uint8_t note)
{
    note &= 127;
    if (e->p.voice_mode != MFE_POLY) {
        voice_t *v = &e->v[0];
        stack_remove(e, note);
        for (unsigned i = 1; i < MFE_MAX_VOICES; i++)   /* voices left from a POLY patch before a patch change */
            if (e->v[i].active && e->v[i].gate && e->v[i].note == note) voice_release(e, &e->v[i]);
        if (!v->active || v->note != note) return;
        if (e->nstack) {                            /* back to the previous held note, legato */
            v->note = e->stack[e->nstack - 1];
            v->target = v->note << 8;
            if (!e->p.glide) v->pitch = v->target;
            return;
        }
        if (e->sustain) v->sustained = 1, v->gate = 0;
        else voice_release(e, v);
        return;
    }
    for (unsigned i = 0; i < MFE_MAX_VOICES; i++) {
        voice_t *v = &e->v[i];
        if (v->active && v->gate && v->note == note) {
            if (e->sustain) { v->sustained = 1; v->gate = 0; }
            else voice_release(e, v);
        }
    }
}

/* the sequencer's slide (303): move the held note `from` to `to`, legato, with glide */
void mfe_note_slide(mfe_engine_t *e, uint8_t from, uint8_t to, uint8_t vel)
{
    from &= 127; to &= 127;
    voice_t *v = 0;
    for (unsigned i = 0; i < MFE_MAX_VOICES; i++)
        if (e->v[i].active && e->v[i].gate && e->v[i].note == from) { v = &e->v[i]; break; }
    if (!v) { mfe_note_on(e, to, vel); return; }
    if (e->p.voice_mode != MFE_POLY) {
        stack_remove(e, from);
        if (e->nstack == NOTE_STACK) { memmove(e->stack, e->stack + 1, NOTE_STACK - 1); e->nstack--; }
        e->stack[e->nstack++] = to;
    }
    v->note = to;
    v->target = to << 8;
    v->slide = 1;
    e->last_pitch = v->target;
}

void mfe_all_notes_off(mfe_engine_t *e)
{
    e->nstack = 0;
    e->sustain = 0;
    for (unsigned i = 0; i < MFE_MAX_VOICES; i++)
        if (e->v[i].active) { e->v[i].sustained = 0; voice_release(e, &e->v[i]); }
}

void mfe_midi(mfe_engine_t *e, const uint8_t *m, size_t len)
{
    if (len < 2) return;
    uint8_t st = m[0] & 0xF0;
    uint8_t d1 = m[1] & 127, d2 = len > 2 ? m[2] & 127 : 0;
    switch (st) {
    case 0x90: mfe_note_on(e, d1, d2); break;
    case 0x80: mfe_note_off(e, d1); break;
    case 0xD0: e->aftertouch = d1 * 258; break;
    case 0xE0: e->bend = (((int32_t)d2 << 7 | d1) - 8192) * 4; break;
    case 0xB0:
        if (d1 == 1) e->modwheel = d2 * 258;
        else if (d1 >= 16 && d1 <= 23) e->macro[d1 - 16] = d2 * 258;    /* macros 1..8 on CC 16..23 */
        else if (d1 == 64) {
            int on = d2 >= 64;
            if (e->sustain && !on)
                for (unsigned i = 0; i < MFE_MAX_VOICES; i++)
                    if (e->v[i].active && e->v[i].sustained) { e->v[i].sustained = 0; voice_release(e, &e->v[i]); }
            e->sustain = (uint8_t)on;
        } else if (d1 == 120 || d1 == 123) mfe_all_notes_off(e);
        break;
    }
}

/* ------------------------------------------------------------ control --- */
static void voice_control(mfe_engine_t *e, voice_t *v)
{
    const mfe_patch_t *p = &e->p;
    int32_t src[MFE_S_COUNT] = {0}, acc[MFE_D_COUNT] = {0};
    for (int i = 0; i < MFE_NENV; i++) src[MFE_S_ENV1 + i] = mfe_env_tick(&v->env[i], &p->env[i]);
    /* LFO rate destinations use the previous tick's matrix: computed below, applied here next tick */
    for (int i = 0; i < MFE_NLFO; i++) {
        uint32_t si = (p->lfo[i].flags & MFE_LFO_SYNC) ? sync_inc(e, p->lfo[i].sync_div) : 0;
        src[MFE_S_LFO1 + i] = mfe_lfo_tick(&v->lfo[i], &p->lfo[i], 0, si);
    }
    src[MFE_S_VEL] = v->vel * 258;
    src[MFE_S_KEY] = ((int32_t)v->note - 60) * 512;
    src[MFE_S_MODWHEEL] = e->modwheel;
    src[MFE_S_BEND] = e->bend;
    src[MFE_S_AFTERTOUCH] = e->aftertouch;
    for (int i = 0; i < MFE_NMACRO; i++) src[MFE_S_MACRO1 + i] = e->macro[i];
    src[MFE_S_RANDOM] = v->rnd;
    src[MFE_S_ONE] = 32767;
    src[MFE_S_OSC3] = v->osc3_mod;
    for (int i = 0; i < MFE_NMOD; i++) {
        const mfe_mod_t *m = &p->mod[i];
        if (m->src && m->src < MFE_S_COUNT && m->dst && m->dst < MFE_D_COUNT)
            acc[m->dst] += mfe_mul15(src[m->src], m->amount);
    }
    /* glide */
    if (v->pitch != v->target) {
        if (p->glide || v->slide) {
            int32_t c = mfe_time_coef(p->glide ? p->glide : MFE_SLIDE_TIME_Q15);
            v->pitch = v->target + (int32_t)(((int64_t)(v->pitch - v->target) * c) >> 30);
            if (v->pitch - v->target < 4 && v->target - v->pitch < 4) v->pitch = v->target;
        } else v->pitch = v->target;
    }
    if (v->pitch == v->target) v->slide = 0;
    int32_t base = v->pitch + ((e->bend * p->bend_range) >> 7) + ((acc[MFE_D_PITCH] * MFE_PITCH_MOD_RANGE) >> 7);
    for (int i = 0; i < MFE_NOSC; i++) {
        const mfe_osc_t *o = &p->osc[i];
        /* analogue drift: a slow random walk towards a new target every so often */
        if (o->drift > 0) {
            int32_t span = (o->drift * 31 / 127) * 64;          /* 127 -> 31/256 st = about 12 cents */
            int32_t d = v->drift_to[i] - v->drift[i];
            if (d < 2 && d > -2) v->drift_to[i] = (int32_t)(mfe_rand(&e->rng) % (uint32_t)(2 * span + 1)) - span;
            v->drift[i] += d > 0 ? 1 : d < 0 ? -1 : 0;           /* one step a tick: a wander of ~1.5 s */
        } else v->drift[i] = v->drift_to[i] = 0;
        int32_t keyb = base;
        if (o->flags & MFE_OSC_LO) keyb = (60 << 8) - 84 * 256 + ((acc[MFE_D_PITCH] * MFE_PITCH_MOD_RANGE) >> 7);
        if (i == 0) v->minc = mfe_pitch_inc(base + p->modosc.semis * 256);
        int32_t pt = keyb + (v->drift[i] >> 6) + o->semis * 256 + o->fine * 256 / 100 + ((acc[MFE_D_OSC1_PITCH + i] * MFE_PITCH_MOD_RANGE) >> 7);
        v->inc[i] = mfe_pitch_inc(pt);
        v->pos[i] = MFE_CLAMP(o->position + acc[MFE_D_OSC1_POS + i], 0, 32767);
        int32_t w = MFE_CLAMP(o->warp + acc[MFE_D_OSC1_WARP + i], 0, 32767);
        v->knee[i] = 0x80000000u - (uint32_t)(((int64_t)w * 29491) >> 15) * 65536u;   /* 0.5 .. 0.05 */
        v->oamp[i] = (o->flags & MFE_OSC_ON) ? MFE_CLAMP(o->amp + acc[MFE_D_OSC1_AMP + i], 0, 32767) : 0;
    }
    if (!v->oamp[2]) v->ph[2] += v->inc[2] * MFE_CTL;   /* silent osc 3 still runs (MFE_S_OSC3) */
    {
        const mfe_osc_t *o3 = &p->osc[2];
        unsigned t = o3->table < MFE_WT_COUNT ? o3->table : 0;
        int32_t fp = MFE_CLAMP(o3->position, 0, 32767) * (MFE_WT_FRAMES - 1);
        unsigned fi = (unsigned)(fp >> 15);
        if (fi > MFE_WT_FRAMES - 1) fi = MFE_WT_FRAMES - 1;
        const int16_t *w = mfe_wt_frame(t, fi, MFE_WT_LEVELS - 3);   /* 4 harmonics: smooth enough */
        v->osc3_mod = w[v->ph[2] >> (32u - MFE_WT_BITS[MFE_WT_LEVELS - 3])];
    }
    v->namp = MFE_CLAMP(p->noise_amp + acc[MFE_D_NOISE_AMP], 0, 32767);
    v->ncoef = 32767 - MFE_CLAMP(p->noise_color, 0, 32000);
    for (int f = 0; f < MFE_NFILT; f++) {
        const mfe_filter_t *fl = &p->filt[f];
        if (fl->type == MFE_F_OFF) continue;
        int32_t cut = (int32_t)(((int64_t)(fl->cutoff + acc[MFE_D_F1_CUTOFF + f]) * (136 * 256)) >> 15);
        cut += (int32_t)(((int64_t)fl->keytrack * (((int32_t)v->note - 48) * 256)) >> 15);
        int32_t res = fl->res + acc[MFE_D_F1_RES + f];
        if (fl->type == MFE_F_LADDER) {
            mfe_ladder_coef(&v->lc[f], cut, res);
        } else if (fl->type == MFE_F_ACID) {
            mfe_acid_coef(&v->lc[f], cut, res);
        } else if (fl->type == MFE_F_LP4) {
            mfe_svf_coef(&v->cf[f][0], MFE_F_LP2, cut, 9000);        /* first stage near Butterworth */
            mfe_svf_coef(&v->cf[f][1], MFE_F_LP2, cut, res);
        } else {
            mfe_svf_coef(&v->cf[f][0], fl->type, cut, res);
        }
    }
    v->serpar = MFE_CLAMP(p->serpar + acc[MFE_D_SERPAR], 0, 32767);
    v->drive = acc[MFE_D_DRIVE];
    /* amplitude: ENV 4 x velocity x matrix */
    int32_t vs = p->vel_sens * 258;
    int32_t velg = 32767 - vs + mfe_mul15(vs, src[MFE_S_VEL]);
    int32_t a = mfe_mul15(src[MFE_S_ENV4], velg);
    a = mfe_mul15(a, MFE_CLAMP(32767 + acc[MFE_D_AMP], 0, 65535));
    v->amp_prev = v->amp;
    v->amp = a;
    int32_t pan = MFE_CLAMP(acc[MFE_D_PAN], -32768, 32767);
    uint32_t ang = (uint32_t)(pan + 32768) << 14;                   /* 0 .. quarter turn (centre: 45 deg) */
    v->gl = mfe_sin(ang + 0x40000000u);
    v->gr = mfe_sin(ang);
    if (v->env[MFE_NENV - 1].stage == MFE_ES_OFF) v->active = 0;
}

static inline int32_t filt_run(voice_t *v, const mfe_patch_t *p, int f, int32_t x)
{
    const mfe_filter_t *fl = &p->filt[f];
    if (fl->type == MFE_F_OFF) return x;
    if (fl->drive > 0) x = mfe_softclip(x + (int32_t)(((int64_t)x * fl->drive * 7) >> 15));
    if (fl->type == MFE_F_LADDER || fl->type == MFE_F_ACID) return mfe_ladder_tick(&v->lad[f], &v->lc[f], x);
    x = mfe_svf_tick(&v->svf[f][0], &v->cf[f][0], x);
    if (fl->type == MFE_F_LP4) x = mfe_svf_tick(&v->svf[f][1], &v->cf[f][1], x);
    return x;
}

static void voice_render(mfe_engine_t *e, voice_t *v, int32_t *mix, unsigned n)
{
    const mfe_patch_t *p = &e->p;
    const int16_t *fr0[MFE_NOSC], *fr1[MFE_NOSC];
    uint32_t sh[MFE_NOSC] = {24, 24, 24};
    int32_t ff[MFE_NOSC], r2[MFE_NOSC];
    uint32_t s1[MFE_NOSC], s2[MFE_NOSC];
    for (int i = 0; i < MFE_NOSC; i++) {
        unsigned t = p->osc[i].table < MFE_WT_COUNT ? p->osc[i].table : 0;
        unsigned L = mfe_wt_level(v->inc[i]);
        int32_t fp = v->pos[i] * (MFE_WT_FRAMES - 1);                 /* frame position, Q15 */
        int32_t fi = fp >> 15;
        if (fi >= MFE_WT_FRAMES - 1) { fi = MFE_WT_FRAMES - 2; ff[i] = 32767; } else ff[i] = fp & 32767;
        fr0[i] = mfe_wt_frame(t, (unsigned)fi, L);
        fr1[i] = mfe_wt_frame(t, (unsigned)fi + 1, L);
        sh[i] = 32u - MFE_WT_BITS[L];
        r2[i] = p->osc[i].route;
        /* phase warp slopes (Q16 per 2^-16 of phase) for the two segments around the knee */
        s1[i] = (uint32_t)((0x80000000ull << 16) / v->knee[i]);
        s2[i] = (uint32_t)((0x80000000ull << 16) / (0x100000000ull - v->knee[i]));
    }
    int32_t da = (v->amp - v->amp_prev) / (int32_t)n, a = v->amp_prev;
    const mfe_modosc_t *mo = &p->modosc;
    int mrm = mo->rm_osc && mo->rm ? mo->rm_osc - 1 : -1, mpm = mo->pm_osc && mo->pm ? mo->pm_osc - 1 : -1;
    int mpos = mo->pos_osc && mo->pos ? mo->pos_osc - 1 : -1;
    int use_mod = mrm >= 0 || mpm >= 0 || mpos >= 0;
    unsigned Lpos = mpos >= 0 ? mfe_wt_level(v->inc[mpos]) : 0;
    unsigned tpos = mpos >= 0 && p->osc[mpos].table < MFE_WT_COUNT ? p->osc[mpos].table : 0;
    int32_t nr2 = p->noise_route;
    for (unsigned s = 0; s < n; s++) {
        int32_t in1 = 0, in2 = 0, msig = 0;
        if (use_mod) msig = mfe_sin(v->mph += v->minc);
        for (int i = 0; i < MFE_NOSC; i++) {
            if (!v->oamp[i]) continue;
            uint32_t ph = v->ph[i] += v->inc[i];
            if (i == mpm) ph += (uint32_t)(((int64_t)msig * mo->pm) << 2);   /* phase mod: up to +-2 cycles */
            if (i == mpos) {                                                   /* position mod, per sample */
                int32_t pq = MFE_CLAMP(v->pos[i] + (int32_t)(((int64_t)msig * mo->pos) >> 15), 0, 32767);
                int32_t fp = pq * (MFE_WT_FRAMES - 1), fi = fp >> 15;
                if (fi >= MFE_WT_FRAMES - 1) { fi = MFE_WT_FRAMES - 2; ff[i] = 32767; } else ff[i] = fp & 32767;
                fr0[i] = mfe_wt_frame(tpos, (unsigned)fi, Lpos);
                fr1[i] = mfe_wt_frame(tpos, (unsigned)fi + 1, Lpos);
            }
            uint32_t k = v->knee[i];
            if (k != 0x80000000u)
                ph = ph < k ? (uint32_t)(((uint64_t)ph * s1[i]) >> 16)
                            : 0x80000000u + (uint32_t)(((uint64_t)(ph - k) * s2[i]) >> 16);
            uint32_t idx = ph >> sh[i], nx = (idx + 1) & ((1u << (32u - sh[i])) - 1u);
            int32_t fr = (int32_t)((ph >> (sh[i] - 15u)) & 0x7FFF);
            int32_t a0 = fr0[i][idx] + (((fr0[i][nx] - fr0[i][idx]) * fr) >> 15);
            int32_t a1 = fr1[i][idx] + (((fr1[i][nx] - fr1[i][idx]) * fr) >> 15);
            int32_t x = mfe_mul15(a0 + mfe_mul15(a1 - a0, ff[i]), v->oamp[i]);   /* full level: the output limiter guards peaks */
            if (i == mrm) x = mfe_mul15(x, 32767 - mo->rm) + mfe_mul15(mfe_mul15(x, msig), mo->rm);   /* ring mod */
            int32_t x2 = mfe_mul15(x, r2[i]);
            in1 += x - x2;
            in2 += x2;
        }
        if (v->namp) {
            int32_t w = (int32_t)(mfe_rand(&v->nrng) >> 16) - 32768;
            v->nlp += mfe_mul15(w - v->nlp, v->ncoef);
            int32_t x = mfe_mul15(v->nlp, v->namp), x2 = mfe_mul15(x, nr2);
            in1 += x - x2;
            in2 += x2;
        }
        int32_t y1 = filt_run(v, p, 0, in1);
        int32_t y2 = filt_run(v, p, 1, in2 + mfe_mul15(y1, v->serpar));
        int32_t y = mfe_mul15(y1, mfe_mul15(p->f1_mix, 32767 - v->serpar)) + mfe_mul15(y2, p->f2_mix);
        a += da;
        y = mfe_mul15(y, a);
        mix[2 * s] += mfe_mul15(y, v->gl);
        mix[2 * s + 1] += mfe_mul15(y, v->gr);
    }
}

void mfe_render(mfe_engine_t *e, int32_t *out)
{
    memset(e->mix, 0, sizeof e->mix);
    for (unsigned t = 0; t < MFE_BLOCK; t += MFE_CTL) {
        if (e->seq) mfe_seq_ctl(e->seq, e);      /* step sequencer: notes start on this control tick */
        for (unsigned i = 0; i < MFE_MAX_VOICES; i++) {
            voice_t *v = &e->v[i];
            if (!v->active) continue;
            voice_control(e, v);
            voice_render(e, v, e->mix + 2 * t, MFE_CTL);
        }
    }
    for (unsigned i = 0; i < MFE_BLOCK * 2; i++) e->mix[i] = (int32_t)(((int64_t)e->mix[i] * e->p.master) >> 13);
    /* MFE_D_DRIVE (a per-voice matrix destination) drives the master TAPE: the newest sounding voice's
     * modulation is added to the preset's tape drive (17 Nov 2026: the insert drive became TAPE) */
    mfe_fx_t fx = e->p.fx;
    const voice_t *nv = 0;
    for (unsigned i = 0; i < MFE_MAX_VOICES; i++)
        if (e->v[i].active && (!nv || e->v[i].age > nv->age)) nv = &e->v[i];
    if (nv && nv->drive) fx.tape_drive = (int16_t)MFE_CLAMP(fx.tape_drive + nv->drive, 0, 32767);
    mfe_fx_process(e->fx, &fx, e->mix, MFE_BLOCK);
    /* soft-knee limiter: linear up to 0.75 FS, then 0.75 + 0.25 * tanh(excess / 0.25): never clips */
    for (unsigned i = 0; i < MFE_BLOCK * 2; i++) {
        int32_t x = e->mix[i], a = x < 0 ? -x : x;
        if (a > 24576) a = 24576 + ((8191 * mfe_softclip((a - 24576) * 4)) >> 15);
        out[i] = mfe_sat16(x < 0 ? -a : a);
    }
}
