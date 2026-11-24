/* SPDX-License-Identifier: AGPL-3.0-only
 * Vintage Chonk ORBIT: host tests of the poly-convolution / cross-synthesis core (dsp/orbit).
 * Measurements, not just "it runs": FFT against a naive DFT, partitioned convolution against direct
 * convolution, zero latency of BODY, transparency of every matrix dimension at 0, stability with silence,
 * DC and full-scale noise, the sample player's loop/reverse, the blender, keytrack and synth sources. */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "mfo.h"

#define PI_D 3.14159265358979323846
static int fails, checks;
#define CHECK(cond, ...) do { checks++; if (!(cond)) { fails++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

static uint32_t rs = 12345u;
static float frand(void) { rs = rs * 1664525u + 1013904223u; return (float)(rs >> 8) / 8388608.0f - 1.0f; }

/* ------------------------------------------------------------------ FFT --- */
static void test_fft(void)
{
    double worst_dft = 0, worst_rt = 0;
    for (int n = 4; n <= 8192; n *= 2) {
        mfo_fft_t *f = mfo_fft_create(n);
        float *x = malloc(sizeof(float) * n), *y = malloc(sizeof(float) * n);
        float *re = malloc(sizeof(float) * (n / 2 + 1)), *im = malloc(sizeof(float) * (n / 2 + 1));
        for (int i = 0; i < n; i++) x[i] = frand();
        mfo_rfft(f, x, re, im);
        if (n <= 4096) {
            double num = 0, den = 0;
            for (int k = 0; k <= n / 2; k++) {
                double sr = 0, si = 0;
                for (int i = 0; i < n; i++) { double a = -2.0 * PI_D * (double)k * i / n; sr += x[i] * cos(a); si += x[i] * sin(a); }
                num += (sr - re[k]) * (sr - re[k]) + (si - im[k]) * (si - im[k]);
                den += sr * sr + si * si;
            }
            double rel = sqrt(num / den);
            if (rel > worst_dft) worst_dft = rel;
            CHECK(rel < 1e-5, "FFT n=%d vs DFT relative error %.3g", n, rel);
        }
        mfo_irfft(f, re, im, y);
        double mx = 0;
        for (int i = 0; i < n; i++) { double d = fabs(y[i] / n - x[i]); if (d > mx) mx = d; }
        if (mx > worst_rt) worst_rt = mx;
        CHECK(mx < 1e-5, "FFT n=%d round trip max error %.3g", n, mx);
        free(x); free(y); free(re); free(im);
        mfo_fft_destroy(f);
    }
    printf("fft: sizes 4..8192, worst relative error vs naive DFT %.2e (to 4096), worst round-trip error %.2e\n", worst_dft, worst_rt);
}

/* ---------------------------------------------------------- convolution --- */
/* run conv over T samples with chunk sizes from `bs` (0 = random 1..700); compare against direct */
static double conv_case(int L, int inch, int kch, int bs, int sparse)
{
    float *h[2], *x[2], *y[2];
    int T = L + 3000;
    for (int c = 0; c < 2; c++) {
        h[c] = calloc((size_t)L, sizeof(float));
        x[c] = malloc(sizeof(float) * T);
        y[c] = calloc((size_t)T, sizeof(float));
        if (sparse) {
            for (int i = 0; i < L && i < 700; i++) h[c][i] = frand() * expf(-i / 200.0f);
            for (int k = 0; k < 60; k++) h[c][(int)((frand() * 0.5f + 0.5f) * (L - 1))] = frand() * 0.5f;
            h[c][L - 1] = 0.3f;
        } else {
            for (int i = 0; i < L; i++) h[c][i] = frand() * expf(-3.0f * i / L);
        }
        for (int i = 0; i < T; i++) x[c][i] = frand();
    }
    const float *hp[2] = { h[0], h[1] };
    mfo_kernel_t *k = mfo_kernel_create(hp, kch, L, 1.0f);
    mfo_conv_t *cv = mfo_conv_create(inch, L);
    int pos = 0;
    while (pos < T) {
        int m = bs ? bs : 1 + (int)((frand() * 0.5f + 0.5f) * 699);
        if (pos + m > T) m = T - pos;
        const float *ip[2] = { x[0] + pos, x[1] + pos };
        float *op[2] = { y[0] + pos, y[1] + pos };
        mfo_conv_process(cv, k, ip, op, m);
        pos += m;
    }
    int *nzi[2] = { malloc(sizeof(int) * L), malloc(sizeof(int) * L) }, nnz[2] = { 0, 0 };
    for (int c = 0; c < 2; c++) for (int j = 0; j < L; j++) if (h[c][j] != 0.0f) nzi[c][nnz[c]++] = j;
    double num = 0, den = 0;
    for (int o = 0; o < 2; o++) {
        const float *xi = x[inch == 2 ? o : 0], *hk = h[kch == 2 ? o : 0];
        for (int i = 0; i < T; i += (L > 20000 && !sparse ? 7 : 1)) {
            double s = 0;
            if (sparse) { for (int q = 0; q < nnz[kch == 2 ? o : 0]; q++) { int j = nzi[kch == 2 ? o : 0][q]; if (j <= i) s += (double)hk[j] * xi[i - j]; } }
            else for (int j = 0; j < L && j <= i; j++) s += (double)hk[j] * xi[i - j];
            num += (s - y[o][i]) * (s - y[o][i]);
            den += s * s;
        }
    }
    mfo_conv_destroy(cv);
    mfo_kernel_destroy(k);
    for (int c = 0; c < 2; c++) { free(h[c]); free(x[c]); free(y[c]); free(nzi[c]); }
    return sqrt(num / (den > 0 ? den : 1));
}

static void test_conv(void)
{
    static const int lens[] = { 1, 7, 63, 64, 65, 200, 511, 512, 513, 1000, 4095, 4096, 4097, 9000, 20000 };
    static const int blocks[] = { 1, 17, 64, 256, 1000, 0 };
    double worst = 0;
    int cases = 0;
    for (unsigned a = 0; a < sizeof lens / sizeof lens[0]; a++)
        for (unsigned b = 0; b < sizeof blocks / sizeof blocks[0]; b++) {
            if (blocks[b] == 1 && lens[a] > 5000) continue;
            int inch = 1 + (int)((a + b) % 2), kch = 1 + (int)((a / 2 + b) % 2);
            double e = conv_case(lens[a], inch, kch, blocks[b], 0);
            cases++;
            if (e > worst) worst = e;
            CHECK(e < 1e-4, "conv L=%d block=%d in=%d ir=%d relative error %.3g", lens[a], blocks[b], inch, kch, e);
        }
    /* long (up to 6 s at 48 kHz) impulses: dense head + sparse taps out to the end */
    static const int longs[] = { 48000, 192000, 288000 };
    for (unsigned a = 0; a < 3; a++) {
        double e = conv_case(longs[a], 2, 2, 0, 1);
        cases++;
        if (e > worst) worst = e;
        CHECK(e < 1e-4, "conv long L=%d relative error %.3g", longs[a], e);
    }
    printf("conv: %d cases (IR 1..288000 taps, blocks 1/17/64/256/1000/random, mono/stereo), worst relative error %.2e\n", cases, worst);
}

static void test_zero_latency(void)
{
    /* impulse response of the convolver equals the IR from sample 0 */
    int L = 10000;
    float *h = malloc(sizeof(float) * L), *x = calloc(L + 100, sizeof(float)), *y = calloc(L + 100, sizeof(float));
    for (int i = 0; i < L; i++) h[i] = frand();
    x[0] = 1.0f;
    const float *hp[1] = { h };
    mfo_kernel_t *k = mfo_kernel_create(hp, 1, L, 1.0f);
    mfo_conv_t *c = mfo_conv_create(1, L);
    for (int p = 0; p < L + 100; p += 37) {
        int m = L + 100 - p < 37 ? L + 100 - p : 37;
        const float *ip[1] = { x + p };
        float *op[2] = { y + p, NULL };
        mfo_conv_process(c, k, ip, op, m);
    }
    double mx = 0;
    for (int i = 0; i < L; i++) if (fabs(y[i] - h[i]) > mx) mx = fabs(y[i] - h[i]);
    CHECK(fabs(y[0] - h[0]) < 1e-6 && mx < 1e-5, "BODY impulse response: y[0]=%g h[0]=%g, max error %.3g", y[0], h[0], mx);
    mfo_conv_destroy(c); mfo_kernel_destroy(k);
    /* a pure delta kernel passes the input through on the same sample (host blocks of 1) */
    float d = 1.0f;
    const float *dp[1] = { &d };
    k = mfo_kernel_create(dp, 1, 1, 1.0f);
    c = mfo_conv_create(1, 1);
    int same = 1;
    for (int i = 0; i < 1000; i++) {
        float in = frand(), out = 0;
        const float *ip[1] = { &in };
        float *op[2] = { &out, NULL };
        mfo_conv_process(c, k, ip, op, 1);
        if (out != in) same = 0;
    }
    CHECK(same, "delta kernel: output not identical to input on the same sample");
    mfo_conv_destroy(c); mfo_kernel_destroy(k);
    printf("zero latency: BODY impulse response starts at sample 0 (max error %.2e); delta kernel bit-exact pass-through\n", mx);
    free(h); free(x); free(y);
}

/* ------------------------------------------------------ cross-synthesis --- */
static double xsyn_run(mfo_xsyn_params_t *p, const float *s0, const float *s1, const float *mod, float *o0, float *o1, int T, mfo_xsyn_t *x)
{
    for (int pos = 0; pos < T; pos += MFO_BLOCK) {
        const float *ip[2] = { s0 + pos, s1 + pos };
        float *op[2] = { o0 + pos, o1 + pos };
        mfo_xsyn_process(x, p, ip, mod ? mod + pos : NULL, op, MFO_BLOCK);
    }
    return 0;
}

static double rel_err_delayed(const float *ref, const float *out, int T, int lat)
{
    double num = 0, den = 0;
    for (int i = lat + 2048; i < T; i++) { double d = out[i] - ref[i - lat]; num += d * d; den += (double)ref[i - lat] * ref[i - lat]; }
    return sqrt(num / den);
}

static float peak_hz(const float *x, int n, float sr)
{
    int N = 8192;
    mfo_fft_t *f = mfo_fft_create(N);
    float *b = calloc(N, sizeof(float)), *re = malloc(sizeof(float) * (N / 2 + 1)), *im = malloc(sizeof(float) * (N / 2 + 1));
    for (int i = 0; i < N && i < n; i++) b[i] = x[i] * (0.5f - 0.5f * cosf(2.0f * (float)PI_D * i / N));
    mfo_rfft(f, b, re, im);
    int best = 1; float bm = 0;
    for (int k = 1; k < N / 2; k++) { float m = re[k] * re[k] + im[k] * im[k]; if (m > bm) { bm = m; best = k; } }
    free(b); free(re); free(im); mfo_fft_destroy(f);
    return best * sr / N;
}

static double band_energy(const float *x, int n, float sr, float f_lo, float f_hi)
{
    int N = 4096; double e = 0;
    mfo_fft_t *f = mfo_fft_create(N);
    float *b = malloc(sizeof(float) * N), *re = malloc(sizeof(float) * (N / 2 + 1)), *im = malloc(sizeof(float) * (N / 2 + 1));
    for (int s = 0; s + N <= n; s += N) {
        for (int i = 0; i < N; i++) b[i] = x[s + i] * (0.5f - 0.5f * cosf(2.0f * (float)PI_D * i / N));
        mfo_rfft(f, b, re, im);
        for (int k = 0; k <= N / 2; k++) { float hz = k * sr / N; if (hz >= f_lo && hz < f_hi) e += re[k] * re[k] + im[k] * im[k]; }
    }
    free(b); free(re); free(im); mfo_fft_destroy(f);
    return e;
}

static void test_xsyn(void)
{
    const float sr = 48000.0f;
    int T = 48000;
    float *s0 = malloc(sizeof(float) * T), *s1 = malloc(sizeof(float) * T), *m = malloc(sizeof(float) * T);
    float *o0 = malloc(sizeof(float) * T), *o1 = malloc(sizeof(float) * T);
    for (int i = 0; i < T; i++) {
        s0[i] = 0.5f * sinf(2.0f * (float)PI_D * 220.0f * i / sr) + 0.2f * frand();
        s1[i] = 0.3f * frand();
        m[i] = 0.4f * frand();
    }
    mfo_xsyn_t *x = mfo_xsyn_create(sr, 1024, 128);
    int lat = mfo_xsyn_latency(x);
    CHECK(lat == 256, "STFT latency %d, expected 256", lat);
    printf("xsyn: STFT 1024 / hop 128, latency %d samples = %.2f ms at 48 kHz\n", lat, 1000.0 * lat / sr);
    mfo_xsyn_params_t p = { 0, 0, 0, 0, 220.0f, 1 };
    /* all dimensions 0, STFT forced on: perfect reconstruction, delayed by the latency */
    xsyn_run(&p, s0, s1, m, o0, o1, T, x);
    double e0 = rel_err_delayed(s0, o0, T, lat), e1 = rel_err_delayed(s1, o1, T, lat);
    CHECK(e0 < 1e-4 && e1 < 1e-4, "STFT at amount 0 not transparent: %.3g %.3g", e0, e1);
    printf("xsyn: TONE=GRAIN=PITCH=0 through the running STFT: relative error vs delayed input %.2e / %.2e\n", e0, e1);
    /* bypass path (nothing spectral on): bit exact delay; MOTION 0 is exact too */
    mfo_xsyn_reset(x);
    p.force_stft = 0;
    xsyn_run(&p, s0, s1, m, o0, o1, T, x);
    int exact = 1;
    for (int i = lat; i < T; i++) if (o0[i] != s0[i - lat] || o1[i] != s1[i - lat]) { exact = 0; break; }
    CHECK(exact, "bypass (all dims 0) is not a bit-exact delay");
    printf("xsyn: all dimensions 0 (bypass, MOTION 0): bit-exact delay of %d samples\n", lat);
    /* switching a dimension on from 0 must not click: compare the first frames against the delayed input */
    {
        mfo_xsyn_reset(x);
        double worst = 0;
        for (int pos = 0; pos < T; pos += MFO_BLOCK) {
            p.tone = pos < T / 2 ? 0.0f : 1e-3f;          /* tiny amount: output must stay ~ the input */
            const float *ip[2] = { s0 + pos, s1 + pos };
            float *op[2] = { o0 + pos, o1 + pos };
            mfo_xsyn_process(x, &p, ip, m + pos, op, MFO_BLOCK);
        }
        for (int i = T / 2; i < T / 2 + 4096; i++) { double d = fabs(o0[i] - s0[i - lat]); if (d > worst) worst = d; }
        CHECK(worst < 0.01, "engaging the STFT from bypass jumps by %.3g", worst);
        printf("xsyn: engaging the STFT from bypass (TONE 0 -> 0.001): max deviation %.2e\n", worst);
        p.tone = 0;
    }
    /* each dimension at amount 1 does something measurable */
    {
        /* TONE: white-noise carrier, low-passed modulator -> high band falls */
        float *wn = malloc(sizeof(float) * T), *lpm = malloc(sizeof(float) * T);
        float lp = 0, lp2 = 0;
        for (int i = 0; i < T; i++) { wn[i] = 0.3f * frand(); lp += (frand() - lp) * 0.05f; lp2 += (lp - lp2) * 0.05f; lpm[i] = lp2 * 4.0f; }
        mfo_xsyn_reset(x);
        p.tone = 1.0f;
        xsyn_run(&p, wn, wn, lpm, o0, o1, T, x);
        double hi_in = band_energy(wn + 8192, T - 8192, sr, 4000, 20000), lo_in = band_energy(wn + 8192, T - 8192, sr, 50, 500);
        double hi_out = band_energy(o0 + 8192, T - 8192, sr, 4000, 20000), lo_out = band_energy(o0 + 8192, T - 8192, sr, 50, 500);
        double tilt = 10.0 * log10((hi_out / lo_out) / (hi_in / lo_in));
        CHECK(tilt < -20.0, "TONE=1 with a dark modulator: high/low balance moved only %.1f dB", tilt);
        printf("xsyn: TONE=1 (white carrier, dark modulator): high/low band balance %.1f dB\n", tilt);
        p.tone = 0;
        /* PITCH: modulator = one partial at 455 Hz, played note 220 Hz -> moved to the 2nd harmonic, 440 Hz */
        for (int i = 0; i < T; i++) m[i] = 0.5f * sinf(2.0f * (float)PI_D * 455.0f * i / sr);
        mfo_xsyn_reset(x);
        p.pitch = 1.0f; p.f0 = 220.0f;
        xsyn_run(&p, wn, wn, m, o0, o1, T, x);
        float pk = peak_hz(o0 + 16384, T - 16384, sr);
        CHECK(fabsf(pk - 440.0f) < 8.0f, "PITCH=1: 455 Hz partial should land on 440 Hz, got %.1f", pk);
        printf("xsyn: PITCH=1 (455 Hz partial, note 220 Hz): output peak %.1f Hz (harmonic 2 = 440)\n", pk);
        p.pitch = 0;
        /* GRAIN: sine carrier, noise modulator -> output becomes noise-like (spectral flatness rises) */
        for (int i = 0; i < T; i++) m[i] = 0.4f * frand();
        float *sine = malloc(sizeof(float) * T);
        for (int i = 0; i < T; i++) sine[i] = 0.5f * sinf(2.0f * (float)PI_D * 1000.0f * i / sr);
        mfo_xsyn_reset(x);
        p.grain = 1.0f;
        xsyn_run(&p, sine, sine, m, o0, o1, T, x);
        double on = band_energy(o0 + 8192, T - 8192, sr, 900, 1100), off = band_energy(o0 + 8192, T - 8192, sr, 2000, 20000);
        double on_in = band_energy(sine + 8192, T - 8192, sr, 900, 1100), off_in = band_energy(sine + 8192, T - 8192, sr, 2000, 20000);
        double ratio_db = 10.0 * log10((off / on) / (off_in / on_in + 1e-30));
        CHECK(ratio_db > 20.0, "GRAIN=1 (sine carrier, noise modulator): off-peak energy rose only %.1f dB", ratio_db);
        printf("xsyn: GRAIN=1 (sine carrier, noise modulator): off-peak / peak energy up %.1f dB\n", ratio_db);
        p.grain = 0;
        /* MOTION: gated modulator (on 50 ms / off 50 ms) chops the carrier */
        for (int i = 0; i < T; i++) m[i] = ((i / 2400) & 1) ? 0.0f : 0.5f * frand();
        mfo_xsyn_reset(x);
        p.motion = 1.0f;
        xsyn_run(&p, sine, sine, m, o0, o1, T, x);
        double eon = 0, eoff = 0;
        for (int i = 9600; i < T; i++) {
            int src_i = i - lat;
            if (((src_i / 2400) & 1) == 0 && (src_i % 2400) > 400) eon += o0[i] * o0[i];
            else if (((src_i / 2400) & 1) == 1 && (src_i % 2400) > 1200) eoff += o0[i] * o0[i];   /* after 25 ms of silence */
        }
        double chop = 10.0 * log10(eon / (eoff + 1e-20));
        CHECK(chop > 15.0, "MOTION=1: on/off contrast only %.1f dB", chop);
        printf("xsyn: MOTION=1 (gated modulator): on/off contrast %.1f dB, aligned to the latency\n", chop);
        p.motion = 0;
        free(wn); free(lpm); free(sine);
    }
    mfo_xsyn_destroy(x);
    free(s0); free(s1); free(m); free(o0); free(o1);
}

/* --------------------------------------------------------------- engine --- */
static mfo_engine_t *neutral_engine(int spectral)
{
    mfo_config_t c;
    mfo_config_default(&c);
    c.spectral = spectral;
    mfo_engine_t *e = mfo_engine_create(&c);
    mfo_set_param(e, MFO_P_DRIFT, 0);
    mfo_set_param(e, MFO_P_DRIVE, 0);
    mfo_set_param(e, MFO_P_MASTER, 1);
    mfo_set_param(e, MFO_P_PUCK_X, -0.7f);
    mfo_set_param(e, MFO_P_PUCK_Y, 0.7f);
    for (int l = 0; l < MFO_LAYERS; l++) {
        mfo_set_param(e, MFO_LPARAM(l, MFO_L_ATTACK), 0);
        mfo_set_param(e, MFO_LPARAM(l, MFO_L_SUSTAIN), 1);
        mfo_set_param(e, MFO_LPARAM(l, MFO_L_VEL_SENS), 0);
        mfo_set_param(e, MFO_LPARAM(l, MFO_L_BODY), 0);
    }
    return e;
}

static void test_sample_player(void)
{
    const int len = 4800;
    float *ramp = malloc(sizeof(float) * len);
    for (int i = 0; i < len; i++) ramp[i] = 0.4f * (float)i / len;
    mfo_buffer_t b = { { ramp, NULL }, 1, len, 48000.0f, 60.0f };
    float w[4];
    int T = 4000;
    float *l = malloc(sizeof(float) * 3 * len), *r = malloc(sizeof(float) * 3 * len);
    /* forward */
    mfo_engine_t *e = neutral_engine(0);
    CHECK(mfo_latency_samples(e) == 0, "BODY-only engine latency %d", mfo_latency_samples(e));
    mfo_layer_set_sample(e, 0, &b);
    mfo_render(e, l, r, 64);                 /* settle the smoothed parameters */
    mfo_blend_weights(e, -0.7f, 0.7f, w);
    float g = w[0] + w[3];
    mfo_note_on(e, 60, 100);
    mfo_render(e, l, r, T);
    double mx = 0;
    for (int i = 64; i < T; i++) { double d = fabs(l[i] - g * ramp[i]); if (d > mx) mx = d; }
    CHECK(mx < 1e-5, "forward playback deviates by %.3g", mx);
    double fwd = mx;
    mfo_engine_destroy(e);
    /* reverse */
    e = neutral_engine(0);
    mfo_layer_set_sample(e, 0, &b);
    mfo_set_param(e, MFO_LPARAM(0, MFO_L_REVERSE), 1);
    mfo_render(e, l, r, 64);
    mfo_note_on(e, 60, 100);
    mfo_render(e, l, r, T);
    mx = 0;
    for (int i = 64; i < T; i++) { double d = fabs(l[i] - g * ramp[len - 1 - i]); if (d > mx) mx = d; }
    CHECK(mx < 1e-5, "reverse playback deviates by %.3g", mx);
    double rev = mx;
    mfo_engine_destroy(e);
    /* loop 25%..75%, no crossfade: wraps exactly */
    e = neutral_engine(0);
    mfo_layer_set_sample(e, 0, &b);
    mfo_set_param(e, MFO_LPARAM(0, MFO_L_LOOP), 1);
    mfo_set_param(e, MFO_LPARAM(0, MFO_L_LOOP_START), 0.25f);
    mfo_set_param(e, MFO_LPARAM(0, MFO_L_LOOP_END), 0.75f);
    mfo_set_param(e, MFO_LPARAM(0, MFO_L_XFADE), 0.0f);
    mfo_render(e, l, r, 64);
    mfo_note_on(e, 60, 100);
    mfo_render(e, l, r, 3 * len);
    mx = 0;
    int ls = len / 4, le = 3 * len / 4;
    for (int i = 64; i < 3 * len; i++) {
        int p = i;
        while (p >= le) p -= le - ls;
        /* the cubic interpolator reads one sample across the loop seam: skip 2 samples at each seam */
        int q = i >= le ? (i - le) % (le - ls) : 99;
        if (q < 2 || q > le - ls - 3) continue;
        double d = fabs(l[i] - g * ramp[p]);
        if (d > mx) mx = d;
    }
    CHECK(mx < 1e-5, "loop playback deviates by %.3g", mx);
    double lp = mx;
    /* loop with crossfade: no jump at the seam bigger than the ramp's own steps */
    mfo_engine_reset(e);
    mfo_set_param(e, MFO_LPARAM(0, MFO_L_XFADE), 0.2f);
    mfo_render(e, l, r, 64);
    mfo_note_on(e, 60, 100);
    mfo_render(e, l, r, 3 * len);
    double jump = 0;
    for (int i = 200; i < 3 * len; i++) { double d = fabs(l[i] - l[i - 1]); if (d > jump) jump = d; }
    CHECK(jump < 0.02, "crossfaded loop: largest sample step %.3g", jump);
    mfo_engine_destroy(e);
    printf("sample: forward max error %.1e, reverse %.1e, loop (25..75%%) %.1e, crossfaded loop max step %.4f (no seam click)\n", fwd, rev, lp, jump);
    /* pitch: an octave up plays twice as fast */
    e = neutral_engine(0);
    mfo_layer_set_sample(e, 0, &b);
    mfo_render(e, l, r, 64);
    mfo_note_on(e, 72, 100);
    mfo_render(e, l, r, 2000);
    mx = 0;
    for (int i = 64; i < 2000; i++) { double d = fabs(l[i] - g * ramp[2 * i]); if (d > mx) mx = d; }
    CHECK(mx < 1e-4, "octave-up playback deviates by %.3g", mx);
    mfo_engine_destroy(e);
    /* stretch 2x: pitch kept (a 440 Hz sample stays at 440 Hz), length doubled */
    {
        int sl = 48000;
        float *sn = malloc(sizeof(float) * sl);
        for (int i = 0; i < sl; i++) sn[i] = 0.3f * sinf(2.0f * (float)PI_D * 440.0f * i / 48000.0f);
        mfo_buffer_t sb = { { sn, NULL }, 1, sl, 48000.0f, 60.0f };
        e = neutral_engine(0);
        mfo_layer_set_sample(e, 0, &sb);
        mfo_set_param(e, MFO_LPARAM(0, MFO_L_STRETCH), 2.0f);
        mfo_render(e, l, r, 64);
        mfo_note_on(e, 60, 100);
        float *ol = malloc(sizeof(float) * 3 * sl), *or_ = malloc(sizeof(float) * 3 * sl);
        mfo_render(e, ol, or_, 3 * sl);
        float hz = peak_hz(ol + 4800, 3 * sl - 4800, 48000.0f);
        int last = 0;
        for (int i = 0; i < 3 * sl; i++) if (fabsf(ol[i]) > 0.01f) last = i;
        CHECK(fabsf(hz - 440.0f) < 8.0f, "stretch 2: pitch moved to %.1f Hz", hz);
        CHECK(last > 1.8 * sl && last < 2.2 * sl, "stretch 2: sound lasted %d samples for a %d-sample source", last, sl);
        printf("sample: stretch x2 keeps pitch (%.1f Hz for 440) and lasts %.2fx the source\n", hz, (double)last / sl);
        free(ol); free(or_); free(sn);
        mfo_engine_destroy(e);
    }
    free(l); free(r); free(ramp);
}

static void test_engine_body_latency(void)
{
    /* engine with spectral off: a click through BODY with an IR comes out at the same sample */
    mfo_engine_t *e = neutral_engine(0);
    float click[256] = { 0 };
    click[0] = 0.4f;
    mfo_buffer_t b = { { click, NULL }, 1, 256, 48000.0f, 60.0f };
    int L = 6000;
    float *ir = malloc(sizeof(float) * L);
    for (int i = 0; i < L; i++) ir[i] = (i == 0 ? 0.8f : 0.0f) + 0.2f * frand() * expf(-i / 800.0f);
    const float *ip[1] = { ir };
    mfo_kernel_t *k = mfo_kernel_create(ip, 1, L, 1.0f);
    mfo_layer_set_sample(e, 0, &b);
    mfo_layer_set_impulse(e, 0, NULL, k);
    mfo_set_param(e, MFO_LPARAM(0, MFO_L_BODY), 1);
    float l[8192], r[8192];
    mfo_render(e, l, r, 64 + 13);           /* note lands mid-block */
    mfo_note_on(e, 60, 100);
    mfo_render(e, l, r, 8192);
    float w[4];
    mfo_blend_weights(e, -0.7f, 0.7f, w);
    int first = -1;
    for (int i = 0; i < 8192; i++) if (fabsf(l[i]) > 1e-6f) { first = i; break; }
    /* the layer gets click*IR (BODY 100 %), DRY gets the click: both start at sample 0 */
    double mx = 0;
    for (int i = 1; i < L; i++) { double d = fabs(l[i] - w[0] * 0.4f * ir[i]); if (d > mx) mx = d; }
    CHECK(first == 0, "engine BODY output starts at sample %d (expected 0)", first);
    CHECK(mx < 1e-4, "engine BODY output differs from click*IR by %.3g", mx);
    printf("engine: spectral off -> latency %d; a click through BODY starts at output sample %d, matches click*IR to %.1e\n",
           mfo_latency_samples(e), first, mx);
    mfo_engine_destroy(e);
    mfo_kernel_destroy(k);
    free(ir);
    /* the full engine reports the STFT latency */
    mfo_config_t c; mfo_config_default(&c);
    e = mfo_engine_create(&c);
    CHECK(mfo_latency_samples(e) == 256, "spectral engine latency %d", mfo_latency_samples(e));
    printf("engine: spectral on -> latency %d samples (%.2f ms)\n", mfo_latency_samples(e), mfo_latency_ms(e));
    mfo_engine_destroy(e);
}

static void test_body_zero_transparent(void)
{
    /* BODY 0 with a kernel loaded == no kernel at all (bit exact) */
    float *sn = malloc(sizeof(float) * 48000);
    for (int i = 0; i < 48000; i++) sn[i] = 0.3f * frand();
    mfo_buffer_t b = { { sn, NULL }, 1, 48000, 48000.0f, 60.0f };
    float ir[2000];
    for (int i = 0; i < 2000; i++) ir[i] = frand() * expf(-i / 300.0f);
    const float *ip[1] = { ir };
    mfo_kernel_t *k = mfo_kernel_create(ip, 1, 2000, 1.0f);
    float *a = malloc(sizeof(float) * 20000), *bb = malloc(sizeof(float) * 20000), *r = malloc(sizeof(float) * 20000);
    for (int pass = 0; pass < 2; pass++) {
        mfo_engine_t *e = neutral_engine(1);
        mfo_layer_set_sample(e, 0, &b);
        mfo_layer_set_impulse(e, 0, &b, pass ? k : NULL);
        mfo_set_param(e, MFO_LPARAM(0, MFO_L_BODY), 0);
        mfo_note_on(e, 60, 100);
        mfo_render(e, pass ? bb : a, r, 20000);
        mfo_engine_destroy(e);
    }
    int same = memcmp(a, bb, sizeof(float) * 20000) == 0;
    CHECK(same, "BODY 0 with a kernel differs from no kernel");
    printf("engine: BODY=0 with an impulse loaded is bit-identical to no impulse\n");
    mfo_kernel_destroy(k);
    free(sn); free(a); free(bb); free(r);
}

static void test_stability(void)
{
    /* every dimension on, three layers, LIVE cross-modulation, with silence / DC / full-scale noise */
    int len = 96000;
    float *noise = malloc(sizeof(float) * len), *dc = malloc(sizeof(float) * len), *sil = calloc((size_t)len, sizeof(float));
    for (int i = 0; i < len; i++) { noise[i] = frand(); dc[i] = 1.0f; }
    int IRL = 48000;
    float *irb = malloc(sizeof(float) * IRL);
    mfo_generate_impulse(MFO_GEN_PLATE, irb, IRL, 48000.0f, 7);
    const float *ip[1] = { irb };
    mfo_kernel_t *k = mfo_kernel_create(ip, 1, IRL, 1.0f);
    mfo_buffer_t ib = { { irb, NULL }, 1, IRL, 48000.0f, 60.0f };
    const float *srcs[3] = { noise, dc, sil };
    const char *names[3] = { "full-scale noise", "DC 1.0", "silence" };
    float *l = malloc(sizeof(float) * 48000), *r = malloc(sizeof(float) * 48000);
    for (int s = 0; s < 3; s++) {
        mfo_config_t c; mfo_config_default(&c);
        mfo_engine_t *e = mfo_engine_create(&c);
        mfo_buffer_t sb = { { srcs[s], NULL }, 1, len, 48000.0f, 60.0f };
        for (int la = 0; la < 3; la++) {
            mfo_layer_set_sample(e, la, &sb);
            mfo_layer_set_impulse(e, la, &ib, k);
            int base = MFO_LPARAM(la, 0);
            mfo_set_param(e, base + MFO_L_BODY, 1);
            mfo_set_param(e, base + MFO_L_TONE, 1);
            mfo_set_param(e, base + MFO_L_GRAIN, 0.7f);
            mfo_set_param(e, base + MFO_L_PITCH, 0.6f);
            mfo_set_param(e, base + MFO_L_MOTION, 0.8f);
            mfo_set_param(e, base + MFO_L_FILTER_TYPE, 1);
            mfo_set_param(e, base + MFO_L_RESO, 1.0f);
            mfo_set_param(e, base + MFO_L_FDRIVE, 1.0f);
            mfo_set_param(e, base + MFO_L_MOD_SOURCE, la == 0 ? 2 : 0);    /* layer 1 LIVE from layer 2 */
            mfo_set_param(e, base + MFO_L_TAPE, 1);
            mfo_set_param(e, base + MFO_L_LOOP, 1);
        }
        mfo_set_param(e, MFO_P_ORBIT_PATH, 3);
        mfo_set_param(e, MFO_P_DRIFT, 1);
        mfo_set_param(e, MFO_P_DRIVE, 1);
        for (int n = 48; n < 72; n += 3) mfo_note_on(e, n, 127);
        int bad = 0; float peak = 0;
        clock_t t0 = clock();
        for (int sec = 0; sec < 3; sec++) {
            if (sec == 1) mfo_all_notes_off(e);
            mfo_render(e, l, r, 48000);
            for (int i = 0; i < 48000; i++) {
                if (!isfinite(l[i]) || !isfinite(r[i])) bad++;
                if (fabsf(l[i]) > peak) peak = fabsf(l[i]);
            }
        }
        /* then 3 s of nothing at all: tails decay; time it (denormal slow-downs would show here) */
        clock_t t1 = clock();
        float tail_peak = 0;
        for (int sec = 0; sec < 8; sec++) {
            mfo_render(e, l, r, 48000);
            for (int i = 0; i < 48000; i++) { if (!isfinite(l[i])) bad++; if (sec == 7 && fabsf(l[i]) > tail_peak) tail_peak = fabsf(l[i]); }
        }
        clock_t t2 = clock();
        double busy = (double)(t1 - t0) / CLOCKS_PER_SEC / 3.0, idle = (double)(t2 - t1) / CLOCKS_PER_SEC / 8.0;
        CHECK(bad == 0, "%s: %d non-finite samples", names[s], bad);
        CHECK(peak <= 2.0f, "%s: output peak %.2f (master soft clip should hold it)", names[s], peak);
        CHECK(idle < busy * 3.0 + 0.05, "%s: silent tail renders %.3f s per s vs %.3f busy (denormals?)", names[s], idle, busy);
        printf("stability: %-16s finite, peak %.3f, tail after 8 s %.1e, render %.3f s/s busy, %.3f s/s silent tail\n",
               names[s], peak, tail_peak, busy, idle);
        mfo_engine_destroy(e);
    }
    mfo_kernel_destroy(k);
    free(noise); free(dc); free(sil); free(irb); free(l); free(r);
}

static void test_blender(void)
{
    mfo_engine_t *e = neutral_engine(0);
    double worst = 0, minw = 1;
    for (float y = -1.5f; y <= 1.5f; y += 0.05f)
        for (float x = -1.5f; x <= 1.5f; x += 0.05f) {
            float w[4];
            mfo_blend_weights(e, x, y, w);
            double s = w[0] + w[1] + w[2] + w[3];
            if (fabs(s - 1.0) > worst) worst = fabs(s - 1.0);
            for (int i = 0; i < 4; i++) if (w[i] < minw) minw = w[i];
        }
    CHECK(worst < 1e-6, "blend weights sum off by %.3g", worst);
    CHECK(minw >= 0, "negative blend weight %.3g", minw);
    static const float nx[4] = { -0.7f, 0.7f, 0.7f, -0.7f }, ny[4] = { 0.7f, 0.7f, -0.7f, -0.7f };
    float atnode = 1;
    for (int i = 0; i < 4; i++) { float w[4]; mfo_blend_weights(e, nx[i], ny[i], w); if (w[i] < atnode) atnode = w[i]; }
    CHECK(atnode > 0.999f, "puck on a node gives that node only %.4f", atnode);
    float wc[4];
    mfo_blend_weights(e, 0, 0, wc);
    CHECK(fabsf(wc[0] - 0.25f) < 1e-4f && fabsf(wc[3] - 0.25f) < 1e-4f, "centre weights %.3f %.3f %.3f %.3f", wc[0], wc[1], wc[2], wc[3]);
    /* the orbit path moves the puck smoothly round the nodes */
    mfo_set_param(e, MFO_P_PUCK_X, 0); mfo_set_param(e, MFO_P_PUCK_Y, 0);
    mfo_set_param(e, MFO_P_ORBIT_PATH, 1);
    mfo_set_param(e, MFO_P_ORBIT_RATE, 1.0f);
    float l[480], r[480], px0 = 0, py0 = 0, maxstep = 0, rmin = 9, rmax = 0;
    for (int b = 0; b < 100; b++) {
        mfo_render(e, l, r, 480);
        float px, py;
        mfo_orbit_position(e, &px, &py);
        if (b > 5) { float st = hypotf(px - px0, py - py0); if (st > maxstep) maxstep = st; float rr = hypotf(px, py); if (rr < rmin) rmin = rr; if (rr > rmax) rmax = rr; }
        px0 = px; py0 = py;
    }
    CHECK(fabsf(rmin - 0.7f) < 0.03f && fabsf(rmax - 0.7f) < 0.03f, "circle radius %.3f..%.3f, expected 0.7", rmin, rmax);
    printf("blender: weights sum to 1 within %.1e over a 61x61 grid (min weight %.2e), node %.4f, centre 0.25 each; circle orbit r %.3f..%.3f, step %.4f per 10 ms\n",
           worst, minw, atnode, rmin, rmax, maxstep);
    mfo_engine_destroy(e);
}

static float zc_freq(const float *x, int n, float sr)
{
    double first = -1, last = -1; int count = 0;
    for (int i = 1; i < n; i++)
        if (x[i - 1] < 0 && x[i] >= 0) { double t = i - 1 + x[i - 1] / (x[i - 1] - x[i]); if (first < 0) first = t; last = t; count++; }
    return count > 1 ? (float)((count - 1) * sr / (last - first)) : 0.0f;
}

static void test_synth_and_keytrack(void)
{
    /* a synth print of a sine patch at A4 is 440 Hz at 48 kHz (MFE runs at 44.1 kHz) */
    mfe_patch_t p;
    mfe_patch_init(&p);
    p.osc[0].position = 0;
    p.filt[0].type = MFE_F_OFF;
    p.env[3].attack = 0;
    p.vel_sens = 0;
    int n = 24000;
    float *buf = malloc(sizeof(float) * n);
    int got = mfo_print_synth(&p, 69, 100, 0.5f, 0.5f, 48000.0f, buf, NULL);
    float hz = zc_freq(buf + 2000, n - 4000, 48000.0f);
    CHECK(got == n && fabsf(hz - 440.0f) < 0.5f, "synth print: %d samples, %.2f Hz (expected 440)", got, hz);
    /* SYNTH layer in the engine */
    mfo_engine_t *e = neutral_engine(0);
    mfo_layer_set_synth(e, 0, &p);
    mfo_note_on(e, 69, 100);
    float *l = malloc(sizeof(float) * 24000), *r = malloc(sizeof(float) * 24000);
    mfo_render(e, l, r, 24000);
    float hz2 = zc_freq(l + 4000, 20000, 48000.0f);
    CHECK(fabsf(hz2 - 440.0f) < 0.5f, "SYNTH layer plays %.2f Hz at A4", hz2);
    printf("synth: print of an MFE sine at A4 = %.2f Hz at 48 kHz; SYNTH layer = %.2f Hz\n", hz, hz2);
    mfo_engine_destroy(e);
    /* keytrack: +12 halves the impulse; each voice convolves with its own note's impulse */
    float ir[4800];
    mfo_generate_impulse(MFO_GEN_WOOD_BOX, ir, 4800, 48000.0f, 3);
    mfo_buffer_t ib = { { ir, NULL }, 1, 4800, 48000.0f, 60.0f };
    mfo_keytrack_t *kt = mfo_keytrack_create(&ib, 48000.0f, 48, 72, 1.0f, 1.0f);
    CHECK(kt != NULL, "keytrack set not built");
    float click[64] = { 0.4f };
    mfo_buffer_t cb = { { click, NULL }, 1, 64, 48000.0f, 60.0f };
    e = neutral_engine(0);
    mfo_layer_set_sample(e, 0, &cb);
    mfo_layer_set_keytrack(e, 0, kt);
    mfo_set_param(e, MFO_LPARAM(0, MFO_L_KEYTRACK), 1);
    mfo_set_param(e, MFO_LPARAM(0, MFO_L_BODY), 1);
    mfo_set_param(e, MFO_P_PUCK_X, 0.7f); mfo_set_param(e, MFO_P_PUCK_Y, 0.7f);   /* node 2: layer 1 silent ... */
    mfo_set_param(e, MFO_LPARAM(0, MFO_L_LEVEL), 1);
    mfo_set_param(e, MFO_P_PUCK_X, -0.7f); mfo_set_param(e, MFO_P_PUCK_Y, 0.7f);
    mfo_render(e, l, r, 64);
    mfo_note_on(e, 60, 100);
    mfo_render(e, l, r, 9600);
    float f60 = peak_hz(l, 9600, 48000.0f);
    mfo_engine_reset(e);
    mfo_render(e, l, r, 64);
    mfo_note_on(e, 72, 100);
    mfo_render(e, l, r, 9600);
    float f72 = peak_hz(l, 9600, 48000.0f);
    CHECK(fabsf(f72 / f60 - 2.0f) < 0.1f, "keytrack: strongest mode at %.1f Hz (note 60) and %.1f Hz (note 72), ratio %.3f", f60, f72, f72 / f60);
    printf("keytrack: per-voice BODY, strongest wood-box mode %.1f Hz at C4 -> %.1f Hz at C5 (ratio %.3f)\n", f60, f72, f72 / f60);
    mfo_engine_destroy(e);
    mfo_keytrack_destroy(kt);
    free(buf); free(l); free(r);
}

static void test_mod_matrix(void)
{
    mfo_engine_t *e = neutral_engine(0);
    int id = MFO_LPARAM(1, MFO_L_TONE);
    mfo_set_param(e, id, 0.2f);
    mfo_set_mod(e, 0, MFO_MS_MACRO1, id, 0.5f);
    mfo_set_mod_source(e, MFO_MS_MACRO1, 1.0f);
    float l[4800], r[4800];
    mfo_render(e, l, r, 4800);
    float v = mfo_get_param_effective(e, id);
    CHECK(fabsf(v - 0.7f) < 0.01f, "macro 1 -> L2 TONE: effective %.3f, expected 0.7", v);
    int cid = MFO_LPARAM(0, MFO_L_CUTOFF);
    mfo_set_param(e, cid, 1000.0f);
    mfo_set_mod(e, 1, MFO_MS_EXT1, cid, 0.1f);           /* log param: 0.1 of 10 octaves = +1 octave */
    mfo_set_mod_source(e, MFO_MS_EXT1, 1.0f);
    mfo_render(e, l, r, 4800);
    float fc = mfo_get_param_effective(e, cid);
    CHECK(fabsf(fc / 2000.0f - 1.0f) < 0.02f, "EXT1 -> cutoff: %.1f Hz, expected about 2000", fc);
    printf("mod matrix: MACRO1 -> L2 TONE 0.2 + 0.5 = %.3f; EXT1 -> L1 CUTOFF 1 kHz + 1 octave = %.0f Hz (smoothed)\n", v, fc);
    const mfo_param_info_t *pi = mfo_param_info(MFO_LPARAM(2, MFO_L_BODY));
    CHECK(pi && strcmp(pi->name, "L3 BODY") == 0, "param name %s", pi ? pi->name : "(null)");
    printf("params: %d total (%d global + 3 x %d layer)\n", MFO_PARAM_COUNT, MFO_P_GLOBAL_COUNT, MFO_L_COUNT);
    mfo_engine_destroy(e);
}

int main(void)
{
    test_fft();
    test_conv();
    test_zero_latency();
    test_xsyn();
    test_engine_body_latency();
    test_body_zero_transparent();
    test_sample_player();
    test_blender();
    test_synth_and_keytrack();
    test_mod_matrix();
    test_stability();
    printf("%d checks, %d failed\n", checks, fails);
    return fails ? 1 : 0;
}
