/* SPDX-License-Identifier: AGPL-3.0-only
 * Vintage Chonk ORBIT: real FFT (power-of-two sizes), written for this project.
 * A real n-point transform runs as an n/2-point complex radix-2 FFT (split re/im arrays, per-stage
 * contiguous twiddles, radix-4 merged first stages) plus the usual even/odd post-processing. */
#include <stdlib.h>
#include "mfo_internal.h"

mfo_fft_t *mfo_fft_create(int n)
{
    if (n < 4 || (n & (n - 1))) return NULL;
    mfo_fft_t *f = calloc(1, sizeof *f);
    if (!f) return NULL;
    int m = n / 2;
    f->n = n; f->m = m;
    f->rev = malloc(sizeof(int) * (size_t)m);
    f->twr = malloc(sizeof(float) * (size_t)m);
    f->twi = malloc(sizeof(float) * (size_t)m);
    f->pr = malloc(sizeof(float) * (size_t)m);
    f->pi = malloc(sizeof(float) * (size_t)m);
    f->wr = malloc(sizeof(float) * (size_t)m);
    f->wi = malloc(sizeof(float) * (size_t)m);
    if (!f->rev || !f->twr || !f->twi || !f->pr || !f->pi || !f->wr || !f->wi) { mfo_fft_destroy(f); return NULL; }
    int bits = 0;
    while ((1 << bits) < m) bits++;
    for (int i = 0; i < m; i++) {
        int r = 0;
        for (int b = 0; b < bits; b++) if (i & (1 << b)) r |= 1 << (bits - 1 - b);
        f->rev[i] = r;
    }
    for (int h = 1; h < m; h <<= 1)               /* stage combining blocks of 2h, twiddles exp(-i pi j / h) */
        for (int j = 0; j < h; j++) {
            double a = -3.14159265358979323846 * j / h;
            f->twr[h - 1 + j] = (float)cos(a);
            f->twi[h - 1 + j] = (float)sin(a);
        }
    for (int k = 0; k < m; k++) {
        double a = -2.0 * 3.14159265358979323846 * k / n;
        f->pr[k] = (float)cos(a);
        f->pi[k] = (float)sin(a);
    }
    return f;
}

void mfo_fft_destroy(mfo_fft_t *f)
{
    if (!f) return;
    free(f->rev); free(f->twr); free(f->twi); free(f->pr); free(f->pi); free(f->wr); free(f->wi);
    free(f);
}

int mfo_fft_size(const mfo_fft_t *f) { return f->n; }

/* in-place forward complex FFT of size m on bit-reversed input */
static void cfft(const mfo_fft_t *f, float *restrict re, float *restrict im)
{
    int m = f->m;
    /* first two stages as radix-4 butterflies (m >= 4), else a plain radix-2 */
    int h0;
    if (m >= 4) {
        for (int i = 0; i < m; i += 4) {
            float a0r = re[i] + re[i + 1], a0i = im[i] + im[i + 1];
            float a1r = re[i] - re[i + 1], a1i = im[i] - im[i + 1];
            float a2r = re[i + 2] + re[i + 3], a2i = im[i + 2] + im[i + 3];
            float a3r = re[i + 2] - re[i + 3], a3i = im[i + 2] - im[i + 3];
            /* combine with twiddles 1 and -i */
            re[i] = a0r + a2r; im[i] = a0i + a2i;
            re[i + 2] = a0r - a2r; im[i + 2] = a0i - a2i;
            re[i + 1] = a1r + a3i; im[i + 1] = a1i - a3r;
            re[i + 3] = a1r - a3i; im[i + 3] = a1i + a3r;
        }
        h0 = 4;
    } else {
        for (int i = 0; i < m; i += 2) {
            float ar = re[i], ai = im[i];
            re[i] = ar + re[i + 1]; im[i] = ai + im[i + 1];
            re[i + 1] = ar - re[i + 1]; im[i + 1] = ai - im[i + 1];
        }
        h0 = 2;
    }
    for (int h = h0; h < m; h <<= 1) {
        const float *restrict wr = f->twr + h - 1, *restrict wi = f->twi + h - 1;
        for (int i = 0; i < m; i += 2 * h) {
            float *restrict ar = re + i, *restrict ai = im + i, *restrict br = re + i + h, *restrict bi = im + i + h;
            for (int j = 0; j < h; j++) {
                float tr = br[j] * wr[j] - bi[j] * wi[j];
                float ti = br[j] * wi[j] + bi[j] * wr[j];
                br[j] = ar[j] - tr; bi[j] = ai[j] - ti;
                ar[j] += tr; ai[j] += ti;
            }
        }
    }
}

void mfo_rfft(mfo_fft_t *f, const float *in, float *re, float *im)
{
    int m = f->m;
    float *zr = f->wr, *zi = f->wi;
    for (int i = 0; i < m; i++) {
        int r = f->rev[i];
        zr[r] = in[2 * i];
        zi[r] = in[2 * i + 1];
    }
    cfft(f, zr, zi);
    /* X[k] = Fe + W^k Fo,  Fe = (Z[k] + conj Z[m-k]) / 2,  Fo = (Z[k] - conj Z[m-k]) / 2i */
    re[0] = zr[0] + zi[0]; im[0] = 0.0f;
    re[m] = zr[0] - zi[0]; im[m] = 0.0f;
    for (int k = 1; k < m; k++) {
        float ar = zr[k], ai = zi[k], br = zr[m - k], bi = -zi[m - k];   /* b = conj Z[m-k] */
        float er = 0.5f * (ar + br), ei = 0.5f * (ai + bi);
        float dr = ar - br, di = ai - bi;                               /* Fo = (dr + i di) / 2i = (di - i dr) / 2 */
        float or_ = 0.5f * di, oi = -0.5f * dr;
        float wr = f->pr[k], wi = f->pi[k];
        re[k] = er + or_ * wr - oi * wi;
        im[k] = ei + or_ * wi + oi * wr;
    }
}

void mfo_irfft(mfo_fft_t *f, const float *re, const float *im, float *out)
{
    int m = f->m;
    float *zr = f->wr, *zi = f->wi;
    /* 2Z[k] = (X[k] + conj X[m-k]) + i (X[k] - conj X[m-k]) / W^k ; inverse = conj(FFT(conj)) */
    for (int k = 0; k < m; k++) {
        float ar = re[k], ai = im[k], br = re[m - k], bi = -im[m - k];
        float er = ar + br, ei = ai + bi;
        float dr = ar - br, di = ai - bi;
        float wr = f->pr[k], wi = -f->pi[k];                   /* 1/W^k = conj W^k */
        float qr = dr * wr - di * wi, qi = dr * wi + di * wr;
        float zr_ = er - qi, zi_ = ei + qr;                     /* e + i q */
        int r = f->rev[k];
        zr[r] = zr_;
        zi[r] = -zi_;                                           /* conjugate for the inverse */
    }
    cfft(f, zr, zi);
    for (int i = 0; i < m; i++) {
        out[2 * i] = zr[i];
        out[2 * i + 1] = -zi[i];
    }
}
