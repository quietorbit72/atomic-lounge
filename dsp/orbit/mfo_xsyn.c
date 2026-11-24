/* SPDX-License-Identifier: AGPL-3.0-only
 * Vintage Chonk ORBIT: STFT cross-synthesis of a layer (carrier = the layer's source, modulator = the
 * impulse stream or a LIVE layer): TONE, GRAIN, PITCH in the spectral domain, MOTION in the time domain.
 *
 * Low-latency STFT: an N-point analysis window that rises slowly over the frame and falls over its last
 * hop (good frequency resolution), and a 2*hop synthesis window at the frame end, chosen so that
 * analysis x synthesis is a periodic Hann of 2*hop: overlap-add at hop gives perfect reconstruction with
 * a latency of 2*hop samples (Mauler & Martin-style asymmetric windows).
 *
 * Per frame (log2-magnitude domain; E = cepstrally smoothed log envelope):
 *   TONE   L += a_t * (E_mod - E_src + level match)           the impulse's EQ curve on the source
 *   GRAIN  L -> E_cur + (L_mod - E_mod), phase -> modulator   impulse fine structure under the source envelope
 *   PITCH  as GRAIN with the modulator's partials moved onto the played note's harmonics (phase-vocoder
 *          phase accumulators per harmonic, peak-locked regions)
 * Each blends geometrically in magnitude and by normalised vector sum in phase, so amount 0 is exact. */
#include <stdlib.h>
#include "mfo_internal.h"

struct mfo_xsyn {
    float sr;
    int N, M, K, Q;
    mfo_fft_t *fft;
    float *wa, *ws;                  /* analysis N, synthesis 2M */
    float *buf[3];                   /* N + M history: ch0, ch1, mod */
    int fill;                        /* new samples since the last frame (0..M) */
    int stagger;                     /* fill at reset: frames fall at a phase of their own */
    int active;
    float *fifo[2];                  /* M output samples per channel */
    int rd;
    float *ola[2];                   /* 2M */
    float *frame, *tre, *tim;        /* N, K, K */
    float *mre, *mim, *mlog, *menv;  /* modulator spectrum */
    float *sre[2], *sim[2], *slog[2], *senv[2];
    float *pre, *pim, *plog;         /* PITCH: re-tuned modulator */
    float *cep;                      /* N */
    float *hacc; int *hstamp; int hmax;
    int *peaks;
    int frame_no;
    float em, es[2];
    /* MOTION */
    float *gring; int glen, gpos;
    float efast, epeak, motion_prev;
    float ka, kr, kpk;
};

static void *xc(size_t n, size_t s) { return calloc(n ? n : 1, s); }

mfo_xsyn_t *mfo_xsyn_create(float sample_rate, int n, int hop)
{
    if (n < 64 || (n & (n - 1)) || hop < 16 || (hop & (hop - 1)) || hop % MFO_BLOCK || 3 * hop > n) return NULL;
    mfo_xsyn_t *x = xc(1, sizeof *x);
    if (!x) return NULL;
    x->sr = sample_rate; x->N = n; x->M = hop; x->K = n / 2 + 1;
    x->Q = (int)mfo_clampf(sample_rate * 0.0012f, 12.0f, (float)(n / 4));
    x->fft = mfo_fft_create(n);
    x->wa = xc((size_t)n, sizeof(float));
    x->ws = xc(2 * (size_t)hop, sizeof(float));
    int ok = x->fft && x->wa && x->ws;
    for (int c = 0; c < 3; c++) { x->buf[c] = xc((size_t)(n + hop), sizeof(float)); ok = ok && x->buf[c]; }
    for (int c = 0; c < 2; c++) {
        x->fifo[c] = xc((size_t)hop, sizeof(float));
        x->ola[c] = xc(2 * (size_t)hop, sizeof(float));
        x->sre[c] = xc((size_t)x->K, sizeof(float)); x->sim[c] = xc((size_t)x->K, sizeof(float));
        x->slog[c] = xc((size_t)x->K, sizeof(float)); x->senv[c] = xc((size_t)x->K, sizeof(float));
        ok = ok && x->fifo[c] && x->ola[c] && x->sre[c] && x->sim[c] && x->slog[c] && x->senv[c];
    }
    x->frame = xc((size_t)n, sizeof(float)); x->cep = xc((size_t)n, sizeof(float));
    x->tre = xc((size_t)x->K, sizeof(float)); x->tim = xc((size_t)x->K, sizeof(float));
    x->mre = xc((size_t)x->K, sizeof(float)); x->mim = xc((size_t)x->K, sizeof(float));
    x->mlog = xc((size_t)x->K, sizeof(float)); x->menv = xc((size_t)x->K, sizeof(float));
    x->pre = xc((size_t)x->K, sizeof(float)); x->pim = xc((size_t)x->K, sizeof(float)); x->plog = xc((size_t)x->K, sizeof(float));
    x->hmax = 2 * x->K + 2;
    x->hacc = xc((size_t)x->hmax, sizeof(float)); x->hstamp = xc((size_t)x->hmax, sizeof(int));
    x->peaks = xc((size_t)x->K, sizeof(int));
    x->glen = 2 * hop;
    x->gring = xc((size_t)x->glen, sizeof(float));
    ok = ok && x->frame && x->cep && x->tre && x->tim && x->mre && x->mim && x->mlog && x->menv && x->pre && x->pim &&
         x->plog && x->hacc && x->hstamp && x->peaks && x->gring;
    if (!ok) { mfo_xsyn_destroy(x); return NULL; }
    /* windows */
    int M = hop, R = n - M;                          /* rising part length R, falling part M */
    for (int i = 0; i < n; i++) {
        double h = i < R ? 0.5 - 0.5 * cos(3.14159265358979323846 * i / R)                       /* hann over 2R, rising half */
                         : 0.5 - 0.5 * cos(3.14159265358979323846 * (i - (n - 2 * M)) / M);    /* hann over 2M, falling half */
        x->wa[i] = (float)sqrt(h);
    }
    for (int i = 0; i < 2 * M; i++) {
        double h = 0.5 - 0.5 * cos(3.14159265358979323846 * i / M);    /* periodic hann of 2M */
        double a = x->wa[n - 2 * M + i];
        x->ws[i] = a > 1e-9 ? (float)(h / a) : 0.0f;
    }
    x->ka = 1.0f - expf(-1.0f / (0.001f * sample_rate));
    x->kr = 1.0f - expf(-1.0f / (0.012f * sample_rate));
    x->kpk = expf(-1.0f / (1.5f * sample_rate));
    mfo_xsyn_reset(x);
    return x;
}

void mfo_xsyn_destroy(mfo_xsyn_t *x)
{
    if (!x) return;
    mfo_fft_destroy(x->fft);
    free(x->wa); free(x->ws);
    for (int c = 0; c < 3; c++) free(x->buf[c]);
    for (int c = 0; c < 2; c++) { free(x->fifo[c]); free(x->ola[c]); free(x->sre[c]); free(x->sim[c]); free(x->slog[c]); free(x->senv[c]); }
    free(x->frame); free(x->cep); free(x->tre); free(x->tim); free(x->mre); free(x->mim); free(x->mlog); free(x->menv);
    free(x->pre); free(x->pim); free(x->plog); free(x->hacc); free(x->hstamp); free(x->peaks); free(x->gring);
    free(x);
}

void mfo_xsyn_reset(mfo_xsyn_t *x)
{
    for (int c = 0; c < 3; c++) memset(x->buf[c], 0, sizeof(float) * (size_t)(x->N + x->M));
    for (int c = 0; c < 2; c++) {
        memset(x->fifo[c], 0, sizeof(float) * (size_t)x->M);
        memset(x->ola[c], 0, sizeof(float) * 2 * (size_t)x->M);
    }
    x->fill = x->stagger; x->rd = x->stagger; x->active = 0; x->frame_no = 1;
    memset(x->hstamp, 0, sizeof(int) * (size_t)x->hmax);
    for (int i = 0; i < x->glen; i++) x->gring[i] = 1.0f;
    x->gpos = 0; x->efast = 0; x->epeak = 0; x->motion_prev = 0;
}

int mfo_xsyn_latency(const mfo_xsyn_t *x) { return 2 * x->M; }

void mfo_xsyn_set_stagger(mfo_xsyn_t *x, int offset)
{
    offset = offset / MFO_BLOCK * MFO_BLOCK;
    x->stagger = offset < 0 ? 0 : offset >= x->M ? 0 : offset;
    mfo_xsyn_reset(x);
}

/* log-magnitude (log2 |X|) -> cepstrally smoothed envelope in env */
static void smooth_env(mfo_xsyn_t *x, const float *lg, float *env)
{
    int N = x->N, K = x->K, Q = x->Q;
    memset(x->tim, 0, sizeof(float) * (size_t)K);
    mfo_irfft(x->fft, lg, x->tim, x->cep);
    float sc = 1.0f / N;
    x->cep[0] *= sc;
    for (int q = 1; q < N / 2; q++) {
        float w = q < Q ? sc : q < 2 * Q ? sc * 0.5f * (1.0f + cosf(MFO_PI * (q - Q) / Q)) : 0.0f;
        x->cep[q] *= w;
        x->cep[N - q] *= w;
    }
    x->cep[N / 2] = 0.0f;
    mfo_rfft(x->fft, x->cep, env, x->tim);
}

/* windowed frame of history channel c -> spectrum, log2 magnitude; returns energy */
static float analyse(mfo_xsyn_t *x, int c, int frame_off, float *re, float *im, float *lg)
{
    const float *b = x->buf[c] + frame_off;
    for (int i = 0; i < x->N; i++) x->frame[i] = b[i] * x->wa[i];
    mfo_rfft(x->fft, x->frame, re, im);
    float e = 0.0f;
    for (int k = 0; k < x->K; k++) {
        float m2 = re[k] * re[k] + im[k] * im[k];
        e += m2;
        lg[k] = 0.5f * mfo_log2f(m2 + 1e-24f);
    }
    return e;
}

/* PITCH: move the modulator's partials onto the harmonics of f0 */
static void retune(mfo_xsyn_t *x, float f0)
{
    int K = x->K, np = 0;
    float bin_hz = x->sr / x->N;
    memset(x->pre, 0, sizeof(float) * (size_t)K);
    memset(x->pim, 0, sizeof(float) * (size_t)K);
    float mx = -200.0f;
    for (int k = 1; k < K - 1; k++) if (x->mlog[k] > mx) mx = x->mlog[k];
    float floor_ = mx - 12.0f;                       /* -72 dB */
    for (int k = 2; k < K - 2; k++)
        if (x->mlog[k] > floor_ && x->mlog[k] > x->mlog[k - 1] && x->mlog[k] >= x->mlog[k + 1]) x->peaks[np++] = k;
    while (f0 < bin_hz) f0 *= 2.0f;
    float adv = MFO_TWO_PI * x->M / x->sr;
    for (int i = 0; i < np; i++) {
        int p = x->peaks[i];
        float a = x->mlog[p - 1], b = x->mlog[p], c = x->mlog[p + 1];
        float den = a - 2.0f * b + c;
        float d = den < -1e-9f ? 0.5f * (a - c) / den : 0.0f;
        float fp = (p + d) * bin_hz;
        int h = (int)(fp / f0 + 0.5f);
        if (h < 1) h = 1;
        if (h >= x->hmax) continue;
        float ft = h * f0;
        if (ft > 0.48f * x->sr) continue;
        int kt = (int)(ft / bin_hz + 0.5f), shift = kt - p;
        if (x->hstamp[h] == x->frame_no - 1) x->hacc[h] += adv * ft;
        else if (x->hstamp[h] != x->frame_no) x->hacc[h] = atan2f(x->mim[p], x->mre[p]);
        if (x->hstamp[h] != x->frame_no) {
            x->hacc[h] = fmodf(x->hacc[h], MFO_TWO_PI);
            x->hstamp[h] = x->frame_no;
        }
        float cr = cosf(x->hacc[h]), ci = sinf(x->hacc[h]);
        float pr = x->mre[p], pi = x->mim[p];
        float pm = sqrtf(pr * pr + pi * pi) + 1e-30f;
        pr /= pm; pi /= pm;
        int lo = p - 2, hi = p + 2;
        if (i > 0) { int mid = (x->peaks[i - 1] + p) / 2 + 1; if (lo < mid) lo = mid; }
        if (i + 1 < np) { int mid = (p + x->peaks[i + 1]) / 2; if (hi > mid) hi = mid; }
        for (int q = lo; q <= hi; q++) {
            int qt = q + shift;
            if (qt < 1 || qt >= K - 1) continue;
            /* M[q] * conj(unit M[p]) * e^{i acc} */
            float rr = x->mre[q] * pr + x->mim[q] * pi, ri = x->mim[q] * pr - x->mre[q] * pi;
            x->pre[qt] += rr * cr - ri * ci;
            x->pim[qt] += rr * ci + ri * cr;
        }
    }
    for (int k = 0; k < K; k++) x->plog[k] = 0.5f * mfo_log2f(x->pre[k] * x->pre[k] + x->pim[k] * x->pim[k] + 1e-24f);
}

static void unit_blend(float *ur, float *ui, float vr, float vi, float a)
{
    float r = (1.0f - a) * *ur + a * vr, i = (1.0f - a) * *ui + a * vi;
    float m = r * r + i * i;
    if (m < 1e-12f) { *ur = vr; *ui = vi; return; }
    m = 1.0f / sqrtf(m);
    *ur = r * m; *ui = i * m;
}

static void do_frame(mfo_xsyn_t *x, const mfo_xsyn_params_t *p, int frame_off)
{
    int N = x->N, K = x->K, M = x->M;
    float thresh = 4e-9f * (float)N * (float)N;
    x->em = analyse(x, 2, frame_off, x->mre, x->mim, x->mlog);
    float act = x->em / (x->em + thresh);
    float at = mfo_clampf(p->tone, 0, 1) * act, ag = mfo_clampf(p->grain, 0, 1) * act;
    float ap = p->f0 > 0.0f ? mfo_clampf(p->pitch, 0, 1) * act : 0.0f;
    int need_env = at > 1e-6f || ag > 1e-6f || ap > 1e-6f;
    if (need_env) smooth_env(x, x->mlog, x->menv);
    if (ap > 1e-6f) retune(x, p->f0);
    for (int c = 0; c < 2; c++) {
        float *sr = x->sre[c], *si = x->sim[c], *sl = x->slog[c], *se = x->senv[c];
        x->es[c] = analyse(x, c, frame_off, sr, si, sl);
        if (need_env) {
            smooth_env(x, sl, se);
            float gl = 0.5f * mfo_log2f((x->es[c] + 1e-20f) / (x->em + 1e-20f));
            for (int k = 0; k < K; k++) {
                float L = sl[k], Ecur = se[k];
                if (at > 1e-6f) { float dlt = at * (x->menv[k] + gl - se[k]); L += dlt; Ecur += dlt; }
                if (ag <= 1e-6f && ap <= 1e-6f) {
                    float g = mfo_exp2f(L - sl[k]);
                    x->tre[k] = sr[k] * g; x->tim[k] = si[k] * g;
                    continue;
                }
                float inv = mfo_exp2f(-sl[k]);
                float ur = sr[k] * inv, ui = si[k] * inv;
                if (ag > 1e-6f) {
                    float Lg = Ecur + (x->mlog[k] - x->menv[k]);
                    L += ag * (Lg - L);
                    float mi = mfo_exp2f(-x->mlog[k]);
                    unit_blend(&ur, &ui, x->mre[k] * mi, x->mim[k] * mi, ag);
                }
                if (ap > 1e-6f) {
                    float Lp = Ecur + (x->plog[k] - x->menv[k]);
                    L += ap * (Lp - L);
                    float pi_ = mfo_exp2f(-x->plog[k]);
                    unit_blend(&ur, &ui, x->pre[k] * pi_, x->pim[k] * pi_, ap);
                }
                float mag = mfo_exp2f(L);
                x->tre[k] = mag * ur; x->tim[k] = mag * ui;
            }
            x->tre[0] = sr[0]; x->tim[0] = 0.0f;       /* DC passes unmodified (no offsets from the modulator) */
            x->tim[K - 1] = 0.0f;
            mfo_irfft(x->fft, x->tre, x->tim, x->frame);
        } else {
            mfo_irfft(x->fft, sr, si, x->frame);
        }
        float sc = 1.0f / N;
        float *o = x->ola[c];
        const float *y = x->frame + N - 2 * M;
        for (int i = 0; i < 2 * M; i++) o[i] += y[i] * x->ws[i] * sc;
    }
    x->frame_no++;
}

static void finish_hop(mfo_xsyn_t *x)
{
    int M = x->M;
    for (int c = 0; c < 2; c++) {
        memcpy(x->fifo[c], x->ola[c], sizeof(float) * (size_t)M);
        memmove(x->ola[c], x->ola[c] + M, sizeof(float) * (size_t)M);
        memset(x->ola[c] + M, 0, sizeof(float) * (size_t)M);
    }
}

void mfo_xsyn_process(mfo_xsyn_t *x, const mfo_xsyn_params_t *p, const float *const *src, const float *mod,
                      float *const *out, int n)
{
    int N = x->N, M = x->M, w = N + x->fill;       /* write position in the N+M history */
    /* 1. input into the history (out may alias src) */
    memcpy(x->buf[0] + w, src[0], sizeof(float) * (size_t)n);
    memcpy(x->buf[1] + w, src[1], sizeof(float) * (size_t)n);
    if (mod) memcpy(x->buf[2] + w, mod, sizeof(float) * (size_t)n);
    else memset(x->buf[2] + w, 0, sizeof(float) * (size_t)n);
    /* 2. MOTION envelope of the modulator, now (it is applied 2M later, with the audio) */
    float gains[MFO_BLOCK];
    for (int i = 0; i < n; i++) {
        float v = fabsf(x->buf[2][w + i]);
        x->efast += (v - x->efast) * (v > x->efast ? x->ka : x->kr);
        x->epeak = x->efast > x->epeak ? x->efast : x->epeak * x->kpk;
        float g = x->efast / (x->epeak > 1e-5f ? x->epeak : 1e-5f);
        float old = x->gring[x->gpos];
        x->gring[x->gpos] = g > 1.0f ? 1.0f : g;
        x->gpos = x->gpos + 1 == x->glen ? 0 : x->gpos + 1;
        gains[i] = old;
    }
    /* 3. output: the STFT fifo, or the input delayed by 2M when nothing spectral is on */
    if (x->active) {
        for (int c = 0; c < 2; c++) memcpy(out[c], x->fifo[c] + x->rd, sizeof(float) * (size_t)n);
    } else {
        for (int c = 0; c < 2; c++) memcpy(out[c], x->buf[c] + w - 2 * M, sizeof(float) * (size_t)n);
    }
    x->rd += n;
    float m0 = x->motion_prev, m1 = mfo_clampf(p->motion, 0.0f, 1.0f);
    if (m0 > 0.0f || m1 > 0.0f)
        for (int i = 0; i < n; i++) {
            float m = m0 + (m1 - m0) * (float)(i + 1) / n;
            float g = 1.0f - m + m * gains[i];
            out[0][i] *= g; out[1][i] *= g;
        }
    x->motion_prev = m1;
    /* 4. a hop complete: frame */
    x->fill += n;
    if (x->fill >= M) {
        int want = p->force_stft || p->tone > 1e-6f || p->grain > 1e-6f || (p->pitch > 1e-6f && p->f0 > 0.0f);
        if (want) {
            if (!x->active) {                        /* prime the overlap-add with the previous frame */
                memset(x->ola[0], 0, sizeof(float) * 2 * (size_t)M);
                memset(x->ola[1], 0, sizeof(float) * 2 * (size_t)M);
                do_frame(x, p, 0);
                memmove(x->ola[0], x->ola[0] + M, sizeof(float) * (size_t)M); memset(x->ola[0] + M, 0, sizeof(float) * (size_t)M);
                memmove(x->ola[1], x->ola[1] + M, sizeof(float) * (size_t)M); memset(x->ola[1] + M, 0, sizeof(float) * (size_t)M);
            }
            do_frame(x, p, M);
            finish_hop(x);
            x->active = 1;
        } else {
            x->active = 0;
        }
        for (int c = 0; c < 3; c++) memmove(x->buf[c], x->buf[c] + M, sizeof(float) * (size_t)N);
        x->fill = 0; x->rd = 0;
    }
}
