/* SPDX-License-Identifier: AGPL-3.0-only
 * Vintage Chonk ORBIT: float filters (ZDF SVF, ZDF transistor ladder), ADSR, polyphase resampler.
 * The MFE filters are Q15 fixed point at 44.1 kHz, so ORBIT has its own float versions of the same
 * topologies (TPT one-poles, resolved zero-delay feedback, saturated ladder input). */
#include <stdlib.h>
#include "mfo_internal.h"

static float prewarp(float cutoff, float sr)
{
    float fc = mfo_clampf(cutoff, 10.0f, 0.45f * sr);
    return tanf(MFO_PI * fc / sr);
}

void mfo_svf_run(mfo_svf_t *f, int type, float cutoff, float reso, float sr, float *const *io, int nch, int n)
{
    float g = prewarp(cutoff, sr);
    float k = 2.0f - 1.96f * mfo_clampf(reso, 0.0f, 1.0f);
    float a1 = 1.0f / (1.0f + g * (g + k)), a2 = g * a1, a3 = g * a2;
    for (int c = 0; c < nch; c++) {
        float ic1 = f->ic1[c], ic2 = f->ic2[c];
        float *x = io[c];
        for (int i = 0; i < n; i++) {
            float v0 = x[i];
            float v3 = v0 - ic2;
            float v1 = a1 * ic1 + a2 * v3;
            float v2 = ic2 + a2 * ic1 + a3 * v3;
            ic1 = 2.0f * v1 - ic1;
            ic2 = 2.0f * v2 - ic2;
            x[i] = type == 0 ? v2 : type == 1 ? v1 * k : v0 - k * v1 - v2;    /* LP, BP (unity peak), HP */
        }
        if (fabsf(ic1) < 1e-20f) ic1 = 0.0f;
        if (fabsf(ic2) < 1e-20f) ic2 = 0.0f;
        f->ic1[c] = ic1; f->ic2[c] = ic2;
    }
}

void mfo_ladder_run(mfo_ladder_t *f, float cutoff, float reso, float drive, float sr, float *const *io, int nch, int n)
{
    float g = prewarp(cutoff, sr);
    float G = g / (1.0f + g), G2 = G * G, G4 = G2 * G2;
    float inv1g = 1.0f / (1.0f + g);
    float k = 4.0f * mfo_clampf(reso, 0.0f, 1.0f) * 0.995f;
    float dr = 1.0f + 7.0f * mfo_clampf(drive, 0.0f, 1.0f);
    float comp = (1.0f + 0.5f * k) / dr;               /* pass-band loss of the feedback, drive back out */
    for (int c = 0; c < nch; c++) {
        float *s = f->s[c];
        float *x = io[c];
        for (int i = 0; i < n; i++) {
            float b1 = s[0] * inv1g, b2 = s[1] * inv1g, b3 = s[2] * inv1g, b4 = s[3] * inv1g;
            float S = G2 * G * b1 + G2 * b2 + G * b3 + b4;
            float u = (x[i] * dr - k * S) / (1.0f + k * G4);
            u = mfo_sat(u * 0.5f) * 2.0f;                  /* the transistor pair: soft at the input */
            float v, y = u;
            for (int st = 0; st < 4; st++) {
                v = (y - s[st]) * G;
                y = v + s[st];
                s[st] = y + v;
            }
            x[i] = y * comp;
        }
        for (int st = 0; st < 4; st++) if (fabsf(s[st]) < 1e-20f) s[st] = 0.0f;
    }
}

void mfo_env_gate(mfo_env_t *e, int on)
{
    if (on) e->stage = MFO_ENV_ATTACK;
    else if (e->stage != MFO_ENV_IDLE) e->stage = MFO_ENV_RELEASE;
}

float mfo_env_run(mfo_env_t *e, float a, float d, float s, float r, float sr, float *out, int n)
{
    float ainc = a < 0.0005f ? 1.0f : 1.0f / (a * sr);       /* 0 = instant */
    float dc = expf(-5.0f / (fmaxf(d, 0.001f) * sr));
    float rc = expf(-5.0f / (fmaxf(r, 0.001f) * sr));
    float lv = e->level;
    for (int i = 0; i < n; i++) {
        switch (e->stage) {
        case MFO_ENV_ATTACK:
            lv += ainc;
            if (lv >= 1.0f) { lv = 1.0f; e->stage = MFO_ENV_DECAY; }
            break;
        case MFO_ENV_DECAY:
            lv = s + (lv - s) * dc;
            if (fabsf(lv - s) < 1e-4f) { lv = s; e->stage = MFO_ENV_SUSTAIN; }
            break;
        case MFO_ENV_SUSTAIN:
            lv = s;
            break;
        case MFO_ENV_RELEASE:
            lv *= rc;
            if (lv < 1e-5f) { lv = 0.0f; e->stage = MFO_ENV_IDLE; }
            break;
        default:
            lv = 0.0f;
        }
        if (out) out[i] = lv;
    }
    e->level = lv;
    return lv;
}

/* ---------------------------------------------------------------- resampler --- */
#define RS_HALF (MFO_RS_TAPS / 2)
#define RS_CAP ((int)(sizeof(((mfo_resampler_t *)0)->hist[0]) / sizeof(float)))

static double bessel_i0(double x)
{
    double s = 1.0, t = 1.0;
    for (int k = 1; k < 30; k++) { t *= (x / (2.0 * k)) * (x / (2.0 * k)); s += t; }
    return s;
}

static float *make_kernel(double fc /* cutoff, fraction of the input rate */)
{
    float *k = malloc(sizeof(float) * (MFO_RS_PHASES + 1) * MFO_RS_TAPS);
    if (!k) return NULL;
    const double beta = 8.0, i0b = bessel_i0(beta);
    for (int p = 0; p <= MFO_RS_PHASES; p++)
        for (int i = 0; i < MFO_RS_TAPS; i++) {
            double d = (double)p / MFO_RS_PHASES + (RS_HALF - 1) - i;      /* tau - j */
            double x = 2.0 * fc * d;
            double sinc = fabs(x) < 1e-9 ? 1.0 : sin(3.14159265358979323846 * x) / (3.14159265358979323846 * x);
            double w = d / RS_HALF;
            double win = fabs(w) >= 1.0 ? 0.0 : bessel_i0(beta * sqrt(1.0 - w * w)) / i0b;
            k[p * MFO_RS_TAPS + i] = (float)(2.0 * fc * sinc * win);
        }
    return k;
}

int mfo_resampler_init(mfo_resampler_t *r, double in_rate, double out_rate)
{
    memset(r, 0, sizeof *r);
    r->step = in_rate / out_rate;
    double fc = 0.5 * (out_rate < in_rate ? out_rate / in_rate : 1.0) * 0.92;
    r->kernel = make_kernel(fc);
    r->fill = RS_HALF - 1;          /* zeros before the first input sample */
    r->pos = RS_HALF - 1;
    return r->kernel != NULL;
}

void mfo_resampler_free(mfo_resampler_t *r) { free(r->kernel); r->kernel = NULL; }

int mfo_resampler_space(const mfo_resampler_t *r) { return RS_CAP - r->fill; }

void mfo_resampler_push(mfo_resampler_t *r, const float *l, const float *rr, int n)
{
    if (n > RS_CAP - r->fill) n = RS_CAP - r->fill;
    memcpy(r->hist[0] + r->fill, l, sizeof(float) * (size_t)n);
    memcpy(r->hist[1] + r->fill, rr ? rr : l, sizeof(float) * (size_t)n);
    r->fill += n;
}

int mfo_resampler_pull(mfo_resampler_t *r, float *l, float *rr, int n)
{
    int got = 0;
    while (got < n) {
        int ip = (int)r->pos;
        if (ip + RS_HALF + 1 > r->fill) break;
        double frac = (r->pos - ip) * MFO_RS_PHASES;
        int p = (int)frac;
        float t = (float)(frac - p);
        const float *k0 = r->kernel + p * MFO_RS_TAPS, *k1 = k0 + MFO_RS_TAPS;
        const float *x0 = r->hist[0] + ip - (RS_HALF - 1), *x1 = r->hist[1] + ip - (RS_HALF - 1);
        float a0 = 0, a1 = 0, b0 = 0, b1 = 0;
        for (int i = 0; i < MFO_RS_TAPS; i++) {
            a0 += k0[i] * x0[i]; a1 += k1[i] * x0[i];
            b0 += k0[i] * x1[i]; b1 += k1[i] * x1[i];
        }
        l[got] = a0 + (a1 - a0) * t;
        if (rr) rr[got] = b0 + (b1 - b0) * t;
        got++;
        r->pos += r->step;
    }
    /* drop history no longer needed */
    int keep_from = (int)r->pos - (RS_HALF - 1);
    if (keep_from > 0) {
        int rem = r->fill - keep_from;
        if (rem < 0) rem = 0;
        memmove(r->hist[0], r->hist[0] + keep_from, sizeof(float) * (size_t)rem);
        memmove(r->hist[1], r->hist[1] + keep_from, sizeof(float) * (size_t)rem);
        r->fill = rem;
        r->pos -= keep_from;
    }
    return got;
}

float *mfo_resample_buffer(const float *in, int len, double ratio, int *out_len)
{
    int n = (int)(len * ratio);
    if (n < 1) n = 1;
    float *out = malloc(sizeof(float) * (size_t)n);
    double step = 1.0 / ratio;
    double fc = 0.5 * (ratio < 1.0 ? ratio : 1.0) * 0.92;
    float *k = make_kernel(fc);
    if (!out || !k) { free(out); free(k); *out_len = 0; return NULL; }
    for (int o = 0; o < n; o++) {
        double tau = o * step;
        int ip = (int)tau;
        double frac = (tau - ip) * MFO_RS_PHASES;
        int p = (int)frac;
        float t = (float)(frac - p);
        const float *k0 = k + p * MFO_RS_TAPS, *k1 = k0 + MFO_RS_TAPS;
        float a0 = 0, a1 = 0;
        for (int i = 0; i < MFO_RS_TAPS; i++) {
            int j = ip - (RS_HALF - 1) + i;
            if (j < 0 || j >= len) continue;
            a0 += k0[i] * in[j]; a1 += k1[i] * in[j];
        }
        out[o] = a0 + (a1 - a0) * t;
    }
    free(k);
    *out_len = n;
    return out;
}
