/* SPDX-License-Identifier: AGPL-3.0-only
 * Vintage Chonk ORBIT: procedural impulses, generated here from first principles (no recordings, no
 * outside content): spring tank, plate, wood box, bowed metal, tape hiss burst, formant voice, metal
 * hit, glass. Each is written peak-normalised to 0.9. Offline (allocates). */
#include <stdlib.h>
#include "mfo_internal.h"

static const char *gen_names[MFO_GEN_COUNT] = {
    "SPRING TANK", "PLATE", "WOOD BOX", "BOWED METAL", "TAPE HISS", "FORMANT VOX", "METAL HIT", "GLASS"
};
const char *mfo_generate_name(int t) { return t >= 0 && t < MFO_GEN_COUNT ? gen_names[t] : "?"; }

static float urand(uint32_t *s) { return 0.5f * (mfo_randf(s) + 1.0f); }

/* a decaying sinusoidal mode with an optional attack ramp, added into out */
static void add_mode(float *out, int len, float sr, double f, double amp, double decay_s, double phase, double attack_s)
{
    if (f <= 0.0 || f >= 0.49 * sr) return;
    double w = 2.0 * 3.14159265358979323846 * f / sr, r = exp(-6.907755 / (decay_s * sr));   /* -60 dB at decay_s */
    double cr = cos(w) * r, ci = sin(w) * r, zr = cos(phase), zi = sin(phase);
    int at = (int)(attack_s * sr);
    for (int i = 0; i < len; i++) {
        double a = at > 0 && i < at ? (double)i / at : 1.0;
        out[i] += (float)(amp * a * zi);
        double nr = zr * cr - zi * ci;
        zi = zr * ci + zi * cr;
        zr = nr;
        if (r < 1.0 && i > at && fabs(zr) + fabs(zi) < 1e-7) break;
    }
}

static void normalize_peak(float *x, int len, float peak)
{
    float m = 0.0f;
    for (int i = 0; i < len; i++) if (fabsf(x[i]) > m) m = fabsf(x[i]);
    if (m > 1e-12f) for (int i = 0; i < len; i++) x[i] *= peak / m;
}

static void gen_spring(float *out, int len, float sr, uint32_t *s)
{
    /* two springs: a delay loop with a cascade of dispersive all-passes (the chirps get longer every trip),
     * a loop low-pass and loss; the "boing" is the dispersion, not a sample */
    const int NAP = 80;
    float apz[2][80];
    float lp[2] = { 0, 0 };
    int D[2] = { (int)(0.0337f * sr), (int)(0.0461f * sr) };
    float g[2] = { 0.72f, 0.66f };
    float *line[2];
    for (int k = 0; k < 2; k++) { line[k] = calloc((size_t)D[k], sizeof(float)); memset(apz[k], 0, sizeof apz[k]); }
    if (!line[0] || !line[1]) { free(line[0]); free(line[1]); return; }
    int pos[2] = { 0, 0 };
    float a = 0.62f, klp = 1.0f - expf(-MFO_TWO_PI * 4200.0f / sr);
    for (int i = 0; i < len; i++) {
        float y = 0.0f;
        for (int k = 0; k < 2; k++) {
            float x = line[k][pos[k]];
            for (int j = 0; j < NAP; j++) {           /* first-order all-pass: y = -a x + z; z = x + a y */
                float v = -a * x + apz[k][j];
                apz[k][j] = x + a * v;
                x = v;
            }
            lp[k] += (x - lp[k]) * klp;
            float in = (i == 0 ? 1.0f : 0.0f) + 0.0015f * mfo_randf(s) * (i < (int)(0.01f * sr));
            line[k][pos[k]] = in + g[k] * lp[k];
            pos[k] = pos[k] + 1 == D[k] ? 0 : pos[k] + 1;
            y += lp[k] * (k ? -0.8f : 1.0f);
        }
        out[i] = y;
    }
    free(line[0]); free(line[1]);
    /* tank damping over the whole tail */
    for (int i = 0; i < len; i++) out[i] *= expf(-1.2f * i / sr);
}

static void gen_plate(float *out, int len, float sr, uint32_t *s)
{
    float l1 = 0, l2 = 0, k1 = 1.0f - expf(-MFO_TWO_PI * 700.0f / sr), k2 = 1.0f - expf(-MFO_TWO_PI * 4500.0f / sr);
    for (int i = 0; i < len; i++) {
        float t = i / sr, n = mfo_randf(s);
        l1 += (n - l1) * k1;
        l2 += (n - l2) * k2;
        float lo = l1, mid = l2 - l1, hi = n - l2;
        float v = lo * expf(-6.9f * t / 3.6f) * 1.6f + mid * expf(-6.9f * t / 2.6f) + hi * expf(-6.9f * t / 1.1f) * 0.6f;
        float att = t < 0.004f ? t / 0.004f : 1.0f;
        out[i] = v * att;
    }
    /* a sprinkle of metallic plate modes */
    for (int m = 0; m < 120; m++) {
        double f = 180.0 + 9000.0 * pow(urand(s), 1.6);
        add_mode(out, len, sr, f, 0.012 * (0.5 + urand(s)), 1.0 + 2.5 * urand(s) * (1.0 - f / 12000.0), 6.283 * urand(s), 0.0);
    }
}

static void gen_wood(float *out, int len, float sr, uint32_t *s)
{
    static const double fr[] = { 118, 196, 243, 310, 377, 455, 520, 610, 702, 815, 930, 1080, 1240, 1420, 1650, 1900, 2300, 2800 };
    for (unsigned m = 0; m < sizeof fr / sizeof fr[0]; m++) {
        double f = fr[m] * (1.0 + 0.03 * mfo_randf(s));
        add_mode(out, len, sr, f, (m < 4 ? 0.5 : 0.3) / (1.0 + m * 0.15), 0.03 + 0.16 / (1.0 + m * 0.35), 6.283 * urand(s), 0.0);
    }
    int kn = (int)(0.003f * sr);
    float lp = 0.0f, k = 1.0f - expf(-MFO_TWO_PI * 3000.0f / sr);
    for (int i = 0; i < kn && i < len; i++) { lp += (mfo_randf(s) - lp) * k; out[i] += 0.6f * lp * (1.0f - (float)i / kn); }
}

static void gen_bowed(float *out, int len, float sr, uint32_t *s)
{
    static const double ratio[] = { 1.0, 2.756, 5.404, 8.933, 13.34, 18.64 };
    double f1 = 201.0;                                         /* between G3 and G#3: no note sits on it exactly */
    float T = len / sr;
    for (unsigned m = 0; m < sizeof ratio / sizeof ratio[0]; m++)
        for (int d = 0; d < 2; d++) {                         /* detuned pairs beat slowly */
            double f = f1 * ratio[m] * (1.0 + (d ? 0.0021 : -0.0017) * (1 + m));
            add_mode(out, len, sr, f, 0.35 / (1.0 + m * 0.6), (T * 0.5 < 2.2 ? T * 0.5 : 2.2) / (1.0 + 0.15 * m), 6.283 * urand(s), 0.0);
        }
    for (int m = 0; m < 24; m++)                              /* a cloud of weaker inharmonic partials */
        add_mode(out, len, sr, f1 * (1.5 + 30.0 * urand(s)), 0.05 * urand(s), 0.6 + 1.2 * urand(s), 6.283 * urand(s), 0.0);
    /* the bow: rough noise through the partials, swelling then fading */
    float n1 = 0, k = 1.0f - expf(-MFO_TWO_PI * 25.0f / sr);
    for (int i = 0; i < len; i++) {
        float t = i / sr;
        float sw = t < 0.7f ? t / 0.7f : expf(-(t - 0.7f) * 1.1f);
        n1 += (mfo_randf(s) - n1) * k;
        float rough = 1.0f + 6.0f * n1;
        out[i] *= sw * rough;
    }
}

static void gen_hiss(float *out, int len, float sr, uint32_t *s)
{
    float b0 = 0, b1 = 0, b2 = 0, hp = 0, lp = 0;
    float khp = 1.0f - expf(-MFO_TWO_PI * 180.0f / sr), klp = 1.0f - expf(-MFO_TWO_PI * 9000.0f / sr);
    for (int i = 0; i < len; i++) {
        float t = i / sr, w = mfo_randf(s);
        b0 = 0.99765f * b0 + w * 0.0990460f;             /* pinkish (Kellet-style three poles) */
        b1 = 0.96300f * b1 + w * 0.2965164f;
        b2 = 0.57000f * b2 + w * 1.0526913f;
        float p = b0 + b1 + b2 + w * 0.1848f;
        lp += (p - lp) * klp;
        hp += (lp - hp) * khp;
        float v = lp - hp;
        float env = (t < 0.004f ? t / 0.004f : 1.0f) * expf(-t / 0.45f) * (1.0f + 0.25f * sinf(MFO_TWO_PI * 7.0f * t));
        out[i] = v * env;
    }
}

/* a two-pole resonator (formant) */
typedef struct { float y1, y2; } reso_t;
static float reso(reso_t *r, float x, float f, float bw, float sr)
{
    float rr = expf(-MFO_PI * bw / sr), c = 2.0f * rr * cosf(MFO_TWO_PI * f / sr);
    float y = (1.0f - rr) * x + c * r->y1 - rr * rr * r->y2;
    r->y2 = r->y1; r->y1 = y;
    return y;
}

static void gen_vox(float *out, int len, float sr, uint32_t *s)
{
    /* glottal pulse train (jittered, ~140 Hz) through four formants morphing a -> o -> e -> a */
    static const float V[4][4] = { { 730, 1090, 2440, 3400 }, { 570, 840, 2410, 3300 }, { 300, 2250, 3000, 3700 }, { 730, 1090, 2440, 3400 } };
    static const float BW[4] = { 80, 90, 120, 150 };
    reso_t r[4];
    memset(r, 0, sizeof r);
    double ph = 0.0;
    float T = len / sr, prev = 0.0f;
    for (int i = 0; i < len; i++) {
        float t = i / sr;
        float u = t / (T > 0 ? T : 1) * 3.0f;
        int a = (int)u; if (a > 2) a = 2;
        float f = u - a;
        f = f * f * (3.0f - 2.0f * f);
        double f0 = 140.0 * (1.0 + 0.04 * sin(6.2831853 * 5.2 * t)) * (1.0 + 0.004 * mfo_randf(s));
        ph += f0 / sr;
        float g = 0.0f;
        if (ph >= 1.0) { ph -= 1.0; g = 1.0f; }
        float src = g - prev * 0.0f + 0.02f * mfo_randf(s);   /* pulse + breath */
        prev = g;
        float y = 0.0f;
        for (int k = 0; k < 4; k++) y += reso(&r[k], src, V[a][k] + (V[a + 1][k] - V[a][k]) * f, BW[k], sr) * (k == 0 ? 1.0f : 0.7f / k);
        float env = (t < 0.08f ? t / 0.08f : 1.0f) * (t > T - 0.3f ? (T - t) / 0.3f : 1.0f);
        out[i] = y * env;
    }
}

static void gen_metal_hit(float *out, int len, float sr, uint32_t *s)
{
    for (int m = 0; m < 40; m++) {
        double f = 220.0 * pow(1.0 + m, 1.45) * (1.0 + 0.05 * mfo_randf(s));
        add_mode(out, len, sr, f, 0.4 / (1.0 + 0.3 * m), 2.2 / (1.0 + 0.08 * m), 6.283 * urand(s), 0.0);
    }
    int kn = (int)(0.002f * sr);
    for (int i = 0; i < kn && i < len; i++) out[i] += 0.8f * mfo_randf(s) * (1.0f - (float)i / kn);
}

static void gen_glass(float *out, int len, float sr, uint32_t *s)
{
    static const double ratio[] = { 1.0, 2.32, 4.25, 6.63, 9.38 };
    for (unsigned m = 0; m < sizeof ratio / sizeof ratio[0]; m++)
        for (int d = 0; d < 2; d++)
            add_mode(out, len, sr, 880.0 * ratio[m] * (1.0 + (d ? 0.0008 : -0.0008)), 0.3 / (1.0 + m), 3.5 / (1.0 + 0.5 * m), 6.283 * urand(s), 0.001);
}

void mfo_generate_impulse(int type, float *out, int len, float sr, uint32_t seed)
{
    if (!out || len < 1) return;
    memset(out, 0, sizeof(float) * (size_t)len);
    uint32_t s = seed ? seed : 1u;
    switch (type) {
    case MFO_GEN_SPRING: gen_spring(out, len, sr, &s); break;
    case MFO_GEN_PLATE: gen_plate(out, len, sr, &s); break;
    case MFO_GEN_WOOD_BOX: gen_wood(out, len, sr, &s); break;
    case MFO_GEN_BOWED_METAL: gen_bowed(out, len, sr, &s); break;
    case MFO_GEN_TAPE_HISS: gen_hiss(out, len, sr, &s); break;
    case MFO_GEN_FORMANT_VOX: gen_vox(out, len, sr, &s); break;
    case MFO_GEN_METAL_HIT: gen_metal_hit(out, len, sr, &s); break;
    case MFO_GEN_GLASS: gen_glass(out, len, sr, &s); break;
    default: return;
    }
    /* 5 Hz high-pass: no DC in any impulse */
    float x1 = 0.0f, y1 = 0.0f, R = 1.0f - MFO_TWO_PI * 5.0f / sr;
    for (int i = 0; i < len; i++) { float y = out[i] - x1 + R * y1; x1 = out[i]; y1 = y; out[i] = y; }
    normalize_peak(out, len, 0.9f);
}
