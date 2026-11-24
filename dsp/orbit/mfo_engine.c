/* SPDX-License-Identifier: AGPL-3.0-only
 * Vintage Chonk ORBIT: the engine (layers, voices, sample and synth sources, matrix, filters, blender,
 * orbit path, drift, parameters and mod matrix). Everything is allocated in mfo_engine_create; the
 * render path only computes. Internal processing runs in chunks that never cross a multiple of
 * MFO_BLOCK samples (the convolution / STFT tick grid), so host block sizes do not matter. */
#include <stdio.h>
#include <stdlib.h>
#if defined(__SSE__) || defined(_M_X64) || defined(_M_AMD64)
#include <xmmintrin.h>
#define MFO_HAVE_SSE 1
#endif
#include "mfo_internal.h"

/* ------------------------------------------------------------------ params --- */
static const mfo_param_info_t g_info[MFO_P_GLOBAL_COUNT] = {
    { "PUCK X", -1, 1, 0, 0 }, { "PUCK Y", -1, 1, 0, 0 },
    { "NODE1 X", -1, 1, -0.7f, 0 }, { "NODE1 Y", -1, 1, 0.7f, 0 },
    { "NODE2 X", -1, 1, 0.7f, 0 }, { "NODE2 Y", -1, 1, 0.7f, 0 },
    { "NODE3 X", -1, 1, 0.7f, 0 }, { "NODE3 Y", -1, 1, -0.7f, 0 },
    { "DRY X", -1, 1, -0.7f, 0 }, { "DRY Y", -1, 1, -0.7f, 0 },
    { "BLEND SHARP", 1, 6, 2, 0 },
    { "ORBIT PATH", 0, 3, 0, MFO_PF_INT },
    { "ORBIT RATE", 0.01f, 20, 0.1f, MFO_PF_LOG },
    { "ORBIT SYNC", 0, 64, 0, MFO_PF_INT },
    { "ORBIT SIZE X", 0, 1, 0.7f, 0 }, { "ORBIT SIZE Y", 0, 1, 0.45f, 0 },
    { "ORBIT WOBBLE", 0, 1, 0, 0 },
    { "ORBIT DIR", 0, 1, 0, MFO_PF_INT },
    { "ORBIT ROTATE", 0, 1, 0, 0 },
    { "DRIFT", 0, 1, 0.2f, 0 },
    { "DRIVE", 0, 1, 0.1f, 0 },
    { "MASTER", 0, 2, 0.8f, 0 },
    { "LFO1 RATE", 0.01f, 20, 0.3f, MFO_PF_LOG }, { "LFO2 RATE", 0.01f, 20, 1.7f, MFO_PF_LOG },
};
static const mfo_param_info_t l_info[MFO_L_COUNT] = {
    { "LEVEL", 0, 2, 1, 0 }, { "PAN", -1, 1, 0, 0 },
    { "BODY", 0, 1, 1, 0 }, { "TONE", 0, 1, 0, 0 }, { "GRAIN", 0, 1, 0, 0 }, { "MOTION", 0, 1, 0, 0 }, { "PITCH", 0, 1, 0, 0 },
    { "TUNE", -48, 48, 0, 0 }, { "FINE", -100, 100, 0, 0 },
    { "ATTACK", 0, 10, 0.005f, 0 }, { "DECAY", 0, 10, 0.5f, 0 }, { "SUSTAIN", 0, 1, 1, 0 }, { "RELEASE", 0, 20, 0.3f, 0 },
    { "VEL SENS", 0, 1, 0.6f, 0 },
    { "FILTER TYPE", 0, 4, 0, MFO_PF_INT },
    { "CUTOFF", 20, 20000, 20000, MFO_PF_LOG },
    { "RESO", 0, 1, 0, 0 }, { "FDRIVE", 0, 1, 0, 0 },
    { "FENV AMT", -1, 1, 0, 0 },
    { "FENV A", 0, 10, 0.01f, 0 }, { "FENV D", 0, 10, 0.4f, 0 }, { "FENV S", 0, 1, 0, 0 }, { "FENV R", 0, 20, 0.4f, 0 },
    { "MOD SOURCE", 0, 3, 0, MFO_PF_INT },
    { "SCAN RATE", 0, 4, 1, 0 }, { "SCAN POS", 0, 1, 0, 0 }, { "SCAN LOOP", 0, 1, 1, MFO_PF_INT },
    { "IR PITCH", -24, 24, 0, 0 },
    { "KEYTRACK", 0, 1, 0, MFO_PF_INT },
    { "START", 0, 1, 0, 0 }, { "VEL START", 0, 1, 0, 0 },
    { "LOOP", 0, 1, 0, MFO_PF_INT }, { "LOOP START", 0, 1, 0.2f, 0 }, { "LOOP END", 0, 1, 1, 0 }, { "XFADE", 0, 0.5f, 0.1f, 0 },
    { "REVERSE", 0, 1, 0, MFO_PF_INT },
    { "STRETCH", 0.25f, 4, 1, MFO_PF_LOG },
    { "TAPE", 0, 1, 0, MFO_PF_INT },
    { "WOW", 0, 1, 0.3f, 0 }, { "FLUTTER", 0, 1, 0.2f, 0 }, { "BUMP", 0, 1, 0.5f, 0 }, { "HISS", 0, 1, 0.2f, 0 },
};
static mfo_param_info_t all_info[MFO_PARAM_COUNT];
static char all_names[MFO_LAYERS * MFO_L_COUNT][32];
static int info_ready;

static void build_info(void)
{
    if (info_ready) return;
    for (int i = 0; i < MFO_P_GLOBAL_COUNT; i++) all_info[i] = g_info[i];
    for (int l = 0; l < MFO_LAYERS; l++)
        for (int p = 0; p < MFO_L_COUNT; p++) {
            int id = MFO_LPARAM(l, p);
            char *nm = all_names[l * MFO_L_COUNT + p];
            snprintf(nm, 32, "L%d %s", l + 1, l_info[p].name);
            all_info[id] = l_info[p];
            all_info[id].name = nm;
        }
    info_ready = 1;
}

const mfo_param_info_t *mfo_param_info(int id)
{
    build_info();
    return id >= 0 && id < MFO_PARAM_COUNT ? &all_info[id] : NULL;
}

/* ------------------------------------------------------------------- types --- */
typedef struct {
    int active, note, released;
    float vel;
    uint32_t age;
    mfo_env_t env;
    double pos;                  /* read position (buffer samples, virtual = possibly reversed) */
    double head;                 /* stretch: time position */
    double gpos[2]; float gph[2];
    float dw, dwl;               /* drift random walk (raw, smoothed) */
    int tail;                    /* keytrack: samples of convolution tail left after the source ended */
    int src_done;
    const mfo_kernel_t *kk;      /* keytrack kernel of this note */
} voice_t;

typedef struct { float b0, b1, b2, a1, a2, z1[2], z2[2]; } biquad_t;

typedef struct {
    int src;
    mfo_buffer_t sample; int has_sample;
    mfe_engine_t *synth; mfe_patch_t patch; mfo_resampler_t rs; double rs_step0;
    float syn_l[MFE_BLOCK], syn_r[MFE_BLOCK];
    mfo_buffer_t imp; int has_imp;
    const mfo_kernel_t *kernel;
    const mfo_keytrack_t *kt;
    voice_t v[MFO_MAX_VOICES];
    mfo_conv_t *bus_conv; int bus_conv_on;
    mfo_conv_t **vconv;
    mfo_xsyn_t *xs;
    double scan;                 /* impulse stream position */
    mfo_svf_t svf; mfo_ladder_t lad; mfo_env_t fenv;
    float wow_ph, flut_ph, wow_w, act;
    biquad_t bump; float tlp[2], hiss_s;
    float dcx[2], dcy[2];        /* SYNTH sources: DC blocker (MFE output carries a small offset) */
    float dcut, dcut_l, dirp, dirp_l;
    float body_prev, lvl_prev[2];
    int last_note; float last_vel;
    float bus[2][MFO_BLOCK];     /* source bus (after tape) */
    float out[2][MFO_BLOCK];
    float mod[MFO_BLOCK];
    int notes_held;
    /* live INPUT (1.2.0) */
    int in_src, in_imp, in_gate;
    float in_win;
    mfo_env_t inenv;
    mfo_conv_t *iconv[2];        /* two convolvers on the bus: the active kernel and the one coming in */
    mfo_kernel_t *ik[2];
    int ik_ready[2];
    mfo_kload_t *ild;
    float *istage;               /* the captured window being loaded */
    int iact;                    /* slot at full weight, -1 = none yet */
    int iphase, icount, ilen;    /* 0 capture next, 1 loading, 2 warm-up, 3 cross-fade */
} layer_t;

struct mfo_keytrack {
    int lo, hi;
    mfo_kernel_t **k;
};

struct mfo_engine {
    mfo_config_t cfg;
    float sr;
    int lat;
    int64_t t;
    uint32_t rng;
    float base[MFO_PARAM_COUNT], cur[MFO_PARAM_COUNT];
    int cur_init;
    mfo_mod_t mod[MFO_NMOD];
    float msrc[MFO_MS_COUNT];
    float lfo_ph[2];
    float bend, bpm;
    double orbit_th;
    float wob[2], wobl[2];
    float px, py, wprev[4];
    layer_t L[MFO_LAYERS];
    float *dry[2]; int dry_pos;
    uint32_t age;
    /* live INPUT (1.2.0) */
    float *cap; int cap_len, cap_pos; int cap_on;   /* mono capture ring of the input (after the trim) */
    float in_gain;
    const float *cin[2]; float cin_mono[MFO_BLOCK];  /* the current chunk's input */
    float held_wob; int freeze;
};

void mfo_config_default(mfo_config_t *c)
{
    c->sample_rate = 48000.0f;
    c->max_voices = 16;
    c->max_ir_seconds = 6.0f;
    c->keytrack_voices = 8;
    c->keytrack_seconds = 1.5f;
    c->stft_size = 1024;
    c->stft_hop = 128;
    c->spectral = 1;
    c->seed = 0x0B17u;
    c->live_input = 1;
}

static float P(const mfo_engine_t *e, int id) { return e->cur[id]; }
static float LP_(const mfo_engine_t *e, int l, int p) { return e->cur[MFO_LPARAM(l, p)]; }

void mfo_engine_destroy(mfo_engine_t *e)
{
    if (!e) return;
    for (int l = 0; l < MFO_LAYERS; l++) {
        layer_t *L = &e->L[l];
        free(L->synth);
        mfo_resampler_free(&L->rs);
        mfo_conv_destroy(L->bus_conv);
        if (L->vconv) for (int i = 0; i < e->cfg.keytrack_voices; i++) mfo_conv_destroy(L->vconv[i]);
        free(L->vconv);
        mfo_xsyn_destroy(L->xs);
        for (int s = 0; s < 2; s++) { mfo_conv_destroy(L->iconv[s]); mfo_kernel_destroy(L->ik[s]); }
        mfo_kload_destroy(L->ild);
        free(L->istage);
    }
    free(e->cap);
    free(e->dry[0]); free(e->dry[1]);
    free(e);
}

mfo_engine_t *mfo_engine_create(const mfo_config_t *c)
{
    mfo_config_t cfg;
    if (c) cfg = *c; else mfo_config_default(&cfg);
    if (cfg.max_voices < 1) cfg.max_voices = 1;
    if (cfg.max_voices > MFO_MAX_VOICES) cfg.max_voices = MFO_MAX_VOICES;
    if (cfg.keytrack_voices > cfg.max_voices) cfg.keytrack_voices = cfg.max_voices;
    if (cfg.keytrack_voices < 0) cfg.keytrack_voices = 0;
    if (!mfe_tables_ready()) mfe_init_tables();
    build_info();
    mfo_engine_t *e = calloc(1, sizeof *e);
    if (!e) return NULL;
    e->cfg = cfg; e->sr = cfg.sample_rate;
    int ok = 1;
    int max_ir = (int)(cfg.max_ir_seconds * cfg.sample_rate);
    int kt_len = (int)(cfg.keytrack_seconds * cfg.sample_rate);
    for (int l = 0; l < MFO_LAYERS; l++) {
        layer_t *L = &e->L[l];
        L->synth = calloc(1, mfe_engine_size());
        ok = ok && L->synth && mfo_resampler_init(&L->rs, MFE_SR, cfg.sample_rate);
        L->rs_step0 = L->rs.step;
        if (L->synth) mfe_engine_init(L->synth);
        L->bus_conv = mfo_conv_create(2, max_ir > 1 ? max_ir : 1);
        ok = ok && L->bus_conv;
        if (cfg.keytrack_voices) {
            L->vconv = calloc((size_t)cfg.keytrack_voices, sizeof *L->vconv);
            ok = ok && L->vconv;
            for (int i = 0; ok && i < cfg.keytrack_voices; i++) { L->vconv[i] = mfo_conv_create(1, kt_len > 1 ? kt_len : 1); ok = ok && L->vconv[i]; }
        }
        if (cfg.spectral) { L->xs = mfo_xsyn_create(cfg.sample_rate, cfg.stft_size, cfg.stft_hop); ok = ok && L->xs; }
        /* spread the large FFT boundaries of the layers and voices over time (no latency change) */
        if (L->bus_conv) mfo_conv_set_stagger(L->bus_conv, l * (4096 / MFO_LAYERS + MFO_BLOCK));
        for (int i = 0; ok && i < cfg.keytrack_voices; i++)
            mfo_conv_set_stagger(L->vconv[i], ((l * cfg.keytrack_voices + i) * (4096 / (MFO_LAYERS * cfg.keytrack_voices) / MFO_BLOCK + 1) * MFO_BLOCK) % 4096);
        if (L->xs) mfo_xsyn_set_stagger(L->xs, (l * MFO_BLOCK) % cfg.stft_hop);
        L->last_note = 60; L->last_vel = 0.8f;
        L->bump.b0 = 1.0f;
        L->in_win = 0.25f;
        if (cfg.live_input) {
            int il = (int)(MFO_INPUT_MAX_SECONDS * cfg.sample_rate);
            for (int s = 0; ok && s < 2; s++) {
                L->iconv[s] = mfo_conv_create(2, il); L->ik[s] = mfo_kernel_alloc(1, il);
                ok = ok && L->iconv[s] && L->ik[s];
                if (L->iconv[s]) mfo_conv_set_stagger(L->iconv[s], ((l * 2 + s) * 640) % 4096);
            }
            L->ild = mfo_kload_create(il);
            L->istage = calloc((size_t)il, sizeof(float));
            ok = ok && L->ild && L->istage;
        }
    }
    if (cfg.live_input) {
        e->cap_len = (int)(MFO_INPUT_MAX_SECONDS * cfg.sample_rate) + MFO_BLOCK;
        e->cap = calloc((size_t)e->cap_len, sizeof(float));
        ok = ok && e->cap;
    }
    e->in_gain = 1.0f;
    e->held_wob = -1.0f;
    e->lat = cfg.spectral && e->L[0].xs ? mfo_xsyn_latency(e->L[0].xs) : 0;
    if (e->lat) {
        e->dry[0] = calloc((size_t)e->lat, sizeof(float));
        e->dry[1] = calloc((size_t)e->lat, sizeof(float));
        ok = ok && e->dry[0] && e->dry[1];
    }
    if (!ok) { mfo_engine_destroy(e); return NULL; }
    for (int i = 0; i < MFO_PARAM_COUNT; i++) e->base[i] = all_info[i].def;
    e->rng = cfg.seed ? cfg.seed : 1u;
    e->bpm = 120.0f;
    mfo_engine_reset(e);
    return e;
}

static void input_reset_layer(mfo_engine_t *e, int l)
{
    layer_t *L = &e->L[l];
    for (int s = 0; s < 2; s++) { if (L->iconv[s]) mfo_conv_reset(L->iconv[s]); L->ik_ready[s] = 0; }
    L->iact = -1; L->iphase = 0; L->icount = 0;
}

void mfo_engine_reset(mfo_engine_t *e)
{
    e->t = 0;
    for (int l = 0; l < MFO_LAYERS; l++) {
        layer_t *L = &e->L[l];
        memset(L->v, 0, sizeof L->v);
        mfo_conv_reset(L->bus_conv);
        L->bus_conv_on = 0;
        for (int i = 0; i < e->cfg.keytrack_voices; i++) mfo_conv_reset(L->vconv[i]);
        if (L->xs) mfo_xsyn_reset(L->xs);
        mfe_all_notes_off(L->synth);
        memset(&L->svf, 0, sizeof L->svf); memset(&L->lad, 0, sizeof L->lad); memset(&L->fenv, 0, sizeof L->fenv);
        memset(L->bump.z1, 0, sizeof L->bump.z1); memset(L->bump.z2, 0, sizeof L->bump.z2);
        L->tlp[0] = L->tlp[1] = 0; L->hiss_s = 0; L->act = 0;
        L->dcx[0] = L->dcx[1] = L->dcy[0] = L->dcy[1] = 0;
        L->body_prev = 0; L->lvl_prev[0] = L->lvl_prev[1] = 0;
        L->notes_held = 0;
        memset(&L->inenv, 0, sizeof L->inenv);
        input_reset_layer(e, l);
    }
    if (e->cap) memset(e->cap, 0, sizeof(float) * (size_t)e->cap_len);
    e->cap_pos = 0;
    if (e->lat) { memset(e->dry[0], 0, sizeof(float) * (size_t)e->lat); memset(e->dry[1], 0, sizeof(float) * (size_t)e->lat); }
    e->dry_pos = 0;
    e->cur_init = 0;
    e->orbit_th = 0;
    for (int i = 0; i < 4; i++) e->wprev[i] = -1.0f;
}

int mfo_latency_samples(const mfo_engine_t *e) { return e->lat; }
float mfo_latency_ms(const mfo_engine_t *e) { return 1000.0f * e->lat / e->sr; }

/* -------------------------------------------------------- parameter calls --- */
void mfo_set_param(mfo_engine_t *e, int id, float v)
{
    if (id < 0 || id >= MFO_PARAM_COUNT) return;
    e->base[id] = mfo_clampf(v, all_info[id].min, all_info[id].max);
}
float mfo_get_param(const mfo_engine_t *e, int id) { return id >= 0 && id < MFO_PARAM_COUNT ? e->base[id] : 0.0f; }
float mfo_get_param_effective(const mfo_engine_t *e, int id) { return id >= 0 && id < MFO_PARAM_COUNT ? e->cur[id] : 0.0f; }

void mfo_set_mod(mfo_engine_t *e, int slot, int src, int dst, float amount)
{
    if (slot < 0 || slot >= MFO_NMOD) return;
    e->mod[slot].src = (uint8_t)(src >= 0 && src < MFO_MS_COUNT ? src : 0);
    e->mod[slot].dst = (uint16_t)(dst >= 0 && dst < MFO_PARAM_COUNT ? dst : 0);
    e->mod[slot].amount = mfo_clampf(amount, -1.0f, 1.0f);
}
void mfo_set_mod_source(mfo_engine_t *e, int src, float value)
{
    if (src > 0 && src < MFO_MS_COUNT) e->msrc[src] = value;
}

static void update_params(mfo_engine_t *e, int n)
{
    float tgt_mod[MFO_PARAM_COUNT];
    memset(tgt_mod, 0, sizeof tgt_mod);
    for (int s = 0; s < MFO_NMOD; s++) {
        const mfo_mod_t *m = &e->mod[s];
        if (!m->src || m->amount == 0.0f) continue;
        tgt_mod[m->dst] += m->amount * e->msrc[m->src];
    }
    float a = 1.0f - expf(-(float)n / (0.012f * e->sr));
    for (int i = 0; i < MFO_PARAM_COUNT; i++) {
        const mfo_param_info_t *in = &all_info[i];
        float v = e->base[i];
        if (tgt_mod[i] != 0.0f && !(in->flags & MFO_PF_INT)) {
            if (in->flags & MFO_PF_LOG) v *= exp2f(tgt_mod[i] * log2f(in->max / in->min));
            else v += tgt_mod[i] * (in->max - in->min);
            v = mfo_clampf(v, in->min, in->max);
        }
        if (!e->cur_init || (in->flags & MFO_PF_INT)) e->cur[i] = v;
        else e->cur[i] += (v - e->cur[i]) * a;
    }
    e->cur_init = 1;
}

/* ------------------------------------------------------------ randomness --- */
static void walk(mfo_engine_t *e, float *w, float *wl, float dt, float tau)
{
    float a = expf(-dt / tau);
    *w = *w * a + sqrtf(1.0f - a * a) * 1.7f * mfo_randf(&e->rng);
    *wl += (*w - *wl) * (1.0f - expf(-dt / 0.25f));
}

/* ------------------------------------------------------------ buffer read --- */
static inline float bs(const mfo_buffer_t *b, int ch, int rev, int i)
{
    if (i < 0 || i >= b->length) return 0.0f;
    const float *d = b->data[ch < b->channels ? ch : 0];
    return d[rev ? b->length - 1 - i : i];
}
static inline float bread(const mfo_buffer_t *b, int ch, int rev, double p)
{
    int i = (int)floor(p);
    float f = (float)(p - i);
    float y0 = bs(b, ch, rev, i - 1), y1 = bs(b, ch, rev, i), y2 = bs(b, ch, rev, i + 1), y3 = bs(b, ch, rev, i + 2);
    float c1 = 0.5f * (y2 - y0), c2 = y0 - 2.5f * y1 + 2.0f * y2 - 0.5f * y3, c3 = 0.5f * (y3 - y0) + 1.5f * (y1 - y2);
    return ((c3 * f + c2) * f + c1) * f + y1;
}
/* looped read with crossfade over [le - xf, le) against [ls - xf, ls) */
static inline float lread(const mfo_buffer_t *b, int ch, int rev, double p, int loop, double ls, double le, double xf)
{
    if (!loop || xf < 1.0 || p < le - xf) return bread(b, ch, rev, p);
    float f = (float)((p - (le - xf)) / xf);
    return (1.0f - f) * bread(b, ch, rev, p) + f * bread(b, ch, rev, p - (le - ls));
}

/* --------------------------------------------------------------- voices --- */
static float note_ratio(float semis) { return exp2f(semis / 12.0f); }

static void loop_bounds(const mfo_engine_t *e, int l, const mfo_buffer_t *b, double *ls, double *le, double *xf)
{
    double len = b->length;
    *ls = LP_(e, l, MFO_L_LOOP_START) * len;
    *le = LP_(e, l, MFO_L_LOOP_END) * len;
    if (*le > len) *le = len;
    if (*le < *ls + 64.0) *le = *ls + 64.0 < len ? *ls + 64.0 : len;
    *xf = LP_(e, l, MFO_L_XFADE) * (*le - *ls);
    if (*xf > *ls) *xf = *ls;
}

static void voice_start(mfo_engine_t *e, int l, voice_t *v, int note, float vel)
{
    layer_t *L = &e->L[l];
    const mfo_buffer_t *b = &L->sample;
    v->active = 1; v->note = note; v->vel = vel; v->released = 0; v->age = ++e->age;
    v->env.stage = MFO_ENV_ATTACK; v->env.level = 0.0f;
    double st = (LP_(e, l, MFO_L_START) + LP_(e, l, MFO_L_VEL_START) * (1.0f - vel) * 0.25f) * b->length;
    if (st > b->length - 1) st = b->length - 1;
    if (st < 0) st = 0;
    v->pos = st; v->head = st;
    v->gpos[0] = v->gpos[1] = st; v->gph[0] = 0.0f; v->gph[1] = 0.5f;
    v->tail = 0; v->src_done = 0;
    v->dw = v->dwl = 0.0f;
    v->kk = NULL;
    int vi = (int)(v - L->v);
    if (L->kt && vi < e->cfg.keytrack_voices) {
        int k = note < L->kt->lo ? L->kt->lo : note > L->kt->hi ? L->kt->hi : note;
        v->kk = L->kt->k[k - L->kt->lo];
        mfo_conv_reset(L->vconv[vi]);
    }
}

/* render one sample voice into acc (stereo, added); returns 0 when the voice is finished */
static int voice_render(mfo_engine_t *e, int l, voice_t *v, float *const *acc, int n, float tape_rate)
{
    layer_t *L = &e->L[l];
    const mfo_buffer_t *b = &L->sample;
    float buf[2][MFO_BLOCK], envb[MFO_BLOCK];
    int rev = LP_(e, l, MFO_L_REVERSE) > 0.5f, loop = LP_(e, l, MFO_L_LOOP) > 0.5f;
    double ls, le, xf;
    loop_bounds(e, l, b, &ls, &le, &xf);
    float drift = P(e, MFO_P_DRIFT);
    walk(e, &v->dw, &v->dwl, (float)n / e->sr, 3.0f);
    float semis = (float)(v->note - b->root_note) + LP_(e, l, MFO_L_TUNE) + LP_(e, l, MFO_L_FINE) * 0.01f + e->bend
                + drift * 0.12f * v->dwl;
    double rate = note_ratio(semis) * (b->sample_rate / e->sr) * tape_rate;
    float stretch = LP_(e, l, MFO_L_STRETCH);
    int gran = fabsf(stretch - 1.0f) > 0.01f;
    float vs = LP_(e, l, MFO_L_VEL_SENS);
    float vgain = 1.0f - vs + vs * v->vel;
    float lv = mfo_env_run(&v->env, LP_(e, l, MFO_L_ATTACK), LP_(e, l, MFO_L_DECAY), LP_(e, l, MFO_L_SUSTAIN),
                           LP_(e, l, MFO_L_RELEASE), e->sr, envb, n);
    int nch = b->channels > 1 ? 2 : 1;
    if (v->src_done || b->length < 2) {
        memset(buf, 0, sizeof buf);
    } else if (!gran) {
        for (int i = 0; i < n; i++) {
            if (loop && v->pos >= le) v->pos -= (le - ls);
            if (v->pos >= b->length) { v->src_done = 1; for (; i < n; i++) buf[0][i] = buf[1][i] = 0.0f; break; }
            for (int c = 0; c < nch; c++) buf[c][i] = lread(b, c, rev, v->pos, loop, ls, le, xf);
            if (nch == 1) buf[1][i] = buf[0][i];
            v->pos += rate;
        }
    } else {
        /* two Hann grains offset by half a grain; the head moves at the stretched time rate */
        float gd = 1.0f / (0.05f * e->sr);
        double hrate = (b->sample_rate / e->sr) * tape_rate / stretch;
        double lo = loop ? ls : 0.0, hi = loop ? le : (double)b->length;
        for (int i = 0; i < n; i++) {
            float s0 = 0.0f, s1 = 0.0f;
            for (int g = 0; g < 2; g++) {
                float w = sinf(MFO_PI * v->gph[g]);
                w *= w;
                double p = v->gpos[g];
                if (loop && p >= le) p -= (le - ls);
                float a = bread(b, 0, rev, p), c = nch > 1 ? bread(b, 1, rev, p) : a;
                s0 += w * a; s1 += w * c;
                v->gpos[g] += rate;
                v->gph[g] += gd;
                if (v->gph[g] >= 1.0f) {
                    /* WSOLA-style: start the new grain near the head where it lines up best with the other
                     * grain's continuation (no phase cancellation between the overlapping grains) */
                    v->gph[g] -= 1.0f;
                    const double cont = v->gpos[1 - g];
                    int W = (int)(0.006f * b->sample_rate), best = 0;
                    float bc = -1e30f;
                    for (int d = -W; d <= W; d += 2) {
                        float c = 0.0f;
                        for (int j = 0; j < 128; j += 4) {
                            c += bs(b, 0, rev, (int)(v->head + d + rate * j)) * bs(b, 0, rev, (int)(cont + rate * j));
                        }
                        if (c > bc) { bc = c; best = d; }
                    }
                    v->gpos[g] = v->head + best;
                    if (v->gpos[g] < 0) v->gpos[g] = 0;
                }
            }
            buf[0][i] = s0; buf[1][i] = s1;
            v->head += hrate;
            if (v->head >= hi) { if (loop) v->head -= (hi - lo); else { v->head = hi; v->src_done = v->gph[0] < 0.02f || v->gph[1] < 0.02f; } }
        }
    }
    for (int i = 0; i < n; i++) { float g = envb[i] * vgain; buf[0][i] *= g; buf[1][i] *= g; }
    int finished = v->env.stage == MFO_ENV_IDLE || (v->src_done && !loop);
    /* keytrack BODY per voice */
    int vi = (int)(v - L->v);
    if (v->kk && LP_(e, l, MFO_L_KEYTRACK) > 0.5f && vi < e->cfg.keytrack_voices) {
        float mono[MFO_BLOCK], w0[MFO_BLOCK], w1[MFO_BLOCK];
        for (int i = 0; i < n; i++) mono[i] = 0.5f * (buf[0][i] + buf[1][i]);
        const float *ip[1] = { mono };
        float *op[2] = { w0, w1 };
        mfo_conv_process(L->vconv[vi], v->kk, ip, op, n);
        float b0 = L->body_prev, b1 = LP_(e, l, MFO_L_BODY);
        for (int i = 0; i < n; i++) {
            float bb = b0 + (b1 - b0) * (float)(i + 1) / n;
            buf[0][i] += bb * (w0[i] - buf[0][i]);
            buf[1][i] += bb * (w1[i] - buf[1][i]);
        }
        if (finished) {
            if (!v->tail) v->tail = mfo_kernel_length(v->kk) + 1;
            v->tail -= n;
            finished = v->tail <= 0;
            if (!finished) { v->src_done = 1; v->env.stage = MFO_ENV_IDLE; }
        }
    }
    for (int i = 0; i < n; i++) { acc[0][i] += buf[0][i]; acc[1][i] += buf[1][i]; }
    (void)lv;
    return !finished;
}

/* ------------------------------------------------------------ note calls --- */
void mfo_note_on(mfo_engine_t *e, int note, int vel)
{
    if (vel <= 0) { mfo_note_off(e, note); return; }
    float fv = vel / 127.0f;
    e->msrc[MFO_MS_VELOCITY] = fv;
    e->msrc[MFO_MS_KEY] = (note - 60) / 60.0f;
    for (int l = 0; l < MFO_LAYERS; l++) {
        layer_t *L = &e->L[l];
        L->last_note = note; L->last_vel = fv;
        L->notes_held++;
        mfo_env_gate(&L->fenv, 1);
        if (L->has_imp) L->scan = (double)LP_(e, l, MFO_L_SCAN_POS) * L->imp.length;
        if (L->in_src) {
            mfo_env_gate(&L->inenv, 1);
        } else if (L->src == MFO_SRC_SAMPLE && L->has_sample) {
            int nv = e->cfg.max_voices, pick = -1;
            for (int i = 0; i < nv; i++) if (!L->v[i].active) { pick = i; break; }
            if (pick < 0) {                       /* steal: quietest released voice, else the oldest */
                float best = 2.0f; uint32_t oldest = UINT32_MAX;
                for (int i = 0; i < nv; i++) if (L->v[i].released && L->v[i].env.level < best) { best = L->v[i].env.level; pick = i; }
                if (pick < 0) for (int i = 0; i < nv; i++) if (L->v[i].age < oldest) { oldest = L->v[i].age; pick = i; }
            }
            voice_start(e, l, &L->v[pick], note, fv);
        } else if (L->src == MFO_SRC_SYNTH) {
            int nn = note + (int)lrintf(LP_(e, l, MFO_L_TUNE));
            if (nn >= 0 && nn < 128) mfe_note_on(L->synth, (uint8_t)nn, (uint8_t)vel);
        }
    }
}

void mfo_note_off(mfo_engine_t *e, int note)
{
    for (int l = 0; l < MFO_LAYERS; l++) {
        layer_t *L = &e->L[l];
        if (L->notes_held > 0 && --L->notes_held == 0) { mfo_env_gate(&L->fenv, 0); mfo_env_gate(&L->inenv, 0); }
        for (int i = 0; i < e->cfg.max_voices; i++) {
            voice_t *v = &L->v[i];
            if (v->active && v->note == note && !v->released) { v->released = 1; mfo_env_gate(&v->env, 0); }
        }
        if (L->src == MFO_SRC_SYNTH) {
            int nn = note + (int)lrintf(LP_(e, l, MFO_L_TUNE));
            if (nn >= 0 && nn < 128) mfe_note_off(L->synth, (uint8_t)nn);
        }
    }
}

void mfo_all_notes_off(mfo_engine_t *e)
{
    for (int l = 0; l < MFO_LAYERS; l++) {
        layer_t *L = &e->L[l];
        L->notes_held = 0;
        mfo_env_gate(&L->fenv, 0);
        mfo_env_gate(&L->inenv, 0);
        for (int i = 0; i < e->cfg.max_voices; i++) if (L->v[i].active) { L->v[i].released = 1; mfo_env_gate(&L->v[i].env, 0); }
        mfe_all_notes_off(L->synth);
    }
}

void mfo_pitch_bend(mfo_engine_t *e, float semis)
{
    e->bend = semis;
    for (int l = 0; l < MFO_LAYERS; l++) {
        layer_t *L = &e->L[l];
        if (L->src != MFO_SRC_SYNTH) continue;
        float range = L->patch.bend_range ? L->patch.bend_range : 2;
        int v = 8192 + (int)lrintf(semis / range * 8192.0f);
        v = v < 0 ? 0 : v > 16383 ? 16383 : v;
        uint8_t msg[3] = { 0xE0, (uint8_t)(v & 127), (uint8_t)(v >> 7) };
        mfe_midi(L->synth, msg, 3);
    }
}

void mfo_set_tempo(mfo_engine_t *e, float bpm)
{
    e->bpm = bpm > 1.0f ? bpm : 1.0f;
    for (int l = 0; l < MFO_LAYERS; l++) mfe_set_tempo(e->L[l].synth, (uint32_t)(e->bpm * 256.0f));
}

unsigned mfo_active_voices(const mfo_engine_t *e)
{
    unsigned n = 0;
    for (int l = 0; l < MFO_LAYERS; l++) {
        if (e->L[l].src == MFO_SRC_SYNTH) n += mfe_active_voices(e->L[l].synth);
        else for (int i = 0; i < e->cfg.max_voices; i++) n += (unsigned)e->L[l].v[i].active;
    }
    return n;
}

/* -------------------------------------------------------- source setters --- */
void mfo_layer_set_sample(mfo_engine_t *e, int l, const mfo_buffer_t *b)
{
    if (l < 0 || l >= MFO_LAYERS) return;
    layer_t *L = &e->L[l];
    memset(L->v, 0, sizeof L->v);
    if (b && b->length > 0 && b->data[0]) { L->sample = *b; L->has_sample = 1; L->src = MFO_SRC_SAMPLE; }
    else { L->has_sample = 0; L->src = MFO_SRC_OFF; }
}
void mfo_layer_set_synth(mfo_engine_t *e, int l, const mfe_patch_t *p)
{
    if (l < 0 || l >= MFO_LAYERS || !p) return;
    layer_t *L = &e->L[l];
    L->patch = *p;
    mfe_all_notes_off(L->synth);
    mfe_engine_set_patch(L->synth, &L->patch);
    memset(L->v, 0, sizeof L->v);
    L->src = MFO_SRC_SYNTH;
}
void mfo_layer_set_source_off(mfo_engine_t *e, int l)
{
    if (l < 0 || l >= MFO_LAYERS) return;
    memset(e->L[l].v, 0, sizeof e->L[l].v);
    e->L[l].src = MFO_SRC_OFF;
}
void mfo_layer_set_impulse(mfo_engine_t *e, int l, const mfo_buffer_t *b, const mfo_kernel_t *k)
{
    if (l < 0 || l >= MFO_LAYERS) return;
    layer_t *L = &e->L[l];
    if (b && b->length > 0 && b->data[0]) { L->imp = *b; L->has_imp = 1; } else L->has_imp = 0;
    L->kernel = k;
    L->scan = 0;
}
void mfo_layer_set_keytrack(mfo_engine_t *e, int l, const mfo_keytrack_t *k)
{
    if (l < 0 || l >= MFO_LAYERS) return;
    e->L[l].kt = k;
    for (int i = 0; i < e->cfg.max_voices; i++) e->L[l].v[i].kk = NULL;
}

/* -------------------------------------------------------------- blender --- */
void mfo_blend_weights(const mfo_engine_t *e, float x, float y, float w[4])
{
    float p = P(e, MFO_P_BLEND_SHARP);
    if (p < 1.0f) p = e->base[MFO_P_BLEND_SHARP] > 0 ? e->base[MFO_P_BLEND_SHARP] : 2.0f;
    float s = 0.0f;
    for (int i = 0; i < 4; i++) {
        float nx = e->cur_init ? e->cur[MFO_P_NODE1_X + 2 * i] : e->base[MFO_P_NODE1_X + 2 * i];
        float ny = e->cur_init ? e->cur[MFO_P_NODE1_Y + 2 * i] : e->base[MFO_P_NODE1_Y + 2 * i];
        float d2 = (x - nx) * (x - nx) + (y - ny) * (y - ny);
        w[i] = 1.0f / (powf(d2, 0.5f * p) + 1e-4f);
        s += w[i];
    }
    for (int i = 0; i < 4; i++) w[i] /= s;
}

void mfo_orbit_position(const mfo_engine_t *e, float *x, float *y) { *x = e->px; *y = e->py; }

static void orbit_update(mfo_engine_t *e, int n)
{
    float dt = (float)n / e->sr;
    int path = (int)P(e, MFO_P_ORBIT_PATH);
    float cx = P(e, MFO_P_PUCK_X), cy = P(e, MFO_P_PUCK_Y);
    float x = cx, y = cy;
    if (path) {
        float sync = P(e, MFO_P_ORBIT_SYNC);
        float hz = sync >= 1.0f ? e->bpm / 60.0f / sync : P(e, MFO_P_ORBIT_RATE);
        float dir = P(e, MFO_P_ORBIT_DIR) > 0.5f ? 1.0f : -1.0f;
        if (!e->freeze) e->orbit_th += (double)(MFO_TWO_PI * hz * dt * dir);
        if (e->orbit_th > 1e6 || e->orbit_th < -1e6) e->orbit_th = fmod(e->orbit_th, 2.0 * 3.14159265358979323846);
        float th = (float)e->orbit_th, sx = P(e, MFO_P_ORBIT_SIZE_X), sy = P(e, MFO_P_ORBIT_SIZE_Y);
        float ox, oy;
        if (path == 1) { ox = sx * cosf(th); oy = sx * sinf(th); }
        else if (path == 2) { ox = sx * cosf(th); oy = sy * sinf(th); }
        else { ox = sx * sinf(th); oy = sy * sinf(2.0f * th); }
        float rot = P(e, MFO_P_ORBIT_ROTATE) * MFO_TWO_PI, cr = cosf(rot), sr = sinf(rot);
        x = cx + ox * cr - oy * sr;
        y = cy + ox * sr + oy * cr;
    }
    float wob = (path && !e->freeze) || e->held_wob < 0.0f ? P(e, MFO_P_ORBIT_WOBBLE) : e->held_wob;   /* stopped = held */
    walk(e, &e->wob[0], &e->wobl[0], dt, 1.5f);
    walk(e, &e->wob[1], &e->wobl[1], dt, 1.5f);
    x += wob * 0.25f * e->wobl[0];
    y += wob * 0.25f * e->wobl[1];
    e->px = mfo_clampf(x, -1.5f, 1.5f);
    e->py = mfo_clampf(y, -1.5f, 1.5f);
    e->msrc[MFO_MS_ORBIT_X] = e->px;
    e->msrc[MFO_MS_ORBIT_Y] = e->py;
}

/* ---------------------------------------------------------- layer stages --- */
static void bump_coef(biquad_t *q, float sr, float f, float gain_db, float Q)
{
    float A = powf(10.0f, gain_db / 40.0f), w = MFO_TWO_PI * f / sr, al = sinf(w) / (2.0f * Q), cw = cosf(w);
    float a0 = 1.0f + al / A;
    q->b0 = (1.0f + al * A) / a0; q->b1 = -2.0f * cw / a0; q->b2 = (1.0f - al * A) / a0;
    q->a1 = -2.0f * cw / a0; q->a2 = (1.0f - al / A) / a0;
}

static void layer_sources(mfo_engine_t *e, int l, int n)
{
    layer_t *L = &e->L[l];
    float *acc[2] = { L->bus[0], L->bus[1] };
    memset(L->bus, 0, sizeof L->bus);
    int tape = LP_(e, l, MFO_L_TAPE) > 0.5f;
    float tape_rate = 1.0f;
    float dt = (float)n / e->sr;
    if (tape) {
        L->wow_ph += 0.55f * dt; L->wow_ph -= floorf(L->wow_ph);
        L->flut_ph += 6.3f * dt; L->flut_ph -= floorf(L->flut_ph);
        float w = 0.0f; walk(e, &L->wow_w, &w, dt, 0.8f);
        tape_rate = 1.0f + LP_(e, l, MFO_L_WOW) * 0.0045f * (sinf(MFO_TWO_PI * L->wow_ph) + 0.4f * L->wow_w)
                         + LP_(e, l, MFO_L_FLUTTER) * 0.0012f * sinf(MFO_TWO_PI * L->flut_ph);
    }
    if (L->in_src) {
        float envb[MFO_BLOCK];
        float vs = LP_(e, l, MFO_L_VEL_SENS), vg = 1.0f - vs + vs * L->last_vel;
        if (L->in_gate) mfo_env_run(&L->inenv, LP_(e, l, MFO_L_ATTACK), LP_(e, l, MFO_L_DECAY), LP_(e, l, MFO_L_SUSTAIN),
                                    LP_(e, l, MFO_L_RELEASE), e->sr, envb, n);
        for (int i = 0; i < n; i++) {
            float g = L->in_gate ? envb[i] * vg : 1.0f;
            acc[0][i] = e->cin[0][i] * g; acc[1][i] = e->cin[1][i] * g;
        }
    } else if (L->src == MFO_SRC_SAMPLE && L->has_sample) {
        for (int i = 0; i < e->cfg.max_voices; i++)
            if (L->v[i].active && !voice_render(e, l, &L->v[i], acc, n, tape_rate)) L->v[i].active = 0;
    } else if (L->src == MFO_SRC_SYNTH) {
        L->rs.step = L->rs_step0 * tape_rate;
        int got = 0;
        while (got < n) {
            got += mfo_resampler_pull(&L->rs, L->bus[0] + got, L->bus[1] + got, n - got);
            if (got >= n) break;
            int32_t blk[MFE_BLOCK * 2];
            mfe_render(L->synth, blk);
            for (int i = 0; i < MFE_BLOCK; i++) { L->syn_l[i] = blk[2 * i] * (1.0f / 32768.0f); L->syn_r[i] = blk[2 * i + 1] * (1.0f / 32768.0f); }
            mfo_resampler_push(&L->rs, L->syn_l, L->syn_r, MFE_BLOCK);
        }
        float R = 1.0f - MFO_TWO_PI * 5.0f / e->sr;
        for (int c = 0; c < 2; c++) {
            float x1 = L->dcx[c], y1 = L->dcy[c];
            for (int i = 0; i < n; i++) { float x = acc[c][i], y = x - x1 + R * y1; x1 = x; y1 = y; acc[c][i] = y; }
            L->dcx[c] = x1; L->dcy[c] = fabsf(y1) < 1e-20f ? 0.0f : y1;
        }
    }
    if (tape) {
        /* head bump, high roll-off, gentle saturation, hiss that follows the activity */
        bump_coef(&L->bump, e->sr, 95.0f, 6.0f * LP_(e, l, MFO_L_BUMP), 0.9f);
        float lpk = 1.0f - expf(-MFO_TWO_PI * 12500.0f / e->sr);
        float hiss = LP_(e, l, MFO_L_HISS) * 0.012f;
        for (int i = 0; i < n; i++) {
            float m = fabsf(acc[0][i]) + fabsf(acc[1][i]);
            L->act += ((m > 1e-4f ? 1.0f : 0.0f) - L->act) * (m > 1e-4f ? 0.002f : 0.00005f);
            float h = mfo_randf(&e->rng);
            L->hiss_s += (h - L->hiss_s) * 0.6f;
            float hs = (h - L->hiss_s) * hiss * L->act;
            for (int c = 0; c < 2; c++) {
                biquad_t *q = &L->bump;
                float x = acc[c][i];
                float y = q->b0 * x + q->z1[c];
                q->z1[c] = q->b1 * x - q->a1 * y + q->z2[c];
                q->z2[c] = q->b2 * x - q->a2 * y;
                L->tlp[c] += (y - L->tlp[c]) * lpk;
                acc[c][i] = mfo_sat(L->tlp[c] * 1.1f) * 0.91f + hs;
            }
        }
        for (int c = 0; c < 2; c++) {
            if (fabsf(L->bump.z1[c]) < 1e-20f) L->bump.z1[c] = 0;
            if (fabsf(L->bump.z2[c]) < 1e-20f) L->bump.z2[c] = 0;
            if (fabsf(L->tlp[c]) < 1e-20f) L->tlp[c] = 0;
        }
    }
}

/* the impulse played as a modulator stream (static mode) */
static void impulse_stream(mfo_engine_t *e, int l, int n)
{
    layer_t *L = &e->L[l];
    if (!L->has_imp) { memset(L->mod, 0, sizeof(float) * (size_t)n); return; }
    const mfo_buffer_t *b = &L->imp;
    double len = b->length;
    float rate = LP_(e, l, MFO_L_SCAN_RATE);
    float pitch = LP_(e, l, MFO_L_IR_PITCH) + P(e, MFO_P_DRIFT) * 0.3f * L->dirp_l;
    double base = (b->sample_rate / e->sr) * exp2f(pitch / 12.0f);
    int loop = LP_(e, l, MFO_L_SCAN_LOOP) > 0.5f, freeze = rate < 0.001f;
    double ls, le, xf;
    if (freeze) {                                  /* loop a short window at SCAN POS: a held spectrum */
        double F = len < 8192.0 ? len : 8192.0;
        xf = F * 0.25;
        ls = LP_(e, l, MFO_L_SCAN_POS) * len;
        if (ls < xf) ls = xf;
        if (ls > len - F + xf) ls = len - F + xf;
        if (ls < 0) ls = 0;
        le = ls + F - xf;
        if (L->scan < ls - xf || L->scan > le) L->scan = ls;
        rate = 1.0f; loop = 1;
    } else {
        xf = len / 8 < 0.05 * b->sample_rate ? len / 8 : 0.05 * b->sample_rate;
        ls = xf; le = len;
    }
    double step = base * rate;
    int ch2 = b->channels > 1;
    for (int i = 0; i < n; i++) {
        if (loop && L->scan >= le) L->scan -= (le - ls);
        float s = 0.0f;
        if (L->scan < len) {
            s = lread(b, 0, 0, L->scan, loop, ls, le, xf);
            if (ch2) s = 0.5f * (s + lread(b, 1, 0, L->scan, loop, ls, le, xf));
        }
        L->mod[i] = s;
        L->scan += step;
    }
}

static inline float soft_knee(float x)
{
    /* continuous: identity below 0.5, then a tanh-like shoulder to 1.0 */
    float a = fabsf(x);
    if (a <= 0.5f) return x;
    float y = 0.5f + 0.5f * mfo_sat((a - 0.5f) * 2.0f);
    return x < 0 ? -y : y;
}

/* BODY against the live input: two convolvers on the bus, kernels refreshed from the capture ring */
static int input_warm(int len) { return len > 4096 ? 4096 : len > 512 ? 512 : 64; }
static void input_body(mfo_engine_t *e, int l, float *const *bus, float *const *out, int n, float b0, float b1)
{
    layer_t *L = &e->L[l];
    int in = L->iact < 0 ? 0 : 1 - L->iact;        /* the slot being refreshed */
    if (L->iphase == 0) {
        /* snapshot the newest window (oldest sample first), short fades, unit energy above a floor */
        int len = (int)(L->in_win * e->sr);
        int maxl = e->cap_len - MFO_BLOCK;
        if (len > maxl) len = maxl;
        if (len < 64) len = 64;
        int fi = (int)(0.002f * e->sr), fo = (int)(0.1f * len);
        if (fo > (int)(0.02f * e->sr)) fo = (int)(0.02f * e->sr);
        double en = 0.0;
        int start = e->cap_pos - len;
        while (start < 0) start += e->cap_len;
        for (int i = 0; i < len; i++) {
            float x = e->cap[(start + i) % e->cap_len];
            if (i < fi) x *= (float)i / fi;
            if (i >= len - fo) x *= (float)(len - 1 - i) / fo;
            L->istage[i] = x;
            en += (double)x * x;
        }
        double fl = (double)len * 1e-5;              /* -50 dBFS RMS */
        float g = 0.8f / (float)sqrt(en > fl ? en : fl);
        L->ilen = len;
        L->ik_ready[in] = 0;
        mfo_kload_begin(L->ild, L->ik[in], L->istage, len, g);
        L->iphase = 1; L->icount = 0;
    } else if (L->iphase == 1) {
        if (mfo_kload_step(L->ild, 2)) { L->ik_ready[in] = 1; L->iphase = 2; L->icount = 0; }
    }
    float y[2][2][MFO_BLOCK];
    for (int s = 0; s < 2; s++) {
        float *o[2] = { y[s][0], y[s][1] };
        mfo_conv_process(L->iconv[s], L->ik_ready[s] ? L->ik[s] : NULL, (const float *const *)bus, o, n);
    }
    int xf = L->iphase == 3;
    int a = L->iact < 0 ? 0 : L->iact;
    for (int i = 0; i < n; i++) {
        float wn = xf ? (float)(L->icount + i + 1) / MFO_INPUT_XFADE : 0.0f;
        if (wn > 1.0f) wn = 1.0f;
        /* equal-power: the two kernels hold different windows of the input (mostly uncorrelated responses) */
        float wnn = sinf(wn * 1.5707963f), woo = L->iact >= 0 ? cosf(wn * 1.5707963f) : 0.0f;
        float wl = woo * y[a][0][i] + wnn * y[in][0][i], wr = woo * y[a][1][i] + wnn * y[in][1][i];
        float b = b0 + (b1 - b0) * (float)(i + 1) / n;
        out[0][i] = bus[0][i] + b * (wl - bus[0][i]);
        out[1][i] = bus[1][i] + b * (wr - bus[1][i]);
    }
    if (L->iphase == 2) {
        L->icount += n;
        if (L->icount >= input_warm(L->ilen)) { L->iphase = 3; L->icount = 0; }
    } else if (L->iphase == 3) {
        L->icount += n;
        if (L->icount >= MFO_INPUT_XFADE) {
            if (L->iact >= 0) L->ik_ready[L->iact] = 0;
            L->iact = in; L->iphase = 0; L->icount = 0;
        }
    }
}

int mfo_input_body_delay(const mfo_engine_t *e, int l, int *full)
{
    if (l < 0 || l >= MFO_LAYERS) return 0;
    int len = (int)(e->L[l].in_win * e->sr);
    int parts = 0;
    for (int s = 64; s < len; s += s < 512 ? 64 : s < 4096 ? 512 : 4096) parts++;
    int start = ((parts + 1) / 2 + 1) * MFO_BLOCK + input_warm(len);
    if (full) *full = start + MFO_INPUT_XFADE;
    return start;
}

void mfo_layer_set_input(mfo_engine_t *e, int l, int src_input, int imp_input, int gate_note, float window_seconds)
{
    if (l < 0 || l >= MFO_LAYERS) return;
    layer_t *L = &e->L[l];
    src_input = src_input != 0;
    imp_input = imp_input != 0 && L->iconv[0] != NULL;
    if (src_input != L->in_src) {
        memset(L->v, 0, sizeof L->v);
        mfe_all_notes_off(L->synth);
        memset(&L->inenv, 0, sizeof L->inenv);
        if (src_input && L->notes_held > 0) mfo_env_gate(&L->inenv, 1);
        L->in_src = src_input;
    }
    if (imp_input != L->in_imp) { input_reset_layer(e, l); L->in_imp = imp_input; }
    L->in_gate = gate_note != 0;
    L->in_win = mfo_clampf(window_seconds, 0.05f, MFO_INPUT_MAX_SECONDS);
    int any = 0;
    for (int k = 0; k < MFO_LAYERS; k++) any = any || e->L[k].in_imp;
    if (any && !e->cap_on && e->cap) { memset(e->cap, 0, sizeof(float) * (size_t)e->cap_len); e->cap_pos = 0; }
    e->cap_on = any;
}

void mfo_set_input_gain(mfo_engine_t *e, float g) { e->in_gain = g >= 0.0f && g < 100.0f ? g : 1.0f; }
void mfo_set_held_wobble(mfo_engine_t *e, float w) { e->held_wob = w < 0.0f ? -1.0f : mfo_clampf(w, 0.0f, 1.0f); }
void mfo_set_orbit_freeze(mfo_engine_t *e, int on) { e->freeze = on != 0; }

static void layer_matrix(mfo_engine_t *e, int l, int n)
{
    layer_t *L = &e->L[l];
    float *bus[2] = { L->bus[0], L->bus[1] };
    float *out[2] = { L->out[0], L->out[1] };
    /* BODY on the bus (unless keytracked per voice) */
    float b1 = LP_(e, l, MFO_L_BODY), b0 = L->body_prev;
    int kt = LP_(e, l, MFO_L_KEYTRACK) > 0.5f && L->kt && L->src == MFO_SRC_SAMPLE && !L->in_src;
    if (L->in_imp && L->iconv[0]) {
        input_body(e, l, bus, out, n, b0, b1);
    } else if (L->kernel && !kt && (b1 > 1e-5f || b0 > 1e-5f)) {
        if (!L->bus_conv_on) { mfo_conv_reset(L->bus_conv); L->bus_conv_on = 1; }
        float w0[MFO_BLOCK], w1[MFO_BLOCK];
        float *wo[2] = { w0, w1 };
        mfo_conv_process(L->bus_conv, L->kernel, (const float *const *)bus, wo, n);
        for (int i = 0; i < n; i++) {
            float b = b0 + (b1 - b0) * (float)(i + 1) / n;
            out[0][i] = bus[0][i] + b * (w0[i] - bus[0][i]);
            out[1][i] = bus[1][i] + b * (w1[i] - bus[1][i]);
        }
    } else {
        L->bus_conv_on = 0;
        memcpy(out[0], bus[0], sizeof(float) * (size_t)n);
        memcpy(out[1], bus[1], sizeof(float) * (size_t)n);
    }
    L->body_prev = b1;
    /* STFT dimensions + MOTION */
    if (L->xs) {
        int ms = (int)LP_(e, l, MFO_L_MOD_SOURCE);
        const float *mod = L->mod;
        if (L->in_imp) {
            memcpy(L->mod, e->cin_mono, sizeof(float) * (size_t)n);
        } else if (ms >= 1 && ms <= MFO_LAYERS && ms - 1 != l) {
            layer_t *S = &e->L[ms - 1];
            for (int i = 0; i < n; i++) L->mod[i] = 0.5f * (S->bus[0][i] + S->bus[1][i]);
        } else {
            impulse_stream(e, l, n);
        }
        mfo_xsyn_params_t xp;
        xp.tone = LP_(e, l, MFO_L_TONE); xp.grain = LP_(e, l, MFO_L_GRAIN);
        xp.pitch = LP_(e, l, MFO_L_PITCH); xp.motion = LP_(e, l, MFO_L_MOTION);
        xp.f0 = 440.0f * exp2f((L->last_note + LP_(e, l, MFO_L_TUNE) + LP_(e, l, MFO_L_FINE) * 0.01f + e->bend - 69.0f) / 12.0f);
        xp.force_stft = 0;
        mfo_xsyn_process(L->xs, &xp, (const float *const *)out, mod, out, n);
    }
    /* CHONK filter */
    int ft = (int)LP_(e, l, MFO_L_FILTER_TYPE);
    float fl = mfo_env_run(&L->fenv, LP_(e, l, MFO_L_FENV_A), LP_(e, l, MFO_L_FENV_D), LP_(e, l, MFO_L_FENV_S),
                           LP_(e, l, MFO_L_FENV_R), e->sr, NULL, n);
    if (ft) {
        float oct = LP_(e, l, MFO_L_FENV_AMT) * 6.0f * fl + P(e, MFO_P_DRIFT) * 0.5f * L->dcut_l;
        float fc = LP_(e, l, MFO_L_CUTOFF) * exp2f(oct);
        if (ft == 1) mfo_ladder_run(&L->lad, fc, LP_(e, l, MFO_L_RESO), LP_(e, l, MFO_L_FDRIVE), e->sr, out, 2, n);
        else {
            float g = 1.0f + 3.0f * LP_(e, l, MFO_L_FDRIVE);
            if (g > 1.001f) for (int c = 0; c < 2; c++) for (int i = 0; i < n; i++) out[c][i] = mfo_sat(out[c][i] * g) / g * (1.0f + 0.3f * (g - 1.0f) / 3.0f);
            mfo_svf_run(&L->svf, ft - 2, fc, LP_(e, l, MFO_L_RESO), e->sr, out, 2, n);
        }
    }
    /* level, pan, layer soft saturation */
    float lvl = LP_(e, l, MFO_L_LEVEL), pan = LP_(e, l, MFO_L_PAN);
    float g1[2] = { lvl * sqrtf(0.5f * (1.0f - pan)) * 1.41421356f, lvl * sqrtf(0.5f * (1.0f + pan)) * 1.41421356f };
    for (int c = 0; c < 2; c++) {
        float g0 = L->lvl_prev[c];
        for (int i = 0; i < n; i++) {
            float g = g0 + (g1[c] - g0) * (float)(i + 1) / n;
            float x = out[c][i] * g;
            out[c][i] = soft_knee(x);                       /* linear below -6 dBFS */
        }
        L->lvl_prev[c] = g1[c];
    }
}

static void render_chunk(mfo_engine_t *e, const float *inl, const float *inr, float *outl, float *outr, int n)
{
    float dt = (float)n / e->sr;
    update_params(e, n);
    /* the input of this chunk (after the trim), silence when absent */
    float il[MFO_BLOCK], ir[MFO_BLOCK];
    float g = e->in_gain;
    for (int i = 0; i < n; i++) {
        float a = inl ? inl[i] * g : 0.0f, b = inr ? inr[i] * g : a;
        il[i] = a; ir[i] = b;
        e->cin_mono[i] = 0.5f * (a + b);
    }
    e->cin[0] = il; e->cin[1] = ir;
    if (e->cap_on) {
        for (int i = 0; i < n; i++) { e->cap[e->cap_pos] = e->cin_mono[i]; e->cap_pos = e->cap_pos + 1 == e->cap_len ? 0 : e->cap_pos + 1; }
    }
    for (int k = 0; k < 2; k++) {
        float hz = P(e, k ? MFO_P_LFO2_RATE : MFO_P_LFO1_RATE);
        e->lfo_ph[k] += hz * dt; e->lfo_ph[k] -= floorf(e->lfo_ph[k]);
        e->msrc[MFO_MS_LFO1 + k] = sinf(MFO_TWO_PI * e->lfo_ph[k]);
    }
    for (int l = 0; l < MFO_LAYERS; l++) {
        layer_t *L = &e->L[l];
        walk(e, &L->dcut, &L->dcut_l, dt, 4.0f);
        walk(e, &L->dirp, &L->dirp_l, dt, 5.0f);
    }
    orbit_update(e, n);
    float w1[4];
    mfo_blend_weights(e, e->px, e->py, w1);
    if (e->wprev[0] < 0.0f) for (int i = 0; i < 4; i++) e->wprev[i] = w1[i];
    for (int l = 0; l < MFO_LAYERS; l++) layer_sources(e, l, n);
    for (int l = 0; l < MFO_LAYERS; l++) layer_matrix(e, l, n);
    /* DRY: the summed source buses, delayed to line up with the matrix outputs */
    float dryl[MFO_BLOCK], dryr[MFO_BLOCK];
    for (int i = 0; i < n; i++) {
        float a = 0, b = 0;
        for (int l = 0; l < MFO_LAYERS; l++) { a += e->L[l].bus[0][i]; b += e->L[l].bus[1][i]; }
        if (e->lat) {
            float da = e->dry[0][e->dry_pos], db = e->dry[1][e->dry_pos];
            e->dry[0][e->dry_pos] = a; e->dry[1][e->dry_pos] = b;
            e->dry_pos = e->dry_pos + 1 == e->lat ? 0 : e->dry_pos + 1;
            a = da; b = db;
        }
        dryl[i] = soft_knee(a); dryr[i] = soft_knee(b);
    }
    float drive = 1.0f + 3.0f * P(e, MFO_P_DRIVE), master = P(e, MFO_P_MASTER);
    float makeup = master / sqrtf(drive);          /* drive pushes into the knee and gets a little louder */
    for (int i = 0; i < n; i++) {
        float f = (float)(i + 1) / n;
        float w[4];
        for (int k = 0; k < 4; k++) w[k] = e->wprev[k] + (w1[k] - e->wprev[k]) * f;
        float a = w[3] * dryl[i], b = w[3] * dryr[i];
        for (int l = 0; l < MFO_LAYERS; l++) { a += w[l] * e->L[l].out[0][i]; b += w[l] * e->L[l].out[1][i]; }
        outl[i] = soft_knee(a * drive) * makeup;
        outr[i] = soft_knee(b * drive) * makeup;
    }
    for (int k = 0; k < 4; k++) e->wprev[k] = w1[k];
    e->t += n;
}

void mfo_render(mfo_engine_t *e, float *left, float *right, int n) { mfo_render_input(e, NULL, NULL, left, right, n); }

void mfo_render_input(mfo_engine_t *e, const float *in_l, const float *in_r, float *left, float *right, int n)
{
#ifdef MFO_HAVE_SSE
    unsigned csr = _mm_getcsr();
    _mm_setcsr(csr | 0x8040u);            /* flush-to-zero + denormals-are-zero while rendering */
#endif
    int done = 0;
    while (done < n) {
        int room = MFO_BLOCK - (int)(e->t % MFO_BLOCK);
        int m = n - done < room ? n - done : room;
        render_chunk(e, in_l ? in_l + done : NULL, in_r ? in_r + done : (in_l ? in_l + done : NULL), left + done, right + done, m);
        done += m;
    }
#ifdef MFO_HAVE_SSE
    _mm_setcsr(csr);
#endif
}

/* ------------------------------------------------------------- keytrack --- */
mfo_keytrack_t *mfo_keytrack_create(const mfo_buffer_t *ir, float engine_rate, int lo, int hi, float max_seconds, float gain)
{
    if (!ir || ir->length < 1 || lo > hi) return NULL;
    mfo_keytrack_t *k = calloc(1, sizeof *k);
    if (!k) return NULL;
    k->lo = lo; k->hi = hi;
    k->k = calloc((size_t)(hi - lo + 1), sizeof *k->k);
    if (!k->k) { free(k); return NULL; }
    int cap = (int)(max_seconds * engine_rate);
    for (int nt = lo; nt <= hi; nt++) {
        double ratio = (engine_rate / ir->sample_rate) / exp2((nt - ir->root_note) / 12.0);
        float *ch[2] = { NULL, NULL };
        int len = 0;
        for (int c = 0; c < ir->channels && c < 2; c++) {
            int src_len = ir->length;
            int need = (int)(cap / ratio) + 64;               /* only resample what survives the cap */
            if (src_len > need) src_len = need;
            ch[c] = mfo_resample_buffer(ir->data[c], src_len, ratio, &len);
        }
        if (len > cap) len = cap;
        /* fade the last 5 ms when capped */
        int fl = (int)(0.005f * engine_rate);
        for (int c = 0; c < 2; c++) if (ch[c] && len > fl && len == cap) for (int i = 0; i < fl; i++) ch[c][len - 1 - i] *= (float)i / fl;
        const float *cp[2] = { ch[0], ch[1] };
        k->k[nt - lo] = ch[0] ? mfo_kernel_create(cp, ch[1] ? 2 : 1, len, gain) : NULL;
        free(ch[0]); free(ch[1]);
        if (!k->k[nt - lo]) { mfo_keytrack_destroy(k); return NULL; }
    }
    return k;
}

void mfo_keytrack_destroy(mfo_keytrack_t *k)
{
    if (!k) return;
    for (int i = 0; i <= k->hi - k->lo; i++) mfo_kernel_destroy(k->k[i]);
    free(k->k);
    free(k);
}
