/* SPDX-License-Identifier: AGPL-3.0-only
 * Vintage Chonk ORBIT benchmark: 3 layers, 16 voices, 4 s impulses, 48 kHz, host block 256.
 * Prints the share of one CPU core for several configurations (target from docs/ORBIT_MODE.md: < 15 %). */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include "mfo.h"

static double now_s(void)
{
    struct timespec ts;
    timespec_get(&ts, TIME_UTC);
    return ts.tv_sec + ts.tv_nsec * 1e-9;
}

typedef struct { const char *name; int spectral, synth, keytrack, dims; } bench_t;

int main(int argc, char **argv)
{
    const float sr = 48000.0f;
    const int block = 256, seconds = argc > 1 ? atoi(argv[1]) : 20;
    /* material: a synth print as the sample source, procedural 4 s impulses */
    mfe_patch_t pad;
    mfe_patch_init(&pad);
    pad.osc[1].flags = MFE_OSC_ON; pad.osc[1].fine = 9; pad.osc[1].amp = 16000;
    pad.master = 3000;
    pad.env[3].attack = 16000; pad.env[3].release = 20000;
    int slen = (int)(4 * sr);
    float *smp = malloc(sizeof(float) * slen);
    mfo_print_synth(&pad, 60, 100, 3.5f, 4.0f, sr, smp, NULL);
    mfo_buffer_t sb = { { smp, NULL }, 1, slen, sr, 60.0f };
    int ilen = (int)(4 * sr);
    float *ir[3];
    mfo_kernel_t *k[3];
    mfo_buffer_t ib[3];
    mfo_keytrack_t *kt[3];
    static const int types[3] = { MFO_GEN_PLATE, MFO_GEN_SPRING, MFO_GEN_BOWED_METAL };
    for (int l = 0; l < 3; l++) {
        ir[l] = malloc(sizeof(float) * ilen);
        mfo_generate_impulse(types[l], ir[l], ilen, sr, 11u + l);
        const float *p[1] = { ir[l] };
        k[l] = mfo_kernel_create(p, 1, ilen, 0.05f);
        ib[l] = (mfo_buffer_t){ { ir[l], NULL }, 1, ilen, sr, 60.0f };
        kt[l] = mfo_keytrack_create(&ib[l], sr, 36, 84, 1.0f, 0.05f);
    }
    static const bench_t cases[] = {
        { "BODY only (spectral off), 16 sample voices x 3 layers, 4 s IRs", 0, 0, 0, 0 },
        { "all five dimensions, 16 sample voices x 3 layers, 4 s IRs     ", 1, 0, 0, 1 },
        { "all five dimensions, SYNTH sources (MFE, 3 engines)          ", 1, 1, 0, 1 },
        { "KEYTRACK BODY (8 per-voice 1 s IRs per layer) + spectral     ", 1, 0, 1, 1 },
    };
    float *l = malloc(sizeof(float) * block), *r = malloc(sizeof(float) * block);
    printf("ORBIT benchmark: %d s of audio at %.0f Hz, host block %d\n", seconds, sr, block);
    for (unsigned c = 0; c < sizeof cases / sizeof cases[0]; c++) {
        mfo_config_t cfg;
        mfo_config_default(&cfg);
        cfg.spectral = cases[c].spectral;
        cfg.max_voices = 16;
        cfg.keytrack_voices = cases[c].keytrack ? 8 : 0;
        cfg.keytrack_seconds = 1.0f;
        mfo_engine_t *e = mfo_engine_create(&cfg);
        for (int la = 0; la < 3; la++) {
            if (cases[c].synth) mfo_layer_set_synth(e, la, &pad); else mfo_layer_set_sample(e, la, &sb);
            mfo_layer_set_impulse(e, la, &ib[la], k[la]);
            if (cases[c].keytrack) { mfo_layer_set_keytrack(e, la, kt[la]); mfo_set_param(e, MFO_LPARAM(la, MFO_L_KEYTRACK), 1); }
            int b = MFO_LPARAM(la, 0);
            mfo_set_param(e, b + MFO_L_BODY, 1);
            mfo_set_param(e, b + MFO_L_LOOP, 1);
            mfo_set_param(e, b + MFO_L_SUSTAIN, 1);
            mfo_set_param(e, b + MFO_L_FILTER_TYPE, 1);
            mfo_set_param(e, b + MFO_L_CUTOFF, 3000);
            mfo_set_param(e, b + MFO_L_TAPE, 1);
            if (cases[c].dims) {
                mfo_set_param(e, b + MFO_L_TONE, 0.5f);
                mfo_set_param(e, b + MFO_L_GRAIN, 0.3f);
                mfo_set_param(e, b + MFO_L_PITCH, 0.3f);
                mfo_set_param(e, b + MFO_L_MOTION, 0.3f);
            }
        }
        mfo_set_param(e, MFO_P_ORBIT_PATH, 1);
        for (int n = 0; n < 16; n++) mfo_note_on(e, 40 + 2 * n, 100);
        int blocks = (int)(seconds * sr / block);
        for (int i = 0; i < 20; i++) mfo_render(e, l, r, block);     /* warm up */
        double t0 = now_s(), worst = 0;
        for (int i = 0; i < blocks; i++) {
            double a = now_s();
            mfo_render(e, l, r, block);
            double d = now_s() - a;
            if (d > worst) worst = d;
        }
        double t = now_s() - t0;
        printf("  %s : %5.1f %% of one core (avg), worst block %.2f ms of %.2f ms, voices %u, latency %d\n",
               cases[c].name, 100.0 * t / seconds, worst * 1000.0, 1000.0 * block / sr, mfo_active_voices(e), mfo_latency_samples(e));
        mfo_engine_destroy(e);
    }
    for (int la = 0; la < 3; la++) { mfo_kernel_destroy(k[la]); mfo_keytrack_destroy(kt[la]); free(ir[la]); }
    free(smp); free(l); free(r);
    return 0;
}
