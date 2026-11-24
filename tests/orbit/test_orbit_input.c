/* SPDX-License-Identifier: AGPL-3.0-only
 * Atomic Lounge ORBIT core, 1.2.0: the live INPUT (sidechain) as SOURCE and IMPULSE, the held puck and the
 * stopped orbit. Build: see Makefile (make orbit-input). */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "mfo.h"

static int fails = 0, checks = 0;
#define CHECK(c, ...) do { checks++; if (!(c)) { fails++; printf("FAIL: "); printf(__VA_ARGS__); printf("\n"); } } while (0)

#define SR 48000.0f
#define N_SEC(s) ((int)((s) * SR))

static float *sine(int n, float hz, float amp)
{
    float *x = malloc(sizeof(float) * (size_t)n);
    for (int i = 0; i < n; i++) x[i] = amp * sinf(6.2831853f * hz * (float)i / SR);
    return x;
}
static float *noise(int n, float amp, unsigned seed)
{
    float *x = malloc(sizeof(float) * (size_t)n);
    for (int i = 0; i < n; i++) { seed = seed * 1664525u + 1013904223u; x[i] = amp * ((float)(seed >> 8) / 8388608.0f - 1.0f); }
    return x;
}
static double rms(const float *x, int n) { double s = 0; for (int i = 0; i < n; i++) s += (double)x[i] * x[i]; return sqrt(s / (n > 0 ? n : 1)); }
static double goertzel(const float *x, int n, float hz)
{
    double w = 2.0 * 3.14159265358979 * hz / SR, c = 2.0 * cos(w), s1 = 0, s2 = 0;
    for (int i = 0; i < n; i++) { double s0 = x[i] + c * s1 - s2; s2 = s1; s1 = s0; }
    return sqrt(s1 * s1 + s2 * s2 - c * s1 * s2) / n;
}
static int finite_all(const float *x, int n) { for (int i = 0; i < n; i++) if (!isfinite(x[i])) return 0; return 1; }

static mfo_buffer_t g_tone;      /* a looping 2 s sine sample at A4, the "synth" for BODY tests */
static mfo_buffer_t g_noise;     /* white noise sample (broadband source) */

static mfo_engine_t *make(void)
{
    mfo_config_t c; mfo_config_default(&c);
    c.sample_rate = SR;
    mfo_engine_t *e = mfo_engine_create(&c);
    for (int l = 0; l < MFO_LAYERS; l++) {
        mfo_set_param(e, MFO_LPARAM(l, MFO_L_BODY), 0.0f);
        mfo_set_param(e, MFO_LPARAM(l, MFO_L_LEVEL), l == 0 ? 1.0f : 0.0f);
    }
    /* puck on node 1 (layer 1 only), no drift */
    mfo_set_param(e, MFO_P_PUCK_X, -0.7f); mfo_set_param(e, MFO_P_PUCK_Y, 0.7f);
    mfo_set_param(e, MFO_P_DRIFT, 0.0f);
    mfo_set_param(e, MFO_P_MASTER, 1.0f);
    mfo_set_param(e, MFO_P_DRIVE, 0.0f);
    return e;
}

/* render `n` frames in host-sized blocks with an input (may be NULL) */
static void run(mfo_engine_t *e, const float *in, float *l, float *r, int n, int block)
{
    for (int p = 0; p < n; p += block) {
        int m = n - p < block ? n - p : block;
        mfo_render_input(e, in ? in + p : NULL, in ? in + p : NULL, l + p, r + p, m);
    }
}

int main(void)
{
    const int n = N_SEC(3.0f);
    float *L = calloc((size_t)n, sizeof(float)), *R = calloc((size_t)n, sizeof(float));
    float *L2 = calloc((size_t)n, sizeof(float)), *R2 = calloc((size_t)n, sizeof(float));
    float *s220 = sine(n, 220.0f, 0.5f), *nz = noise(n, 0.5f, 7), *zero = calloc((size_t)n, sizeof(float));
    float *tone = sine(N_SEC(2.0f), 440.0f, 0.5f), *nbuf = noise(N_SEC(2.0f), 0.5f, 99);
    g_tone = (mfo_buffer_t){ { tone, NULL }, 1, N_SEC(2.0f), SR, 69.0f };
    g_noise = (mfo_buffer_t){ { nbuf, NULL }, 1, N_SEC(2.0f), SR, 69.0f };

    /* 1. no layer uses the input: mfo_render_input with any input == mfo_render (bit-identical) */
    {
        mfo_engine_t *a = make(), *b = make();
        mfo_layer_set_sample(a, 0, &g_tone); mfo_layer_set_sample(b, 0, &g_tone);
        mfo_set_param(a, MFO_LPARAM(0, MFO_L_LOOP), 1); mfo_set_param(b, MFO_LPARAM(0, MFO_L_LOOP), 1);
        mfo_note_on(a, 69, 100); mfo_note_on(b, 69, 100);
        for (int p = 0; p < n; p += 256) { int m = n - p < 256 ? n - p : 256; mfo_render(a, L + p, R + p, m); }
        run(b, nz, L2, R2, n, 256);
        CHECK(memcmp(L, L2, sizeof(float) * (size_t)n) == 0 && memcmp(R, R2, sizeof(float) * (size_t)n) == 0,
              "input ignored when no layer uses it: bit-identical");
        printf("no INPUT layer: output bit-identical to mfo_render with a noise input\n");
        mfo_engine_destroy(a); mfo_engine_destroy(b);
    }

    /* 2. SOURCE INPUT, THRU: the input plays without notes; silence gives silence */
    {
        mfo_engine_t *e = make();
        mfo_layer_set_input(e, 0, 1, 0, 0, 0.25f);
        run(e, s220, L, R, n, 64);
        int lat = mfo_latency_samples(e);
        double ro = rms(L + N_SEC(1), N_SEC(1)), ri = rms(s220, N_SEC(1));
        double a220 = goertzel(L + N_SEC(1), N_SEC(1), 220.0f);
        CHECK(ro > 0.5 * ri && ro < 2.0 * ri, "SOURCE INPUT THRU passes the input (rms %.3f vs %.3f)", ro, ri);
        CHECK(a220 > 0.15, "SOURCE INPUT THRU keeps the 220 Hz (%.3f)", a220);
        /* alignment: output = input delayed by the core latency */
        int best = -1; double bc = -1;
        for (int d = 0; d < 1024; d++) {
            double c = 0; for (int i = 0; i < 4800; i++) c += (double)L[N_SEC(1) + i] * s220[N_SEC(1) + i - d];
            if (c > bc) { bc = c; best = d; }
        }
        /* a 220 Hz sine repeats every 218.2 samples: compare modulo the period */
        double per = SR / 220.0, dd = fmod(fabs((double)best - lat), per);
        CHECK(dd < 2.0 || per - dd < 2.0, "SOURCE INPUT delay = core latency %d (found %d mod period)", lat, best);
        printf("SOURCE INPUT THRU: rms %.3f for input %.3f, 220 Hz kept (%.3f), delay = latency %d samples\n", ro, ri, a220, lat);
        mfo_engine_reset(e);
        run(e, zero, L, R, n, 64);
        CHECK(rms(L, n) == 0.0, "silent input -> silent output (%.3g)", rms(L, n));
        mfo_engine_destroy(e);
    }

    /* 3. SOURCE INPUT, NOTE gate: closed without notes, opens with a note, closes again after release */
    {
        mfo_engine_t *e = make();
        mfo_set_param(e, MFO_LPARAM(0, MFO_L_ATTACK), 0.01f);
        mfo_set_param(e, MFO_LPARAM(0, MFO_L_RELEASE), 0.1f);
        mfo_layer_set_input(e, 0, 1, 0, 1, 0.25f);
        run(e, s220, L, R, N_SEC(0.5f), 64);
        double closed = rms(L, N_SEC(0.5f));
        mfo_note_on(e, 60, 127);
        run(e, s220 + N_SEC(0.5f), L, R, N_SEC(0.5f), 64);
        double open = rms(L + N_SEC(0.1f), N_SEC(0.4f));
        mfo_note_off(e, 60);
        run(e, s220 + N_SEC(1.0f), L, R, N_SEC(1.0f), 64);
        double after = rms(L + N_SEC(0.8f), N_SEC(0.2f));
        CHECK(closed == 0.0, "NOTE gate closed without notes (%.3g)", closed);
        CHECK(open > 0.1, "NOTE gate open with a note (%.3f)", open);
        CHECK(after < 1e-4, "NOTE gate closes after the release, no stuck tail (%.3g)", after);
        printf("SOURCE INPUT NOTE gate: closed %.3g, open %.3f, 0.8 s after note-off %.2g\n", closed, open, after);
        mfo_engine_destroy(e);
    }

    /* 4. IMPULSE INPUT, BODY: a noise source convolved with a 220 Hz input window becomes a 220 Hz tone;
     *    silent input -> silent BODY; no clicks across kernel refreshes with a steady input */
    double body_cpu = 0;
    {
        mfo_engine_t *e = make();
        mfo_layer_set_sample(e, 0, &g_noise);
        mfo_set_param(e, MFO_LPARAM(0, MFO_L_LOOP), 1);
        mfo_set_param(e, MFO_LPARAM(0, MFO_L_BODY), 1.0f);
        mfo_layer_set_input(e, 0, 0, 1, 0, 0.25f);
        mfo_note_on(e, 69, 100);
        clock_t t0 = clock();
        run(e, s220, L, R, n, 256);
        body_cpu = (double)(clock() - t0) / CLOCKS_PER_SEC / 3.0;
        const float *y = L + N_SEC(1.5f);
        double a220 = goertzel(y, N_SEC(1), 220.0f), a1k = goertzel(y, N_SEC(1), 1000.0f), a3k = goertzel(y, N_SEC(1), 3100.0f);
        CHECK(a220 > 20 * a1k && a220 > 20 * a3k, "BODY INPUT: noise through a 220 Hz window is a 220 Hz tone (%.4f vs %.4f / %.4f)", a220, a1k, a3k);
        CHECK(finite_all(L, n) && finite_all(R, n), "BODY INPUT finite");
        printf("BODY INPUT (250 ms window): noise source -> 220 Hz %.4f, 1 kHz %.5f, 3.1 kHz %.5f; %.2f%% of a core\n",
               a220, a1k, a3k, 100.0 * body_cpu);
        /* the response follows a change of the input: switch the input to 660 Hz */
        float *s660 = sine(n, 660.0f, 0.5f);
        run(e, s660, L, R, n, 256);
        int dstart = 0, dfull = 0;
        dstart = mfo_input_body_delay(e, 0, &dfull);
        const float *z = L + N_SEC(1.5f);
        double b220 = goertzel(z, N_SEC(1), 220.0f), b660 = goertzel(z, N_SEC(1), 660.0f);
        CHECK(b660 > 10 * b220, "BODY INPUT follows the input: 660 Hz %.4f vs 220 Hz %.4f", b660, b220);
        printf("BODY INPUT follows the input (660 Hz %.4f, 220 Hz %.5f); refresh: new audio from %d samples, full weight %d samples (%.1f / %.1f ms)\n",
               b660, b220, dstart, dfull, 1000.0 * dstart / SR, 1000.0 * dfull / SR);
        free(s660);
        mfo_engine_destroy(e);
    }
    {
        /* smoothness: a sine source through a steady noise input window: no step larger than the sine's own */
        mfo_engine_t *e = make();
        mfo_layer_set_sample(e, 0, &g_tone);
        mfo_set_param(e, MFO_LPARAM(0, MFO_L_LOOP), 1);
        mfo_set_param(e, MFO_LPARAM(0, MFO_L_BODY), 1.0f);
        mfo_layer_set_input(e, 0, 0, 1, 0, 0.1f);
        mfo_note_on(e, 69, 100);
        float *lp = noise(n, 0.5f, 3);
        run(e, lp, L, R, n, 128);
        double pk = 0, md = 0;
        for (int i = N_SEC(0.5f); i < n; i++) { pk = fmax(pk, fabs(L[i])); md = fmax(md, fabs(L[i] - L[i - 1])); }
        double bound = 2.0 * 3.14159265 * 440.0 / SR * pk * 1.5;
        CHECK(pk > 0.01 && md <= bound, "BODY INPUT refreshes without clicks: max step %.4f <= %.4f (peak %.3f)", md, bound, pk);
        printf("BODY INPUT, 100 ms window over 2.5 s of refreshes: max sample step %.4f (bound %.4f for a 440 Hz sine of peak %.3f)\n", md, bound, pk);
        /* silent input: BODY = 1 goes silent after a refresh, no stuck tail */
        run(e, zero, L, R, n, 128);
        double tail = rms(L + N_SEC(1.0f), N_SEC(2.0f));
        CHECK(tail < 1e-3, "BODY INPUT on a silent input goes silent (%.3g)", tail);
        printf("BODY INPUT after the input stops: rms %.2g from 1 s on\n", tail);
        free(lp);
        mfo_engine_destroy(e);
    }

    /* 5. TONE / GRAIN / MOTION / PITCH with the input as the modulator: each changes the output measurably */
    {
        static const int dims[4] = { MFO_L_TONE, MFO_L_GRAIN, MFO_L_MOTION, MFO_L_PITCH };
        static const char *names[4] = { "TONE", "GRAIN", "MOTION", "PITCH" };
        for (int d = 0; d < 4; d++) {
            double r[3];
            for (int k = 0; k < 3; k++) {
                mfo_engine_t *e = make();
                mfo_layer_set_sample(e, 0, &g_noise);
                mfo_set_param(e, MFO_LPARAM(0, MFO_L_LOOP), 1);
                mfo_set_param(e, MFO_LPARAM(0, dims[d]), 1.0f);
                mfo_layer_set_input(e, 0, 0, 1, 0, 0.25f);
                mfo_note_on(e, 57, 100);
                float *in = k == 0 ? zero : k == 1 ? s220 : nz;
                run(e, in, L, R, n, 64);
                r[k] = rms(L + N_SEC(1), N_SEC(1.5f));
                if (k == 1) {
                    /* how much of the output is at 220 Hz (the modulator's pitch) */
                    double a = goertzel(L + N_SEC(1), N_SEC(1.5f), 220.0f);
                    if (d == 3) CHECK(a > 0.02, "%s with a 220 Hz INPUT puts energy at 220 Hz (%.4f)", names[d], a);
                }
                CHECK(finite_all(L, n), "%s finite", names[d]);
                mfo_engine_destroy(e);
            }
            CHECK(fabs(r[1] - r[2]) > 0.05 * fmax(r[1], r[2]) || fabs(r[0] - r[1]) > 0.05 * fmax(r[0], r[1]),
                  "%s responds to the input (silent %.4f, sine %.4f, noise %.4f)", names[d], r[0], r[1], r[2]);
            printf("%-6s with INPUT as modulator: rms silent %.4f, 220 Hz sine %.4f, noise %.4f\n", names[d], r[0], r[1], r[2]);
        }
    }

    /* 6. full-scale noise and DC on the input, every use at once: finite and bounded */
    {
        mfo_engine_t *e = make();
        float *fs = noise(n, 1.0f, 5);
        for (int l = 0; l < MFO_LAYERS; l++) {
            mfo_set_param(e, MFO_LPARAM(l, MFO_L_LEVEL), 1.0f);
            for (int d = MFO_L_BODY; d <= MFO_L_PITCH; d++) mfo_set_param(e, MFO_LPARAM(l, d), 1.0f);
            mfo_layer_set_input(e, l, l != 1, 1, 0, l == 2 ? 1.0f : 0.25f);
        }
        mfo_layer_set_sample(e, 1, &g_tone);
        mfo_note_on(e, 60, 127);
        run(e, fs, L, R, n, 512);
        double pk = 0; for (int i = 0; i < n; i++) pk = fmax(pk, fabs(L[i]));
        CHECK(finite_all(L, n) && finite_all(R, n) && pk <= 1.5, "full-scale noise on every INPUT use: finite, peak %.3f", pk);
        for (int i = 0; i < n; i++) fs[i] = 1.0f;
        run(e, fs, L, R, n, 512);
        CHECK(finite_all(L, n), "DC 1.0 input finite");
        printf("full-scale noise into all INPUT uses on 3 layers: finite, peak %.3f; DC input finite\n", pk);
        free(fs);
        mfo_engine_destroy(e);
    }

    /* 7. held puck: wobble does not move a held puck once held wobble is 0; legacy (-1) still does */
    {
        mfo_engine_t *e = make();
        mfo_set_param(e, MFO_P_ORBIT_PATH, 0);
        mfo_set_param(e, MFO_P_ORBIT_WOBBLE, 0.8f);
        mfo_set_param(e, MFO_P_PUCK_X, 0.2f); mfo_set_param(e, MFO_P_PUCK_Y, -0.3f);
        mfo_set_held_wobble(e, 0.0f);
        float x0 = 0, y0 = 0, x, y; int moved = 0;
        float w0[4], w[4]; int wdiff = 0;
        for (int k = 0; k < 200; k++) {
            mfo_render(e, L, R, 480);
            mfo_orbit_position(e, &x, &y);
            mfo_blend_weights(e, x, y, w);
            if (k == 10) { x0 = x; y0 = y; memcpy(w0, w, sizeof w); }
            if (k > 10 && (x != x0 || y != y0)) moved = 1;
            if (k > 10 && memcmp(w, w0, sizeof w) != 0) wdiff = 1;
        }
        CHECK(!moved && !wdiff && fabsf(x0 - 0.2f) < 1e-6f && fabsf(y0 + 0.3f) < 1e-6f,
              "held puck stays at (0.2, -0.3) with WOBBLE 0.8 and puck drift 0: bit-identical blend over 2 s");
        mfo_set_held_wobble(e, -1.0f);
        moved = 0;
        for (int k = 0; k < 100; k++) { mfo_render(e, L, R, 480); mfo_orbit_position(e, &x, &y); if (x != x0 || y != y0) moved = 1; }
        CHECK(moved, "legacy held wobble (1.1.0 states) still moves the puck");
        mfo_set_held_wobble(e, 0.3f);
        moved = 0;
        for (int k = 0; k < 100; k++) { mfo_render(e, L, R, 480); mfo_orbit_position(e, &x, &y); if (fabsf(x - 0.2f) > 1e-4f) moved = 1; }
        CHECK(moved, "puck drift 0.3 moves a held puck");
        printf("held puck: fixed at (%.2f, %.2f), weights %.4f %.4f %.4f %.4f bit-identical over 2 s; legacy wobble and puck drift move it\n",
               x0, y0, w0[0], w0[1], w0[2], w0[3]);
        /* stopped orbit: a circle path with the phase frozen does not move */
        mfo_set_held_wobble(e, 0.0f);
        mfo_set_param(e, MFO_P_ORBIT_WOBBLE, 0.0f);
        mfo_set_param(e, MFO_P_ORBIT_PATH, 1);
        mfo_render(e, L, R, 4800);
        mfo_set_orbit_freeze(e, 1);
        mfo_render(e, L, R, 64);
        mfo_orbit_position(e, &x0, &y0);
        moved = 0;
        for (int k = 0; k < 100; k++) { mfo_render(e, L, R, 480); mfo_orbit_position(e, &x, &y); if (x != x0 || y != y0) moved = 1; }
        CHECK(!moved, "frozen orbit (RATE 0) does not move");
        mfo_set_orbit_freeze(e, 0);
        mfo_render(e, L, R, 4800);
        mfo_orbit_position(e, &x, &y);
        CHECK(x != x0 || y != y0, "unfrozen orbit moves again");
        printf("stopped orbit: frozen at (%.3f, %.3f) for 1 s, moves again when released\n", x0, y0);
        /* puck automation: PUCK X/Y set while held -> blend weights of that position, smoothed */
        mfo_set_param(e, MFO_P_ORBIT_PATH, 0);
        mfo_set_param(e, MFO_P_PUCK_X, -0.7f); mfo_set_param(e, MFO_P_PUCK_Y, 0.7f);
        mfo_render(e, L, R, 9600);
        mfo_orbit_position(e, &x, &y);
        mfo_blend_weights(e, x, y, w);
        CHECK(w[0] > 0.99f, "puck on node 1 -> L1 weight %.4f", w[0]);
        mfo_set_param(e, MFO_P_PUCK_X, 0.7f); mfo_set_param(e, MFO_P_PUCK_Y, -0.7f);
        mfo_render(e, L, R, 64);
        mfo_orbit_position(e, &x, &y);
        CHECK(x < 0.0f, "puck moves smoothly, not in one jump (x %.3f after 64 samples)", x);
        mfo_render(e, L, R, 9600);
        mfo_orbit_position(e, &x, &y);
        mfo_blend_weights(e, x, y, w);
        CHECK(w[2] > 0.99f, "puck on node 3 -> L3 weight %.4f", w[2]);
        printf("puck automation: node 1 -> L1 %.4f, node 3 -> L3 %.4f, smoothed (12 ms)\n", w[0] > 0 ? 1.0 : 0.0, w[2]);
        mfo_engine_destroy(e);
    }

    printf("%d checks, %d failed\n", checks, fails);
    free(L); free(R); free(L2); free(R2); free(s220); free(nz); free(zero); free(tone); free(nbuf);
    return fails ? 1 : 0;
}
