/* SPDX-License-Identifier: AGPL-3.0-only
 * Vintage Chonk ORBIT: offline helpers (not for the audio thread): synth prints and impulse shaping. */
#include <stdlib.h>
#include "mfo_internal.h"

int mfo_print_synth(const mfe_patch_t *p, int note, int vel, float hold, float seconds, float sr, float *out_l, float *out_r)
{
    if (!p || seconds <= 0.0f || sr <= 0.0f) return 0;
    if (!mfe_tables_ready()) mfe_init_tables();
    int total = (int)(seconds * sr);
    int in_len = (int)(seconds * MFE_SR) + 2 * MFE_BLOCK;
    int blocks = (in_len + MFE_BLOCK - 1) / MFE_BLOCK;
    in_len = blocks * MFE_BLOCK;
    mfe_engine_t *e = calloc(1, mfe_engine_size());
    float *l = malloc(sizeof(float) * (size_t)in_len), *r = malloc(sizeof(float) * (size_t)in_len);
    if (!e || !l || !r) { free(e); free(l); free(r); return 0; }
    mfe_engine_init(e);
    mfe_engine_set_patch(e, p);
    mfe_note_on(e, (uint8_t)(note & 127), (uint8_t)(vel < 1 ? 1 : vel > 127 ? 127 : vel));
    int off_block = (int)(hold * MFE_SR) / MFE_BLOCK;
    int32_t blk[MFE_BLOCK * 2];
    for (int b = 0; b < blocks; b++) {
        if (b == off_block) mfe_note_off(e, (uint8_t)(note & 127));
        mfe_render(e, blk);
        for (int i = 0; i < MFE_BLOCK; i++) {
            l[b * MFE_BLOCK + i] = blk[2 * i] * (1.0f / 32768.0f);
            r[b * MFE_BLOCK + i] = blk[2 * i + 1] * (1.0f / 32768.0f);
        }
    }
    free(e);
    if (!out_r) for (int i = 0; i < in_len; i++) l[i] = 0.5f * (l[i] + r[i]);
    int nl = 0, nr = 0;
    float *rl = mfo_resample_buffer(l, in_len, sr / MFE_SR, &nl);
    float *rr = out_r ? mfo_resample_buffer(r, in_len, sr / MFE_SR, &nr) : NULL;
    free(l); free(r);
    if (!rl || (out_r && !rr)) { free(rl); free(rr); return 0; }
    int n = total < nl ? total : nl;
    memcpy(out_l, rl, sizeof(float) * (size_t)n);
    if (out_r) memcpy(out_r, rr, sizeof(float) * (size_t)n);
    for (int i = n; i < total; i++) { out_l[i] = 0.0f; if (out_r) out_r[i] = 0.0f; }
    free(rl); free(rr);
    return total;
}

void mfo_ir_opts_default(mfo_ir_opts_t *o)
{
    memset(o, 0, sizeof *o);
    o->fade_out = 0.01f;
    o->normalize = 1;
}

int mfo_ir_prepare(const mfo_buffer_t *src, float target_rate, const mfo_ir_opts_t *o, float *out_data[2])
{
    out_data[0] = out_data[1] = NULL;
    if (!src || src->length < 1 || !src->data[0] || target_rate <= 0.0f) return 0;
    mfo_ir_opts_t d;
    if (!o) { mfo_ir_opts_default(&d); o = &d; }
    int nch = src->channels > 1 ? 2 : 1;
    int s0 = (int)(o->start * src->sample_rate);
    if (s0 < 0) s0 = 0;
    if (s0 >= src->length) s0 = src->length - 1;
    int seg = o->length > 0.0f ? (int)(o->length * src->sample_rate) : src->length - s0;
    if (seg > src->length - s0) seg = src->length - s0;
    if (seg < 1) seg = 1;
    double ratio = (target_rate / src->sample_rate) / exp2(o->pitch / 12.0);
    int cap = (int)(6.0f * target_rate);
    int len = 0;
    for (int c = 0; c < nch; c++) {
        float *tmp = malloc(sizeof(float) * (size_t)seg);
        if (!tmp) return 0;
        for (int i = 0; i < seg; i++) tmp[i] = src->data[c][s0 + (o->reverse ? seg - 1 - i : i)];
        int n = 0;
        out_data[c] = fabs(ratio - 1.0) < 1e-9 ? tmp : mfo_resample_buffer(tmp, seg, ratio, &n);
        if (out_data[c] != tmp) free(tmp); else n = seg;
        if (!out_data[c]) { free(out_data[0]); free(out_data[1]); out_data[0] = out_data[1] = NULL; return 0; }
        len = n;
    }
    if (len > cap) len = cap;
    float T = (float)len / target_rate;
    float tilt = mfo_clampf(o->decay_tilt, -1.0f, 1.0f), damp = mfo_clampf(o->damp, 0.0f, 1.0f), age = mfo_clampf(o->age, 0.0f, 1.0f);
    for (int c = 0; c < nch; c++) {
        float *x = out_data[c];
        /* age: slow wow by re-reading with a varying rate (linear interpolation) */
        if (age > 0.0f) {
            float *y = malloc(sizeof(float) * (size_t)len);
            if (y) {
                double p = 0.0;
                for (int i = 0; i < len; i++) {
                    int k = (int)p;
                    float f = (float)(p - k);
                    y[i] = k + 1 < len ? x[k] + (x[k + 1] - x[k]) * f : 0.0f;
                    p += 1.0 + 0.004 * age * sin(6.2831853 * 0.7 * i / target_rate) + 0.0015 * age * sin(6.2831853 * 5.1 * i / target_rate);
                    if (p < 0) p = 0;
                }
                memcpy(x, y, sizeof(float) * (size_t)len);
                free(y);
            }
        }
        float lp = 0.0f, hp = 0.0f, lp2 = 0.0f;
        float khp = 1.0f - expf(-MFO_TWO_PI * (20.0f + 160.0f * age) / target_rate);
        float kage = 1.0f - expf(-MFO_TWO_PI * (18000.0f - 14000.0f * age) / target_rate);
        int fi = (int)(o->fade_in * target_rate), fo = (int)(o->fade_out * target_rate);
        for (int i = 0; i < len; i++) {
            float t = (float)i / target_rate;
            float v = x[i];
            if (tilt != 0.0f) v *= expf(-tilt * 4.0f * t / (T > 1e-3f ? T : 1e-3f));
            if (damp > 0.0f) {
                float fc = 18000.0f * exp2f(-damp * 8.0f * t / (T > 1e-3f ? T : 1e-3f));
                if (fc < 150.0f) fc = 150.0f;
                lp += (v - lp) * (1.0f - expf(-MFO_TWO_PI * fc / target_rate));
                v = lp;
            }
            if (age > 0.0f) {
                lp2 += (v - lp2) * kage;
                hp += (lp2 - hp) * khp;
                v = lp2 - hp;
            }
            if (fi > 0 && i < fi) v *= (float)i / fi;
            if (fo > 0 && i >= len - fo) v *= (float)(len - 1 - i) / fo;
            x[i] = v;
        }
    }
    if (o->normalize) {
        double en = 0.0;
        for (int c = 0; c < nch; c++) for (int i = 0; i < len; i++) en += (double)out_data[c][i] * out_data[c][i];
        en /= nch;
        if (en > 1e-20) {
            float g = (float)(1.0 / sqrt(en));
            for (int c = 0; c < nch; c++) for (int i = 0; i < len; i++) out_data[c][i] *= g;
        }
    }
    return len;
}
