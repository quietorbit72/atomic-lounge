/* SPDX-License-Identifier: AGPL-3.0-only
 * Vintage Chonk ORBIT demo renders (examples/orbit/*.wav, 48 kHz 24-bit stereo).
 * Only material made here: MFE synth patches and prints, and the procedural impulses of mfo_impgen.c.
 *   orbit_demos [OUTDIR]          (default examples/orbit) */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "mfo.h"

#define SR 48000.0f
static char outdir[512] = "examples/orbit";

static void write_wav(const char *name, const float *l, const float *r, int n, float *peak_db)
{
    float pk = 0;
    for (int i = 0; i < n; i++) { if (fabsf(l[i]) > pk) pk = fabsf(l[i]); if (fabsf(r[i]) > pk) pk = fabsf(r[i]); }
    float g = pk > 0 ? 0.891f / pk : 1.0f;                   /* -1 dBFS peak */
    char path[700];
    snprintf(path, sizeof path, "%s/%s", outdir, name);
    FILE *f = fopen(path, "wb");
    if (!f) { printf("cannot write %s\n", path); return; }
    uint32_t data = (uint32_t)n * 6u, riff = 36u + data, sr = 48000u, br = sr * 6u, fmt = 16u;
    uint16_t pcm = 1, ch = 2, ba = 6, bits = 24;
    fwrite("RIFF", 1, 4, f); fwrite(&riff, 4, 1, f); fwrite("WAVEfmt ", 1, 8, f); fwrite(&fmt, 4, 1, f);
    fwrite(&pcm, 2, 1, f); fwrite(&ch, 2, 1, f); fwrite(&sr, 4, 1, f); fwrite(&br, 4, 1, f); fwrite(&ba, 2, 1, f); fwrite(&bits, 2, 1, f);
    fwrite("data", 1, 4, f); fwrite(&data, 4, 1, f);
    for (int i = 0; i < n; i++) {
        float v[2] = { l[i] * g, r[i] * g };
        for (int c = 0; c < 2; c++) {
            float x = v[c] > 1 ? 1 : v[c] < -1 ? -1 : v[c];
            int32_t s = (int32_t)lrintf(x * 8388607.0f);
            uint8_t b[3] = { (uint8_t)(s & 255), (uint8_t)((s >> 8) & 255), (uint8_t)((s >> 16) & 255) };
            fwrite(b, 1, 3, f);
        }
    }
    fclose(f);
    if (peak_db) *peak_db = 20.0f * log10f(pk > 1e-9f ? pk : 1e-9f);
    printf("  %-34s %5.1f s  (raw peak %.1f dBFS, written at -1 dBFS)\n", name, n / SR, 20.0f * log10f(pk > 1e-9f ? pk : 1e-9f));
}

/* ------------------------------------------------------------- material --- */
static void patch_pad(mfe_patch_t *p)        /* two detuned saws, slow, warm ladder */
{
    mfe_patch_init(p);
    p->osc[1].flags = MFE_OSC_ON; p->osc[1].fine = 11; p->osc[1].amp = 20000;
    p->osc[2].flags = MFE_OSC_ON; p->osc[2].fine = -7; p->osc[2].semis = -12; p->osc[2].amp = 12000;
    p->filt[0].type = MFE_F_LADDER; p->filt[0].cutoff = 23000; p->filt[0].res = 5000;
    p->env[3].attack = 17000; p->env[3].decay = 20000; p->env[3].sustain = 30000; p->env[3].release = 21000;
    p->master = 2600;
}
static void patch_pluck(mfe_patch_t *p)      /* a short square pluck */
{
    mfe_patch_init(p);
    p->osc[0].position = 18724;                  /* frame 4 = square */
    p->filt[0].type = MFE_F_LADDER; p->filt[0].cutoff = 17000; p->filt[0].res = 9000;
    p->env[3].attack = 0; p->env[3].decay = 14000; p->env[3].sustain = 0; p->env[3].release = 12000;
    p->env[0].attack = 0; p->env[0].decay = 11000; p->env[0].sustain = 0;
    p->mod[0].src = MFE_S_ENV1; p->mod[0].dst = MFE_D_F1_CUTOFF; p->mod[0].amount = 9000;
    p->master = 3500;
}

typedef struct { float *d; int n; mfo_buffer_t b; mfo_kernel_t *k; } imp_t;
static void make_imp(imp_t *im, int type, float seconds, float kgain, uint32_t seed)
{
    im->n = (int)(seconds * SR);
    im->d = malloc(sizeof(float) * im->n);
    mfo_generate_impulse(type, im->d, im->n, SR, seed);
    im->b = (mfo_buffer_t){ { im->d, NULL }, 1, im->n, SR, 60.0f };
    /* BODY gain: unit energy, a little hotter */
    double en = 0;
    for (int i = 0; i < im->n; i++) en += (double)im->d[i] * im->d[i];
    const float *p[1] = { im->d };
    im->k = mfo_kernel_create(p, 1, im->n, kgain / (float)sqrt(en > 1e-12 ? en : 1));
}
static void free_imp(imp_t *im) { mfo_kernel_destroy(im->k); free(im->d); }

static mfo_engine_t *engine(void)
{
    mfo_config_t c;
    mfo_config_default(&c);
    mfo_engine_t *e = mfo_engine_create(&c);
    mfo_set_param(e, MFO_P_MASTER, 1.0f);
    mfo_set_param(e, MFO_P_DRIFT, 0.3f);
    for (int l = 0; l < MFO_LAYERS; l++) mfo_set_param(e, MFO_LPARAM(l, MFO_L_BODY), 0);
    return e;
}
static void puck(mfo_engine_t *e, float x, float y) { mfo_set_param(e, MFO_P_PUCK_X, x); mfo_set_param(e, MFO_P_PUCK_Y, y); }

/* a simple score: notes on at time t (s), off after dur */
typedef struct { float t, dur; int note, vel; } ev_t;
static void render_score(mfo_engine_t *e, const ev_t *ev, int nev, float seconds, float *l, float *r,
                         void (*automate)(mfo_engine_t *, float, void *), void *user)
{
    int n = (int)(seconds * SR), blk = 256;
    for (int pos = 0; pos < n; pos += blk) {
        float t0 = pos / SR, t1 = (pos + blk) / SR;
        for (int i = 0; i < nev; i++) {
            if (ev[i].t >= t0 && ev[i].t < t1) mfo_note_on(e, ev[i].note, ev[i].vel);
            float off = ev[i].t + ev[i].dur;
            if (off >= t0 && off < t1) mfo_note_off(e, ev[i].note);
        }
        if (automate) automate(e, t0, user);
        int m = n - pos < blk ? n - pos : blk;
        mfo_render(e, l + pos, r + pos, m);
    }
}

/* ---------------------------------------------------------------- demos --- */
static void demo_body_bowed(float *l, float *r)
{
    /* a synth chord, BODY-convolved with the bowed-metal impulse */
    imp_t im; make_imp(&im, MFO_GEN_BOWED_METAL, 4.0f, 0.8f, 21);
    mfo_engine_t *e = engine();
    mfe_patch_t p; patch_pluck(&p);
    mfo_layer_set_synth(e, 0, &p);
    mfo_layer_set_impulse(e, 0, &im.b, im.k);
    mfo_set_param(e, MFO_LPARAM(0, MFO_L_BODY), 1.0f);
    puck(e, -0.55f, 0.55f);                                    /* mostly layer 1, a touch of dry */
    static const ev_t sc[] = { { 0.2f, 0.6f, 48, 100 }, { 0.2f, 0.6f, 55, 90 }, { 0.2f, 0.6f, 58, 90 }, { 0.2f, 0.6f, 62, 90 },
                               { 2.6f, 0.6f, 44, 100 }, { 2.6f, 0.6f, 51, 90 }, { 2.6f, 0.6f, 55, 90 }, { 2.6f, 0.6f, 60, 90 },
                               { 5.0f, 0.4f, 67, 80 }, { 5.4f, 0.4f, 70, 80 }, { 5.8f, 0.4f, 74, 80 } };
    render_score(e, sc, sizeof sc / sizeof sc[0], 10.0f, l, r, NULL, NULL);
    write_wav("orbit_body_bowed_metal_chord.wav", l, r, (int)(10 * SR), NULL);
    mfo_engine_destroy(e); free_imp(&im);
}

static void auto_tone(mfo_engine_t *e, float t, void *u)
{
    (void)u;
    float a = t < 1.0f ? 0.0f : t < 5.0f ? (t - 1.0f) / 4.0f : 1.0f;      /* TONE 0 -> 1 over 4 s */
    mfo_set_param(e, MFO_LPARAM(0, MFO_L_TONE), a);
}
static void demo_tone_formant(float *l, float *r)
{
    /* saw pad, TONE morph by the procedural formant voice (a -> o -> e -> a), impulse scanned from note-on */
    imp_t im; make_imp(&im, MFO_GEN_FORMANT_VOX, 3.0f, 0.5f, 5);
    mfo_engine_t *e = engine();
    mfe_patch_t p; patch_pad(&p);
    p.filt[0].cutoff = 30000; p.filt[0].res = 0;                /* bright, so the formants have something to shape */
    mfo_layer_set_synth(e, 0, &p);
    mfo_layer_set_impulse(e, 0, &im.b, NULL);
    mfo_set_param(e, MFO_LPARAM(0, MFO_L_SCAN_RATE), 0.75f);
    mfo_set_param(e, MFO_LPARAM(0, MFO_L_SCAN_LOOP), 1);
    puck(e, -0.7f, 0.7f);
    static const ev_t sc[] = { { 0.1f, 9.0f, 45, 100 }, { 0.1f, 9.0f, 52, 90 }, { 0.1f, 9.0f, 57, 90 }, { 0.1f, 9.0f, 60, 90 }, { 0.1f, 9.0f, 64, 90 } };
    render_score(e, sc, 5, 10.0f, l, r, auto_tone, NULL);
    write_wav("orbit_tone_formant_morph_pad.wav", l, r, (int)(10 * SR), NULL);
    mfo_engine_destroy(e); free_imp(&im);
}

static void demo_grain(float *l, float *r)
{
    /* GRAIN texture: a pad carrier wearing a spring tank's fine structure (scanned slowly), plus a little
     * wood-box BODY on layer 2 with the same pad an octave down */
    imp_t sp; make_imp(&sp, MFO_GEN_SPRING, 4.0f, 0.5f, 9);
    imp_t wd; make_imp(&wd, MFO_GEN_WOOD_BOX, 0.5f, 0.9f, 4);
    mfo_engine_t *e = engine();
    mfe_patch_t p; patch_pad(&p);
    mfo_layer_set_synth(e, 0, &p);
    mfo_layer_set_impulse(e, 0, &sp.b, NULL);
    mfo_set_param(e, MFO_LPARAM(0, MFO_L_GRAIN), 0.85f);
    mfo_set_param(e, MFO_LPARAM(0, MFO_L_TONE), 0.3f);
    mfo_set_param(e, MFO_LPARAM(0, MFO_L_SCAN_RATE), 0.35f);
    mfo_layer_set_synth(e, 1, &p);
    mfo_layer_set_impulse(e, 1, &wd.b, wd.k);
    mfo_set_param(e, MFO_LPARAM(1, MFO_L_BODY), 0.7f);
    mfo_set_param(e, MFO_LPARAM(1, MFO_L_TUNE), -12);
    puck(e, 0.0f, 0.62f);                                       /* between layers 1 and 2 */
    static const ev_t sc[] = { { 0.1f, 4.0f, 50, 100 }, { 0.1f, 4.0f, 57, 90 }, { 0.1f, 4.0f, 62, 90 },
                               { 4.4f, 4.0f, 48, 100 }, { 4.4f, 4.0f, 55, 90 }, { 4.4f, 4.0f, 63, 90 } };
    render_score(e, sc, 6, 10.0f, l, r, NULL, NULL);
    write_wav("orbit_grain_spring_texture.wav", l, r, (int)(10 * SR), NULL);
    mfo_engine_destroy(e); free_imp(&sp); free_imp(&wd);
}

static void demo_motion(float *l, float *r)
{
    /* MOTION chop: the pad takes the loudness contour of a rhythmic metal-hit pattern (made here,
     * 120 BPM sixteenths with accents), looped and keyed from note-on */
    int n = (int)(2.0f * SR);
    float *pat = calloc((size_t)n, sizeof(float));
    int hl = (int)(0.25f * SR);
    float *hit = malloc(sizeof(float) * hl);
    mfo_generate_impulse(MFO_GEN_METAL_HIT, hit, hl, SR, 77);
    static const float acc[16] = { 1, 0, .5f, .3f, 1, 0, .6f, 0, .9f, .4f, 0, .7f, 1, 0, .5f, .8f };
    for (int s = 0; s < 16; s++) {
        int at = (int)(s * 0.125f * SR);
        for (int i = 0; i < hl && at + i < n; i++) pat[at + i] += acc[s] * hit[i] * expf(-i / (0.03f * SR));
    }
    mfo_buffer_t pb = { { pat, NULL }, 1, n, SR, 60.0f };
    mfo_engine_t *e = engine();
    mfe_patch_t p; patch_pad(&p);
    p.env[3].attack = 6000;
    mfo_layer_set_synth(e, 0, &p);
    mfo_layer_set_impulse(e, 0, &pb, NULL);
    mfo_set_param(e, MFO_LPARAM(0, MFO_L_MOTION), 1.0f);
    mfo_set_param(e, MFO_LPARAM(0, MFO_L_SCAN_LOOP), 1);
    mfo_set_param(e, MFO_LPARAM(0, MFO_L_FILTER_TYPE), 1);
    mfo_set_param(e, MFO_LPARAM(0, MFO_L_CUTOFF), 1800);
    mfo_set_param(e, MFO_LPARAM(0, MFO_L_RESO), 0.45f);
    mfo_set_param(e, MFO_LPARAM(0, MFO_L_FENV_AMT), 0.35f);
    puck(e, -0.7f, 0.7f);
    static const ev_t sc[] = { { 0.0f, 3.9f, 41, 110 }, { 0.0f, 3.9f, 48, 100 }, { 0.0f, 3.9f, 53, 100 },
                               { 4.0f, 3.9f, 39, 110 }, { 4.0f, 3.9f, 46, 100 }, { 4.0f, 3.9f, 51, 100 } };
    render_score(e, sc, 6, 9.0f, l, r, NULL, NULL);
    write_wav("orbit_motion_chop.wav", l, r, (int)(9 * SR), NULL);
    mfo_engine_destroy(e); free(pat); free(hit);
}

static void demo_pitch(float *l, float *r)
{
    /* PITCH: a tape-hiss noise source plays a melody: the glass impulse's partials, re-tuned to every
     * note, over the noise's own loudness (plus a plate BODY on layer 2 fed the same noise) */
    imp_t gl; make_imp(&gl, MFO_GEN_GLASS, 3.0f, 0.5f, 3);
    imp_t pl; make_imp(&pl, MFO_GEN_PLATE, 3.0f, 0.6f, 8);
    int n = (int)(3.0f * SR);
    float *noise = malloc(sizeof(float) * n);
    mfo_generate_impulse(MFO_GEN_TAPE_HISS, noise, n, SR, 99);
    for (int i = 0; i < n; i++) noise[i] *= expf(i / (0.45f * SR)) * (i > n - 4800 ? (n - i) / 4800.0f : 1.0f);   /* flatten the burst */
    float pk = 0; for (int i = 0; i < n; i++) if (fabsf(noise[i]) > pk) pk = fabsf(noise[i]);
    for (int i = 0; i < n; i++) noise[i] *= 0.3f / pk;
    mfo_buffer_t nb = { { noise, NULL }, 1, n, SR, 60.0f };
    mfo_engine_t *e = engine();
    for (int la = 0; la < 2; la++) {
        mfo_layer_set_sample(e, la, &nb);
        mfo_set_param(e, MFO_LPARAM(la, MFO_L_LOOP), 1);
        mfo_set_param(e, MFO_LPARAM(la, MFO_L_ATTACK), 0.02f);
        mfo_set_param(e, MFO_LPARAM(la, MFO_L_RELEASE), 0.6f);
        mfo_set_param(e, MFO_LPARAM(la, MFO_L_TAPE), 1);
    }
    mfo_layer_set_impulse(e, 0, &gl.b, NULL);
    mfo_set_param(e, MFO_LPARAM(0, MFO_L_PITCH), 1.0f);
    mfo_set_param(e, MFO_LPARAM(0, MFO_L_TONE), 0.4f);
    mfo_set_param(e, MFO_LPARAM(0, MFO_L_SCAN_RATE), 0.0f);            /* freeze: a held glass spectrum */
    mfo_set_param(e, MFO_LPARAM(0, MFO_L_SCAN_POS), 0.05f);
    mfo_layer_set_impulse(e, 1, &pl.b, pl.k);
    mfo_set_param(e, MFO_LPARAM(1, MFO_L_BODY), 1.0f);
    mfo_set_param(e, MFO_LPARAM(1, MFO_L_LEVEL), 0.5f);
    puck(e, -0.25f, 0.7f);
    static const int mel[] = { 64, 67, 71, 69, 67, 64, 62, 64, 60, 59, 60, 64 };
    ev_t sc[12];
    for (int i = 0; i < 12; i++) sc[i] = (ev_t){ 0.2f + i * 0.6f, 0.5f, mel[i], 100 };
    render_score(e, sc, 12, 9.0f, l, r, NULL, NULL);
    write_wav("orbit_pitch_glass_noise_melody.wav", l, r, (int)(9 * SR), NULL);
    mfo_engine_destroy(e); free_imp(&gl); free_imp(&pl); free(noise);
}

static void demo_keytrack(float *l, float *r)
{
    /* KEYTRACK BODY: a click into the wood box, re-pitched per voice: a marimba-like line */
    imp_t wd; make_imp(&wd, MFO_GEN_WOOD_BOX, 0.6f, 1.0f, 4);
    mfo_keytrack_t *kt = mfo_keytrack_create(&wd.b, SR, 36, 96, 1.0f, 2.5f);
    float click[96];
    for (int i = 0; i < 96; i++) click[i] = (i < 3 ? 0.9f : 0.0f) + 0.2f * sinf(i * 0.9f) * expf(-i / 12.0f);
    mfo_buffer_t cb = { { click, NULL }, 1, 96, SR, 60.0f };
    mfo_engine_t *e = engine();
    mfo_layer_set_sample(e, 0, &cb);
    mfo_layer_set_keytrack(e, 0, kt);
    mfo_set_param(e, MFO_LPARAM(0, MFO_L_KEYTRACK), 1);
    mfo_set_param(e, MFO_LPARAM(0, MFO_L_BODY), 1.0f);
    mfo_set_param(e, MFO_LPARAM(0, MFO_L_ATTACK), 0.0f);
    puck(e, -0.7f, 0.7f);
    static const int mel[] = { 60, 64, 67, 72, 71, 67, 64, 62, 65, 69, 72, 74, 72, 69, 65, 64 };
    ev_t sc[18];
    for (int i = 0; i < 16; i++) sc[i] = (ev_t){ 0.1f + i * 0.22f, 0.1f, mel[i], 70 + (i % 4 == 0) * 40 };
    sc[16] = (ev_t){ 3.8f, 0.2f, 48, 110 }; sc[17] = (ev_t){ 3.8f, 0.2f, 55, 100 };
    render_score(e, sc, 18, 5.5f, l, r, NULL, NULL);
    write_wav("orbit_keytrack_wood_box.wav", l, r, (int)(5.5f * SR), NULL);
    mfo_engine_destroy(e); mfo_keytrack_destroy(kt); free_imp(&wd);
}

static void demo_orbit(float *l, float *r)
{
    /* the ORBIT blender: three layers + dry, the puck circling through all four nodes in 8 s
     *   L1 pad   BODY bowed metal      L2 pluck print (sample) BODY spring tank
     *   L3 pad   TONE formant voice + GRAIN plate       DRY at the fourth node */
    imp_t bm; make_imp(&bm, MFO_GEN_BOWED_METAL, 4.0f, 0.7f, 21);
    imp_t sp; make_imp(&sp, MFO_GEN_SPRING, 3.0f, 0.8f, 9);
    imp_t vx; make_imp(&vx, MFO_GEN_FORMANT_VOX, 3.0f, 0.5f, 5);
    mfe_patch_t pad, pl;
    patch_pad(&pad); patch_pluck(&pl);
    int pn = (int)(1.5f * SR);
    float *print = malloc(sizeof(float) * pn);
    mfo_print_synth(&pl, 60, 110, 0.3f, 1.5f, SR, print, NULL);
    mfo_buffer_t pb = { { print, NULL }, 1, pn, SR, 60.0f };
    mfo_engine_t *e = engine();
    mfo_layer_set_synth(e, 0, &pad);
    mfo_layer_set_impulse(e, 0, &bm.b, bm.k);
    mfo_set_param(e, MFO_LPARAM(0, MFO_L_BODY), 1.0f);
    mfo_layer_set_sample(e, 1, &pb);
    mfo_layer_set_impulse(e, 1, &sp.b, sp.k);
    mfo_set_param(e, MFO_LPARAM(1, MFO_L_BODY), 0.9f);
    mfo_set_param(e, MFO_LPARAM(1, MFO_L_TAPE), 1);
    mfo_set_param(e, MFO_LPARAM(1, MFO_L_LEVEL), 1.2f);
    mfo_layer_set_synth(e, 2, &pad);
    mfo_layer_set_impulse(e, 2, &vx.b, NULL);
    mfo_set_param(e, MFO_LPARAM(2, MFO_L_TONE), 1.0f);
    mfo_set_param(e, MFO_LPARAM(2, MFO_L_GRAIN), 0.35f);
    mfo_set_param(e, MFO_LPARAM(2, MFO_L_SCAN_RATE), 0.5f);
    mfo_set_param(e, MFO_LPARAM(2, MFO_L_TUNE), 12);
    mfo_set_param(e, MFO_P_ORBIT_PATH, 1);
    mfo_set_param(e, MFO_P_ORBIT_RATE, 1.0f / 8.0f);
    mfo_set_param(e, MFO_P_ORBIT_SIZE_X, 0.7f);
    mfo_set_param(e, MFO_P_ORBIT_WOBBLE, 0.3f);
    mfo_set_param(e, MFO_P_ORBIT_ROTATE, 0.375f);               /* start at node 1 */
    mfo_set_param(e, MFO_P_DRIVE, 0.25f);
    puck(e, 0, 0);
    ev_t sc[32]; int k = 0;
    static const int ch[4][3] = { { 50, 57, 62 }, { 46, 53, 62 }, { 48, 55, 60 }, { 45, 52, 60 } };
    for (int c = 0; c < 4; c++)
        for (int v = 0; v < 3; v++) sc[k++] = (ev_t){ 0.1f + c * 4.0f, 3.7f, ch[c][v], 95 };
    for (int i = 0; i < 16; i++) sc[k++] = (ev_t){ 0.1f + i * 1.0f + 0.5f, 0.2f, 74 + ((i * 5) % 7), 80 };
    render_score(e, sc, k, 18.0f, l, r, NULL, NULL);
    write_wav("orbit_sweep_three_layers.wav", l, r, (int)(18 * SR), NULL);
    mfo_engine_destroy(e); free_imp(&bm); free_imp(&sp); free_imp(&vx); free(print);
}

static void write_impulses(float *l, float *r)
{
    /* the procedural impulses themselves, for listening */
    static const int t[] = { MFO_GEN_SPRING, MFO_GEN_PLATE, MFO_GEN_WOOD_BOX, MFO_GEN_BOWED_METAL, MFO_GEN_TAPE_HISS,
                             MFO_GEN_FORMANT_VOX, MFO_GEN_METAL_HIT, MFO_GEN_GLASS };
    static const float secs[] = { 3, 3, 0.6f, 4, 1.5f, 3, 2, 3 };
    for (unsigned i = 0; i < sizeof t / sizeof t[0]; i++) {
        int n = (int)(secs[i] * SR);
        mfo_generate_impulse(t[i], l, n, SR, 100 + i);
        memcpy(r, l, sizeof(float) * n);
        char name[64], *q;
        snprintf(name, sizeof name, "impulse_%s.wav", mfo_generate_name(t[i]));
        for (q = name; *q; q++) { if (*q == ' ') *q = '_'; else if (*q >= 'A' && *q <= 'Z') *q = (char)(*q - 'A' + 'a'); }
        write_wav(name, l, r, n, NULL);
    }
}

int main(int argc, char **argv)
{
    if (argc > 1) snprintf(outdir, sizeof outdir, "%s", argv[1]);
    int cap = (int)(20 * SR);
    float *l = malloc(sizeof(float) * cap), *r = malloc(sizeof(float) * cap);
    printf("ORBIT demos -> %s\n", outdir);
    demo_body_bowed(l, r);
    demo_tone_formant(l, r);
    demo_grain(l, r);
    demo_motion(l, r);
    demo_pitch(l, r);
    demo_keytrack(l, r);
    demo_orbit(l, r);
    write_impulses(l, r);
    free(l); free(r);
    return 0;
}
