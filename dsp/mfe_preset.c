/* SPDX-License-Identifier: AGPL-3.0-only
 * Vintage Chonk Engine: the FM1 binary preset (.mfp), format version 1.
 * Little-endian, fields in the order below, CRC-32 (IEEE) of everything before it at the end.
 * tools/preset_converter/mfp.py writes the same layout; tests/integration checks they agree. */
#include <string.h>
#include "mfe_internal.h"

typedef struct { uint8_t *b; const uint8_t *r; size_t n, cap; int err; unsigned ver; } io_t;

static void w8(io_t *o, uint32_t v) { if (o->n < o->cap) o->b[o->n] = (uint8_t)v; else o->err = 1; o->n++; }
static void w16(io_t *o, int32_t v) { w8(o, (uint32_t)v & 255u); w8(o, ((uint32_t)v >> 8) & 255u); }
static uint32_t r8(io_t *o) { if (o->n >= o->cap) { o->err = 1; return 0; } return o->r[o->n++]; }
static int16_t r16(io_t *o) { uint32_t lo = r8(o), hi = r8(o); return (int16_t)(uint16_t)(lo | hi << 8); }

static uint32_t crc32(const uint8_t *d, size_t n)
{
    uint32_t c = 0xFFFFFFFFu;
    for (size_t i = 0; i < n; i++) {
        c ^= d[i];
        for (int k = 0; k < 8; k++) c = (c >> 1) ^ (0xEDB88320u & (0u - (c & 1u)));
    }
    return ~c;
}

/* one walker for both directions keeps the layout in a single place */
#define F8(x) do { if (wr) w8(o, (uint8_t)(x)); else (x) = (__typeof__(x))r8(o); } while (0)
#define F16(x) do { if (wr) w16(o, (x)); else (x) = r16(o); } while (0)

static void walk(io_t *o, mfe_patch_t *p, int wr)
{
    for (int i = 0; i < MFE_NAME_LEN; i++) F8(p->name[i]);
    F8(p->voice_mode); F8(p->max_voices); F8(p->bend_range); F8(p->vel_sens);
    F16(p->glide); F16(p->master);
    for (int i = 0; i < MFE_NMACRO; i++) F16(p->macro[i]);
    for (int i = 0; i < MFE_NOSC; i++) {
        mfe_osc_t *s = &p->osc[i];
        F8(s->table); F8(s->flags); F8(s->semis); F8(s->drift);
        F16(s->fine); F16(s->position); F16(s->warp); F16(s->amp); F16(s->route);
    }
    F16(p->noise_amp); F16(p->noise_color); F16(p->noise_route);
    for (int i = 0; i < MFE_NFILT; i++) {
        mfe_filter_t *f = &p->filt[i];
        F8(f->type); F8(f->reserved); F16(f->cutoff); F16(f->res); F16(f->drive); F16(f->keytrack);
    }
    F16(p->serpar); F16(p->f1_mix); F16(p->f2_mix);
    for (int i = 0; i < MFE_NENV; i++) {
        mfe_env_t *v = &p->env[i];
        F16(v->delay); F16(v->attack); F16(v->decay); F16(v->sustain); F16(v->release);
        F8(v->flags); F8(v->reserved);
        if (o->ver >= 2) { F16(v->att_level); F16(v->dec_level); F16(v->sust_time); }
        else if (!wr) { v->att_level = 32767; v->dec_level = -1; v->sust_time = 0; }
    }
    for (int i = 0; i < MFE_NLFO; i++) {
        mfe_lfo_t *l = &p->lfo[i];
        F8(l->wave); F8(l->flags); F16(l->rate); F16(l->phase);
        if (o->ver >= 3) {
            F8(l->sync_div); F8(l->loop_start); F8(l->loop_end); F8(l->reserved); F16(l->glide);
            for (int k = 0; k < MFE_PERF_STEPS; k++) F8(l->step_shape[k]);
            for (int k = 0; k < MFE_PERF_STEPS; k++) F8(l->step_level[k]);
        } else if (!wr) {
            l->sync_div = 8; l->loop_start = 0; l->loop_end = MFE_PERF_STEPS - 1; l->glide = 0;
            for (int k = 0; k < MFE_PERF_STEPS; k++) { l->step_shape[k] = 0; l->step_level[k] = 127; }
        }
    }
    for (int i = 0; i < MFE_NMOD; i++) {
        mfe_mod_t *m = &p->mod[i];
        F8(m->src); F8(m->dst); F16(m->amount);
    }
    mfe_fx_t *x = &p->fx;
    if (o->ver >= 5) {
        F16(x->fuzz_amount); F16(x->fuzz_octave); F16(x->fuzz_tone); F16(x->fuzz_level);
        F16(x->tape_drive); F16(x->tape_tone); F16(x->tape_level); F16(x->tape_wow);
        F8(x->ens_mode); F8(x->fx_sync); F16(x->ens_mix); F16(x->ens_rate); F16(x->ens_depth);
        F16(x->bbd_mix); F16(x->bbd_time); F16(x->bbd_feedback); F16(x->bbd_mod); F16(x->bbd_tone);
        F16(x->oil_mix); F16(x->oil_time); F16(x->oil_feedback); F16(x->oil_warble); F16(x->oil_smear);
        F8(x->rev_type); F8(x->rev_hold); F16(x->rev_amount);
    } else {
        /* v1-v4: drive, chorus mix/rate/depth, delay mix/time/feedback, reverb mix/size/damp */
        int16_t old[10] = {0};
        for (int k = 0; k < 10; k++) F16(old[k]);
        if (!wr) {
            mfe_fx_defaults(x);
            x->tape_drive = old[0];                                  /* insert drive -> TAPE */
            if (old[1] > 0) { x->ens_mode = MFE_ENS_I; x->ens_mix = old[1]; }   /* chorus -> ENSEMBLE I */
            x->bbd_mix = old[4];                                     /* delay -> BBD ECHO (0-500 ms -> 0-1000 ms) */
            x->bbd_time = (int16_t)(old[5] > 0 ? old[5] / 2 : 0);
            x->bbd_feedback = old[6];
            x->rev_type = MFE_REV_TWIN_TANK; x->rev_amount = old[7];  /* reverb -> TWIN TANK */
        }
    }
    if (o->ver >= 4) {
        mfe_modosc_t *m = &p->modosc;
        F8(m->semis); F8(m->rm_osc); F8(m->pm_osc); F8(m->pos_osc); F8(m->ffm_filter); F8(m->reserved);
        F16(m->rm); F16(m->pm); F16(m->pos); F16(m->ffm);
    } else if (!wr) memset(&p->modosc, 0, sizeof p->modosc);
}

static size_t size_for(unsigned ver)
{
    mfe_patch_t p;
    memset(&p, 0, sizeof p);
    io_t o = {0};
    o.ver = ver;
    walk(&o, &p, 1);
    return 8 + o.n + 4;
}

size_t mfe_preset_size(void)
{
    static size_t n;
    if (!n) n = size_for(MFE_PRESET_VERSION);
    return n;
}

size_t mfe_preset_write(const mfe_patch_t *p, uint8_t *out, size_t cap)
{
    size_t total = mfe_preset_size();
    if (cap < total) return 0;
    io_t o = {out, 0, 0, cap, 0, MFE_PRESET_VERSION};
    memcpy(out, MFE_PRESET_MAGIC, 4);
    o.n = 4;
    w16(&o, MFE_PRESET_VERSION);
    w16(&o, (int32_t)(total - 12));
    mfe_patch_t c = *p;
    walk(&o, &c, 1);
    uint32_t crc = crc32(out, o.n);
    for (int k = 0; k < 4; k++) w8(&o, crc >> (8 * k));
    return o.err ? 0 : o.n;
}

int mfe_preset_read(mfe_patch_t *p, const uint8_t *in, size_t len)
{
    if (len < 8) return -1;
    if (memcmp(in, MFE_PRESET_MAGIC, 4)) return -2;
    unsigned ver = in[4] | in[5] << 8;
    if (ver < 1 || ver > MFE_PRESET_VERSION) return -3;
    size_t total = size_for(ver);
    if (len < total) return -1;
    if ((size_t)(in[6] | in[7] << 8) != total - 12) return -3;
    uint32_t crc = 0;
    for (int k = 0; k < 4; k++) crc |= (uint32_t)in[total - 4 + k] << (8 * k);
    if (crc != crc32(in, total - 4)) return -4;
    io_t o = {0, in, 8, len, 0, ver};
    mfe_patch_t t;
    memset(&t, 0, sizeof t);
    walk(&o, &t, 0);
    if (o.err) return -1;
    t.name[MFE_NAME_LEN - 1] = 0;
    if (t.voice_mode > MFE_LEGATO) return -5;
    for (int i = 0; i < MFE_NOSC; i++) if (t.osc[i].table >= MFE_WT_COUNT) return -5;
    for (int i = 0; i < MFE_NFILT; i++) if (t.filt[i].type >= MFE_F_COUNT) return -5;
    for (int i = 0; i < MFE_NLFO; i++) {
        const mfe_lfo_t *l = &t.lfo[i];
        if (l->wave >= MFE_W_COUNT || l->sync_div >= MFE_SYNC_DIVS || l->loop_start >= MFE_PERF_STEPS ||
            l->loop_end >= MFE_PERF_STEPS) return -5;
        for (int k = 0; k < MFE_PERF_STEPS; k++) if (l->step_shape[k] >= MFE_PERF_SHAPES) return -5;
    }
    for (int i = 0; i < MFE_NMOD; i++) if (t.mod[i].src >= MFE_S_COUNT || t.mod[i].dst >= MFE_D_COUNT) return -5;
    if (t.modosc.rm_osc > 3 || t.modosc.pm_osc > 3 || t.modosc.pos_osc > 3 || t.modosc.ffm_filter > 2) return -5;
    if (t.fx.ens_mode >= MFE_ENS_COUNT || t.fx.rev_type >= MFE_REV_COUNT || t.fx.rev_hold > 1) return -5;
    if ((t.fx.fx_sync & 15u) > MFE_SYNC_DIVS || (t.fx.fx_sync >> 4) > MFE_SYNC_DIVS) return -5;
    *p = t;
    return 0;
}
