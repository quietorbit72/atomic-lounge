/* SPDX-License-Identifier: AGPL-3.0-only
 * Vintage Chonk ORBIT: zero-latency partitioned convolution (BODY).
 *
 * Taps [0, 64) run in direct form (sample accurate, no delay). The rest is covered by uniform-partitioned
 * overlap-save frequency-domain delay lines at three sizes: N = 64 for taps [64, 512), N = 512 for
 * [512, 4096) and N = 4096 for [4096, end). A level of size N starts at offset N, so the newest input
 * block it needs is complete exactly when its output block starts (no added latency). The multiply-adds
 * with older input spectra are spread over the 64-sample ticks of the previous period, so the work at a
 * period boundary is one forward FFT, one partition and one inverse FFT per channel. */
#include <stdlib.h>
#include "mfo_internal.h"

#define CV_B MFO_BLOCK
#define CV_NLEV 3
static const int lev_n[CV_NLEV] = { 64, 512, 4096 };
static int lev_start(int l) { return lev_n[l]; }
static int lev_end(int l, int len) { return l + 1 < CV_NLEV ? (lev_n[l + 1] < len ? lev_n[l + 1] : len) : len; }
static int lev_parts(int l, int len)
{
    int s = lev_start(l), e = lev_end(l, len);
    return e > s ? (e - s + lev_n[l] - 1) / lev_n[l] : 0;
}

struct mfo_kernel {
    int channels, length;
    float headr[2][CV_B];            /* direct taps, reversed: headr[c][B-1-m] = h[m] */
    int parts[CV_NLEV];
    float *re[CV_NLEV][2], *im[CV_NLEV][2];   /* parts * (N+1) bins each, scaled by 1/(2N) */
};

mfo_kernel_t *mfo_kernel_create(const float *const *ir, int channels, int len, float gain)
{
    if (channels < 1 || channels > 2 || len < 1) return NULL;
    mfo_kernel_t *k = calloc(1, sizeof *k);
    if (!k) return NULL;
    k->channels = channels; k->length = len;
    for (int c = 0; c < channels; c++)
        for (int m = 0; m < CV_B && m < len; m++) k->headr[c][CV_B - 1 - m] = ir[c][m] * gain;
    for (int l = 0; l < CV_NLEV; l++) {
        int P = lev_parts(l, len), N = lev_n[l];
        k->parts[l] = P;
        if (!P) continue;
        mfo_fft_t *f = mfo_fft_create(2 * N);
        float *buf = malloc(sizeof(float) * 2 * (size_t)N);
        if (!f || !buf) { mfo_fft_destroy(f); free(buf); mfo_kernel_destroy(k); return NULL; }
        float sc = gain / (2.0f * N);
        for (int c = 0; c < channels; c++) {
            k->re[l][c] = malloc(sizeof(float) * (size_t)P * (N + 1));
            k->im[l][c] = malloc(sizeof(float) * (size_t)P * (N + 1));
            if (!k->re[l][c] || !k->im[l][c]) { mfo_fft_destroy(f); free(buf); mfo_kernel_destroy(k); return NULL; }
            for (int j = 0; j < P; j++) {
                int off = lev_start(l) + j * N, end = lev_end(l, len);
                for (int i = 0; i < 2 * N; i++) {
                    int t = off + i;
                    buf[i] = (i < N && t < end) ? ir[c][t] * sc : 0.0f;
                }
                mfo_rfft(f, buf, k->re[l][c] + (size_t)j * (N + 1), k->im[l][c] + (size_t)j * (N + 1));
            }
        }
        mfo_fft_destroy(f);
        free(buf);
    }
    return k;
}

/* ---- kernels rebuilt in place on the audio thread (live INPUT impulse, 1.2.0) ---------------------- */
mfo_kernel_t *mfo_kernel_alloc(int channels, int max_len)
{
    if (channels < 1 || channels > 2 || max_len < 1) return NULL;
    mfo_kernel_t *k = calloc(1, sizeof *k);
    if (!k) return NULL;
    k->channels = channels; k->length = 0;
    for (int l = 0; l < CV_NLEV; l++) {
        int P = lev_parts(l, max_len), N = lev_n[l];
        k->parts[l] = 0;
        if (!P) continue;
        for (int c = 0; c < channels; c++) {
            k->re[l][c] = calloc((size_t)P * (N + 1), sizeof(float));
            k->im[l][c] = calloc((size_t)P * (N + 1), sizeof(float));
            if (!k->re[l][c] || !k->im[l][c]) { mfo_kernel_destroy(k); return NULL; }
        }
    }
    return k;
}

struct mfo_kload {
    int max_len;
    mfo_fft_t *fft[CV_NLEV];
    float *buf;                      /* 2 * 4096 */
    mfo_kernel_t *k;
    const float *ir;
    int len, level, part, done;
    float gain;
};

mfo_kload_t *mfo_kload_create(int max_len)
{
    mfo_kload_t *L = calloc(1, sizeof *L);
    if (!L) return NULL;
    L->max_len = max_len;
    L->done = 1;
    int ok = 1;
    for (int l = 0; l < CV_NLEV; l++)
        if (lev_parts(l, max_len)) { L->fft[l] = mfo_fft_create(2 * lev_n[l]); ok = ok && L->fft[l]; }
    L->buf = calloc(2 * (size_t)lev_n[CV_NLEV - 1], sizeof(float));
    if (!ok || !L->buf) { mfo_kload_destroy(L); return NULL; }
    return L;
}

void mfo_kload_destroy(mfo_kload_t *L)
{
    if (!L) return;
    for (int l = 0; l < CV_NLEV; l++) mfo_fft_destroy(L->fft[l]);
    free(L->buf);
    free(L);
}

void mfo_kload_begin(mfo_kload_t *L, mfo_kernel_t *k, const float *ir, int len, float gain)
{
    if (len > L->max_len) len = L->max_len;
    if (len < 1) len = 1;
    L->k = k; L->ir = ir; L->len = len; L->gain = gain;
    L->level = 0; L->part = 0; L->done = 0;
    memset(k->headr, 0, sizeof k->headr);
    for (int m = 0; m < CV_B && m < len; m++) k->headr[0][CV_B - 1 - m] = ir[m] * gain;
    k->channels = 1;
    k->length = len;
    for (int l = 0; l < CV_NLEV; l++) k->parts[l] = 0;   /* filled level by level; not usable until done */
}

int mfo_kload_step(mfo_kload_t *L, int max_parts)
{
    if (L->done) return 1;
    mfo_kernel_t *k = L->k;
    while (max_parts > 0 && L->level < CV_NLEV) {
        int l = L->level, P = lev_parts(l, L->len), N = lev_n[l];
        if (L->part >= P) { k->parts[l] = P; L->level++; L->part = 0; continue; }
        float sc = L->gain / (2.0f * N);
        int off = lev_start(l) + L->part * N, end = lev_end(l, L->len);
        for (int i = 0; i < 2 * N; i++) {
            int t = off + i;
            L->buf[i] = (i < N && t < end) ? L->ir[t] * sc : 0.0f;
        }
        mfo_rfft(L->fft[l], L->buf, k->re[l][0] + (size_t)L->part * (N + 1), k->im[l][0] + (size_t)L->part * (N + 1));
        L->part++;
        max_parts--;
    }
    if (L->level >= CV_NLEV) L->done = 1;
    return L->done;
}

void mfo_kernel_destroy(mfo_kernel_t *k)
{
    if (!k) return;
    for (int l = 0; l < CV_NLEV; l++)
        for (int c = 0; c < 2; c++) { free(k->re[l][c]); free(k->im[l][c]); }
    free(k);
}
int mfo_kernel_length(const mfo_kernel_t *k) { return k ? k->length : 0; }
int mfo_kernel_channels(const mfo_kernel_t *k) { return k ? k->channels : 0; }

typedef struct {
    int N, cap;                      /* block size, FDL capacity (partitions) */
    mfo_fft_t *fft;
    float *in[2];                    /* 2N: previous block | current block */
    float *fre[2], *fim[2];          /* FDL: cap * (N+1) */
    int head;                        /* slot of the newest spectrum */
    float *are[2], *aim[2];          /* accumulators per output channel, N+1 */
    float *out[2];                   /* current output block per output channel */
    float *tmp;                      /* 2N */
    float *xre, *xim;                /* N+1 scratch */
} cv_level_t;

struct mfo_conv {
    int inch;
    int64_t t, t0;
    float hx[2][2 * CV_B];           /* direct form history: [B-1 old | chunk] */
    cv_level_t lev[CV_NLEV];
};

mfo_conv_t *mfo_conv_create(int in_channels, int max_len)
{
    if (in_channels < 1 || in_channels > 2) return NULL;
    mfo_conv_t *c = calloc(1, sizeof *c);
    if (!c) return NULL;
    c->inch = in_channels;
    for (int l = 0; l < CV_NLEV; l++) {
        cv_level_t *L = &c->lev[l];
        int N = lev_n[l];
        L->N = N;
        L->cap = lev_parts(l, max_len);
        if (!L->cap) continue;
        size_t bins = (size_t)N + 1;
        L->fft = mfo_fft_create(2 * N);
        L->tmp = calloc(2 * (size_t)N, sizeof(float));
        L->xre = calloc(bins, sizeof(float));
        L->xim = calloc(bins, sizeof(float));
        int ok = L->fft && L->tmp && L->xre && L->xim;
        for (int ch = 0; ch < 2; ch++) {
            if (ch < in_channels) {
                L->in[ch] = calloc(2 * (size_t)N, sizeof(float));
                L->fre[ch] = calloc((size_t)L->cap * bins, sizeof(float));
                L->fim[ch] = calloc((size_t)L->cap * bins, sizeof(float));
                ok = ok && L->in[ch] && L->fre[ch] && L->fim[ch];
            }
            L->are[ch] = calloc(bins, sizeof(float));
            L->aim[ch] = calloc(bins, sizeof(float));
            L->out[ch] = calloc((size_t)N, sizeof(float));
            ok = ok && L->are[ch] && L->aim[ch] && L->out[ch];
        }
        if (!ok) { mfo_conv_destroy(c); return NULL; }
    }
    return c;
}

void mfo_conv_destroy(mfo_conv_t *c)
{
    if (!c) return;
    for (int l = 0; l < CV_NLEV; l++) {
        cv_level_t *L = &c->lev[l];
        mfo_fft_destroy(L->fft);
        free(L->tmp); free(L->xre); free(L->xim);
        for (int ch = 0; ch < 2; ch++) {
            free(L->in[ch]); free(L->fre[ch]); free(L->fim[ch]);
            free(L->are[ch]); free(L->aim[ch]); free(L->out[ch]);
        }
    }
    free(c);
}

void mfo_conv_set_stagger(mfo_conv_t *c, int offset)
{
    c->t0 = offset > 0 ? (int64_t)(offset / CV_B) * CV_B : 0;
    mfo_conv_reset(c);
}

void mfo_conv_reset(mfo_conv_t *c)
{
    c->t = c->t0;
    memset(c->hx, 0, sizeof c->hx);
    for (int l = 0; l < CV_NLEV; l++) {
        cv_level_t *L = &c->lev[l];
        if (!L->cap) continue;
        size_t bins = (size_t)L->N + 1;
        for (int ch = 0; ch < 2; ch++) {
            if (L->in[ch]) {
                memset(L->in[ch], 0, sizeof(float) * 2 * (size_t)L->N);
                memset(L->fre[ch], 0, sizeof(float) * (size_t)L->cap * bins);
                memset(L->fim[ch], 0, sizeof(float) * (size_t)L->cap * bins);
            }
            memset(L->are[ch], 0, sizeof(float) * bins);
            memset(L->aim[ch], 0, sizeof(float) * bins);
            memset(L->out[ch], 0, sizeof(float) * (size_t)L->N);
        }
        L->head = 0;
    }
}

/* acc += x * h over nb bins (complex, split) */
static void cmac(float *restrict ar, float *restrict ai, const float *restrict xr, const float *restrict xi,
                 const float *restrict hr, const float *restrict hi, int nb)
{
    for (int i = 0; i < nb; i++) {
        ar[i] += xr[i] * hr[i] - xi[i] * hi[i];
        ai[i] += xr[i] * hi[i] + xi[i] * hr[i];
    }
}

static void level_tick(mfo_conv_t *c, cv_level_t *L, int l, const mfo_kernel_t *k)
{
    int N = L->N, nb = N + 1;
    int P = k ? k->parts[l] : 0;
    if (P > L->cap) P = L->cap;
    int kch = k ? k->channels : 1;
    int nout = (c->inch == 2 || kch == 2) ? 2 : 1;
    int64_t ph = c->t % N;
    if (ph == 0) {
        /* boundary: newest input block complete -> FDL, finish the output block */
        L->head = (L->head + L->cap - 1) % L->cap;
        for (int ch = 0; ch < c->inch; ch++) {
            mfo_rfft(L->fft, L->in[ch], L->fre[ch] + (size_t)L->head * nb, L->fim[ch] + (size_t)L->head * nb);
            memcpy(L->in[ch], L->in[ch] + N, sizeof(float) * (size_t)N);
        }
        for (int o = 0; o < 2; o++) {
            if (o >= nout || !P) { memset(L->out[o], 0, sizeof(float) * (size_t)N); continue; }
            int ic = c->inch == 2 ? o : 0, kc = kch == 2 ? o : 0;
            cmac(L->are[o], L->aim[o], L->fre[ic] + (size_t)L->head * nb, L->fim[ic] + (size_t)L->head * nb,
                 k->re[l][kc], k->im[l][kc], nb);
            mfo_irfft(L->fft, L->are[o], L->aim[o], L->tmp);
            memcpy(L->out[o], L->tmp + N, sizeof(float) * (size_t)N);
            memset(L->are[o], 0, sizeof(float) * (size_t)nb);
            memset(L->aim[o], 0, sizeof(float) * (size_t)nb);
        }
        if (nout == 1) memcpy(L->out[1], L->out[0], sizeof(float) * (size_t)N);
    }
    if (P > 1) {
        /* spread partitions 1..P-1 (input spectra FDL[j-1]) over the N/B ticks of this period */
        int T = N / CV_B, i = (int)(ph / CV_B);
        int j0 = 1 + (int)((int64_t)(P - 1) * i / T), j1 = 1 + (int)((int64_t)(P - 1) * (i + 1) / T);
        for (int o = 0; o < nout; o++) {
            int ic = c->inch == 2 ? o : 0, kc = kch == 2 ? o : 0;
            for (int j = j0; j < j1; j++) {
                size_t slot = (size_t)((L->head + j - 1) % L->cap) * nb;
                cmac(L->are[o], L->aim[o], L->fre[ic] + slot, L->fim[ic] + slot,
                     k->re[l][kc] + (size_t)j * nb, k->im[l][kc] + (size_t)j * nb, nb);
            }
        }
    }
}

static void conv_chunk(mfo_conv_t *c, const mfo_kernel_t *k, const float *const *in, float *const *out, int n)
{
    int kch = k ? k->channels : 1;
    int nout = (c->inch == 2 || kch == 2) ? 2 : 1;
    int want2 = out[1] != NULL;
    if (c->t % CV_B == 0)
        for (int l = 0; l < CV_NLEV; l++) if (c->lev[l].cap) level_tick(c, &c->lev[l], l, k);
    /* inputs first (out may alias in) */
    for (int ch = 0; ch < c->inch; ch++) memcpy(c->hx[ch] + CV_B - 1, in[ch], sizeof(float) * (size_t)n);
    for (int l = 0; l < CV_NLEV; l++) {
        cv_level_t *L = &c->lev[l];
        if (!L->cap) continue;
        int o0 = (int)(c->t % L->N);
        for (int ch = 0; ch < c->inch; ch++) memcpy(L->in[ch] + L->N + o0, in[ch], sizeof(float) * (size_t)n);
    }
    /* direct-form head */
    for (int o = 0; o < (want2 ? 2 : 1); o++) {
        float *y = out[o];
        if (!k) { memset(y, 0, sizeof(float) * (size_t)n); continue; }
        if (o >= nout) { memcpy(y, out[0], sizeof(float) * (size_t)n); continue; }
        const float *hr = k->headr[kch == 2 ? o : 0];
        const float *x = c->hx[c->inch == 2 ? o : 0];
        for (int i = 0; i < n; i++) {
            const float *xp = x + i;
            float s = 0.0f;
            for (int m = 0; m < CV_B; m++) s += hr[m] * xp[m];
            y[i] = s;
        }
    }
    for (int ch = 0; ch < c->inch; ch++) memmove(c->hx[ch], c->hx[ch] + n, sizeof(float) * (CV_B - 1));
    /* partitioned levels: output blocks added */
    for (int l = 0; l < CV_NLEV; l++) {
        cv_level_t *L = &c->lev[l];
        if (!L->cap) continue;
        int o0 = (int)(c->t % L->N);
        if (!k || !k->parts[l]) continue;
        for (int o = 0; o < (want2 ? 2 : 1); o++) {
            float *y = out[o];
            const float *b = L->out[o] + o0;
            for (int i = 0; i < n; i++) y[i] += b[i];
        }
    }
    c->t += n;
}

void mfo_conv_process(mfo_conv_t *c, const mfo_kernel_t *k, const float *const *in, float *const *out, int n)
{
    int done = 0;
    while (done < n) {
        int room = CV_B - (int)(c->t % CV_B);
        int m = n - done < room ? n - done : room;
        const float *ip[2] = { in[0] + done, c->inch == 2 ? in[1] + done : NULL };
        float *op[2] = { out[0] + done, out[1] ? out[1] + done : NULL };
        conv_chunk(c, k, ip, op, m);
        done += m;
    }
}
