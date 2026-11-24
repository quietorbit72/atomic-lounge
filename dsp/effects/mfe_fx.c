/* SPDX-License-Identifier: AGPL-3.0-only
 * Vintage Chonk Engine: master effects on the stereo bus (preset v5, 17 Nov 2026):
 *
 *   OCT FUZZ -> TAPE -> ENSEMBLE -> BBD ECHO -> OIL CAN -> REVERB (TWIN TANK or SPRING)
 *
 * OCT FUZZ, OIL CAN and the two reverbs are the developer's Chroma Console effects, copied into chroma/ (source
 * the developer's chroma-mod project; each file names its origin and marks the MFE changes) and run here through
 * a thin adapter: 16-frame planar Q23 sub-blocks inside MFE's 128-frame interleaved Q15 block, knobs mapped
 * from the preset (docs/DSP_DESIGN.md, "Master effects"), times rescaled to 44.1 kHz (chroma/mod_common.h).
 * TAPE, ENSEMBLE and BBD ECHO are MFE's own. Integer audio path throughout (int64 products where needed).
 * Each effect is bypassed bit for bit, with no CPU, when its on-parameter is 0; its state is cleared when it
 * comes back on. Buffer sizes are build options (FM1 RAM budget, docs/FM1_FIRMWARE_BUILD_PLAN.md). */
#include <string.h>
#include "mfe_internal.h"

/* ---- build options (sizes in int16 words) ---- */
#ifndef MFE_DELAY_LEN
#define MFE_DELAY_LEN 22050          /* BBD ECHO line, mono at 22.05 kHz: 1.0 s (FM1 build: 16384 = 0.74 s) */
#endif
#ifndef MFE_OILCAN_RING
#define MFE_OILCAN_RING 32768        /* OIL CAN ring per channel (power of 2): 32768 = Chroma's full 40-400 ms
                                      * (synced up to 735 ms); the FM1 build uses 4096 with MFE_OILCAN_LITE:
                                      * one half-rate mono ring of 2 x 4096 words = 40-357 ms (chroma/oilcan.inc) */
#endif
#ifndef MFE_ENS_LEN
#define MFE_ENS_LEN 512              /* ENSEMBLE lines per channel (power of 2): 11.6 ms */
#endif
#ifndef MFE_TAPE_LEN
#define MFE_TAPE_LEN 256             /* TAPE wow lines per channel (power of 2): 5.8 ms */
#endif
#if (MFE_ENS_LEN & (MFE_ENS_LEN - 1)) || MFE_ENS_LEN < 512 || (MFE_TAPE_LEN & (MFE_TAPE_LEN - 1)) || MFE_TAPE_LEN < 256
#error "MFE_ENS_LEN (>= 512) and MFE_TAPE_LEN (>= 256) must be powers of two"
#endif
#if MFE_DELAY_LEN < 2048
#error "MFE_DELAY_LEN must be at least 2048"
#endif

/* ---- the developer's Chroma Console effects: .inc files compiled into this file only (no build entries; never build
 *      dsp/effects/chroma/*.inc on their own) ---- */
#define CHROMA_SR MFE_SR
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"      /* the shared spring core has a helper we do not call */
#include "chroma/mod_delays.h"
#include "chroma/oilcan.inc"
#include "chroma/103_blown_octave_velcro.inc"
#include "chroma/142_tx_twin_tank.inc"
#include "chroma/141_tx_self_osc_spring.inc"
#pragma GCC diagnostic pop
#if MFE_OILCAN_RING < 4096 || (MFE_OILCAN_RING & (MFE_OILCAN_RING - 1))
#error "MFE_OILCAN_RING must be a power of two >= 4096"
#endif

#define HZ(f) ((uint32_t)((f) * 97391.548 + 0.5))      /* phase increment per sample at 44.1 kHz (constants only) */
static inline uint16_t knob(int32_t q15) { return (uint16_t)(MFE_CLAMP(q15, 0, 32767) * 2); }

/* TPT one-pole low-pass, G = g / (1 + g) in Q15; x, state Q15 in int32 */
static inline int32_t lp1(int32_t *s, int32_t G, int32_t x)
{
    int32_t v = (int32_t)(((int64_t)(x - *s) * G) >> 15), y = v + *s;
    *s = y + v;
    return y;
}
/* G for a cutoff pitch (MIDI note << 8) at 44.1 kHz; + 12 << 8 for the 22.05 kHz lines */
static int32_t pole(int32_t pitch_q8)
{
    uint32_t g = (uint32_t)mfe_cutoff_g(pitch_q8);                    /* Q16 */
    int32_t G = 32768 - (int32_t)(0x80000000u / (65536u + g));          /* 32-bit divide only */
    return G > 32767 ? 32767 : G;
}
/* reads, delay d in Q8 samples (>= 1 sample) behind index w / last */
static inline int32_t rd_pow2(const int16_t *b, uint32_t mask, uint32_t w, int32_t dq8)
{
    uint32_t di = (uint32_t)dq8 >> 8;
    int32_t f = dq8 & 255, a = b[(w - di) & mask], c = b[(w - di - 1u) & mask];
    return a + (((c - a) * f) >> 8);
}
static inline int32_t rd_lin(const int16_t *b, int32_t len, int32_t last, int32_t dq8)
{
    int32_t di = dq8 >> 8, f = dq8 & 255, i0 = last - di, i1;
    if (i0 < 0) i0 += len;
    i1 = i0 ? i0 - 1 : len - 1;
    return b[i0] + (((b[i1] - b[i0]) * f) >> 8);
}

/* ------------------------------------------------------------------ state --- */
typedef struct { int32_t env, gain, gtgt, dcx, dcy, lp; } tape_ch_t;
struct tape_s { tape_ch_t ch[2]; int16_t line[2][MFE_TAPE_LEN]; uint32_t w, ph1, ph2, cnt; };
struct ens_s { int16_t line[2][MFE_ENS_LEN]; uint32_t w, ph; int32_t lp[2]; };
struct bbd_s { int16_t line[MFE_DELAY_LEN]; int32_t last, time, in_lp, fb_lp, out_lp; uint32_t ph1, ph2; uint8_t half; };
enum { ON_FUZZ, ON_TAPE, ON_ENS, ON_BBD, ON_OIL, ON_REV, ON_N };

struct mfe_fx_state {
    uint8_t on[ON_N], rev_kind;
    uint32_t bpm_q8;                     /* tempo for synced BBD ECHO / OIL CAN (mfe_set_tempo) */
    Velcro fuzz;
    struct tape_s tape;
    struct ens_s ens;
    struct bbd_s bbd;
    OilCan oil;
    int16_t oil_mem[2 * MFE_OILCAN_RING];
    union { TxTwinTank tt; TxSelfOsc so; } rev;
    int16_t rev_mem[5 * SP_RING];        /* twin tank: 5 springs; spring: 2 */
};

size_t mfe_fx_size(void) { return sizeof(mfe_fx_state_t); }
void mfe_fx_init(mfe_fx_state_t *f) { memset(f, 0, sizeof *f); f->bpm_q8 = 120u << 8; }   /* effects set up when switched on */
void mfe_fx_set_tempo(mfe_fx_state_t *f, uint32_t bpm_q8) { f->bpm_q8 = MFE_CLAMP(bpm_q8, 20u << 8, 400u << 8); }

/* ---- tempo sync (fx_sync, 17 Nov 2026): a delay time from a tempo division ----
 * One beat at bpm_q8 in Q8 samples at 44.1 kHz, 32-bit only (no 64-bit divide on the audio path):
 * 60 * 44100 * 256 = 677,376,000 = samples * bpm_q8. A division longer than max_q8 steps down to the longest
 * division that fits (the table runs short to long); if even 1/32 does not fit, the line's longest time. */
static uint32_t beat_q8(uint32_t bpm_q8)
{
    uint32_t n = (uint32_t)60u * MFE_SR * 256u;
    return (n / bpm_q8) * 256u + ((n % bpm_q8) * 256u) / bpm_q8;
}
static uint32_t sync_len_q8(uint32_t bpm_q8, unsigned div, uint32_t max_q8, unsigned *used)
{
    uint32_t b = beat_q8(bpm_q8), num, den, len;
    if (div >= MFE_SYNC_DIVS) div = MFE_SYNC_DIVS - 1;
    for (;;) {
        mfe_sync_beats(div, &num, &den);
        len = b * num / den;                             /* at most 132300 * 256 * 8: fits */
        if (len <= max_q8 || !div) break;
        div--;
    }
    if (used) *used = div;
    return len < max_q8 ? len : max_q8;
}
/* line limits, Q8 engine samples (44.1 kHz) */
#define BBD_LIM (MFE_DELAY_LEN - 16 - MFE_DELAY_LEN / 50)          /* 22.05 kHz samples, room for the wobble */
#define BBD_MAX_Q8 ((uint32_t)BBD_LIM * 512u)
#define BBD_MIN 220                                                /* 10 ms at 22.05 kHz */
static uint32_t oil_tap48(const OilCan *s, uint32_t bpm_q8, unsigned div, unsigned *used)
{
    uint32_t top = oilcan_tap_top(s);                               /* 48 kHz samples */
    uint32_t len = sync_len_q8(bpm_q8, div, CH_T(top) << 8, used);
    return ((len >> 4) * 160u / (MFE_SR / 300u)) >> 4;             /* engine Q8 -> 48 kHz samples (CH_T inverse) */
}

void mfe_fx_defaults(mfe_fx_t *x)
{
    memset(x, 0, sizeof *x);
    x->fuzz_octave = 32767; x->fuzz_tone = 16384; x->fuzz_level = 16384;
    x->tape_tone = 16384; x->tape_level = 16384;
    x->ens_rate = 16384; x->ens_depth = 16384; x->ens_mix = 16384;
    x->bbd_time = 9830; x->bbd_feedback = 13107; x->bbd_mod = 6553; x->bbd_tone = 16384;     /* 300 ms, 0.4 */
    /* OIL CAN voicing (17 Nov 2026: "proper delay repeats"): clear, darkening repeats with a moderate
     * Tel-Ray warble; the all-pass wash only when SMEAR is turned up. Feedback 0.6 = fb 0.55: about 5 repeats
     * above -26 dB. 130 ms free time; fx_sync 0 = free times for both delays */
    x->oil_time = 16384; x->oil_feedback = 22938; x->oil_warble = 4915; x->oil_smear = 3277;
}

/* what the delays play now: time in ms (rounded), and *div = the division used (MFE_SYNC_DIVS when free) */
unsigned mfe_fx_delay_ms(const mfe_fx_t *x, unsigned which, uint32_t bpm_q8, unsigned *div)
{
    unsigned sync = which ? (x->fx_sync & 15u) : (x->fx_sync >> 4), used = MFE_SYNC_DIVS;
    uint32_t q8;                                                    /* engine samples, Q8 */
    bpm_q8 = MFE_CLAMP(bpm_q8, 20u << 8, 400u << 8);
    if (!which) {
        if (sync) q8 = sync_len_q8(bpm_q8, sync - 1, BBD_MAX_Q8, &used);
        else {
            int32_t t = (MFE_CLAMP(x->bbd_time, 0, 32767) * 22051) >> 15;
            if (t < BBD_MIN) t = BBD_MIN;
            if (t > BBD_LIM) t = BBD_LIM;
            q8 = (uint32_t)t * 512u;
        }
    } else {
        OilCan o;                                                   /* the ring geometry only */
        o.mask = (MFE_OILCAN_RING
#ifdef MFE_OILCAN_LITE
                  * 2u
#endif
                  ) - 1u;
        uint32_t d48;
        if (sync) d48 = oil_tap48(&o, bpm_q8, sync - 1, &used);
        else {
            uint32_t top = 19200, room = OC_ROOM(&o);
            if (CH_T(top) > room) top = (uint32_t)(((uint64_t)room * 19200u) / CH_T(19200));
            d48 = m_span(1920, top, knob(x->oil_time));
            if (d48 > top) d48 = top;
        }
        if (d48 < 1920) d48 = 1920;
        q8 = CH_T(d48) << 8;
    }
    if (div) *div = used;
    return (unsigned)((q8 / 256u * 20u / (MFE_SR / 100u) + 1u) / 2u);   /* samples -> ms, rounded */
}

/* Q15 interleaved (any range) -> Chroma planar Q23 (clamped to the 24-bit range) */
static void to_planar(const int32_t *lr, int32_t *pcm)
{
    for (int i = 0; i < MOD_FRAMES; i++) {
        pcm[i] = mfe_sat16(lr[2 * i]) * 256;
        pcm[MOD_FRAMES + i] = mfe_sat16(lr[2 * i + 1]) * 256;
    }
}

/* -------------------------------------------------------------- OCT FUZZ --- */
static void fuzz_run(mfe_fx_state_t *f, const mfe_fx_t *p, int32_t *lr, unsigned n)
{
    /* knobs: TILT k1 = tone, AMOUNT k2 = amount, SENS k3 (clean blend) = 0; octave level = MFE change */
    FxControls c = {knob(p->fuzz_tone), knob(p->fuzz_amount), 0, 0, 0};
    bv_control(&f->fuzz, &c);
    f->fuzz.octg = MFE_CLAMP(p->fuzz_octave, 0, 32767) + 1;
    int32_t lvl = MFE_CLAMP(p->fuzz_level, 0, 32767), pcm[32];
    for (unsigned k = 0; k + MOD_FRAMES <= n; k += MOD_FRAMES) {
        int32_t *x = lr + 2 * k;
        to_planar(x, pcm);
        bv_process(&f->fuzz, pcm);
        for (int i = 0; i < MOD_FRAMES; i++) {
            x[2 * i] = ((pcm[i] >> 8) * lvl) >> 13;               /* level 0..4 (8192 = 1): the Chroma fuzz is quiet */
            x[2 * i + 1] = ((pcm[MOD_FRAMES + i] >> 8) * lvl) >> 13;
        }
    }
}

/* ------------------------------------------------------------------ TAPE --- */
static void tape_run(struct tape_s *t, const mfe_fx_t *p, int32_t *lr, unsigned n)
{
    int32_t drv = MFE_CLAMP(p->tape_drive, 0, 32767);
    int32_t g12 = 4096 + ((drv * 7) >> 3);                               /* 1 .. 8x, Q12 */
    int32_t tone = MFE_CLAMP(p->tape_tone, 0, 32767);
    int32_t G = pole(28436 + ((tone * (33591 - 28436)) >> 15));          /* 5 .. 16 kHz */
    int32_t bias = ((32767 - tone) * 1200) >> 15;                         /* darker = more bias: even harmonics */
    int32_t lvl = MFE_CLAMP(p->tape_level, 0, 32767);
    int32_t wow = MFE_CLAMP(p->tape_wow, 0, 32767);
    int32_t wdep = mfe_mul15(wow, 66 << 8), fdep = mfe_mul15(wow, 4 << 8);   /* 1.5 ms wow, 0.09 ms flutter */
    for (unsigned i = 0; i < n; i++) {
        if (!(t->cnt++ & 31u))                    /* gain computer: 2:1 above 0.5 FS, once per 32 samples */
            for (int c = 0; c < 2; c++) {
                tape_ch_t *h = &t->ch[c];
                int32_t e = h->env > (1 << 19) ? (1 << 19) : h->env;
                h->gtgt = e > 16384 ? (int32_t)(((uint32_t)(16384 + ((e - 16384) >> 1)) << 11) / (uint32_t)(e >> 4)) : 32768;
            }
        if (wow) { t->ph1 += HZ(0.6); t->ph2 += HZ(6.5); }
        for (int c = 0; c < 2; c++) {
            tape_ch_t *h = &t->ch[c];
            int32_t v = (int32_t)(((int64_t)lr[2 * i + c] * g12) >> 12);
            int32_t a = v < 0 ? -v : v;
            h->env += a > h->env ? (a - h->env) >> 6 : (a - h->env) >> 13;   /* 1.5 ms attack, 190 ms release */
            h->gain += (h->gtgt - h->gain) >> 5;
            v = (int32_t)(((int64_t)v * h->gain) >> 15) + bias;
            a = v < 0 ? -v : v;                                               /* soft knee from 0.5 FS */
            if (a > 16384) a = 16384 + ((16383 * mfe_softclip((a - 16384) * 2)) >> 15);
            v = v < 0 ? -a : a;
            h->dcy = v - h->dcx + mfe_mul15(h->dcy, 32604);                   /* DC block (the bias), ~35 Hz */
            h->dcx = v;
            v = lp1(&h->lp, G, h->dcy);                                       /* gentle high roll-off */
            if (wow) {                                                        /* lagged doubletrack with wow */
                t->line[c][t->w] = (int16_t)mfe_sat16(v);
                int32_t d = (88 << 8) + mfe_mul15(wdep, 16384 + (mfe_sin(t->ph1 + (c ? 0x80000000u : 0)) >> 1))
                            + mfe_mul15(fdep, mfe_sin(t->ph2));
                int32_t r = rd_pow2(t->line[c], MFE_TAPE_LEN - 1, t->w, d);
                v += mfe_mul15(r - v, wow >> 1);
            }
            lr[2 * i + c] = (int32_t)(((int64_t)v * lvl) >> 14);
        }
        t->w = (t->w + 1) & (MFE_TAPE_LEN - 1);
    }
}

/* -------------------------------------------------------------- ENSEMBLE --- */
static void ens_run(struct ens_s *s, const mfe_fx_t *p, int32_t *lr, unsigned n)
{
    unsigned mode = p->ens_mode;
    uint32_t inc;
    int32_t centre, depth;                       /* Q8 samples */
    if (mode == MFE_ENS_VIBRATO) {
        inc = HZ(2.0) + (uint32_t)(((uint64_t)MFE_CLAMP(p->ens_rate, 0, 32767) * HZ(10.0)) >> 15);
        depth = mfe_mul15(MFE_CLAMP(p->ens_depth, 0, 32767), 33869);        /* 0 .. 3 ms */
        centre = 5645 + depth;                                               /* 0.5 ms + depth */
    } else {
        /* the classic two-mode BBD chorus: 1.65 .. 5.35 ms swept by a triangle; I+II fast and shallow */
        inc = mode == MFE_ENS_I ? HZ(0.513) : mode == MFE_ENS_II ? HZ(0.863) : HZ(9.75);
        centre = 39514;                                                      /* 3.5 ms */
        depth = mode == MFE_ENS_I_II ? 2822 : 20886;                         /* 0.25 / 1.85 ms */
    }
    int32_t mix = MFE_CLAMP(p->ens_mix, 0, 32767) >> 1, G = pole(29927);   /* BBD darkening ~7 kHz */
    for (unsigned i = 0; i < n; i++) {
        s->ph += inc;
        int32_t m;
        if (mode == MFE_ENS_VIBRATO) m = mfe_sin(s->ph);
        else { int32_t t = (int32_t)(s->ph >> 16); m = (t < 32768 ? t : 65535 - t) * 2 - 32767; }
        for (int c = 0; c < 2; c++) {
            int32_t x = lr[2 * i + c];
            s->line[c][s->w] = (int16_t)mfe_sat16(x);
            int32_t mm = (c && mode != MFE_ENS_VIBRATO) ? -m : m;           /* stereo: inverted LFO on the right */
            int32_t wet = rd_pow2(s->line[c], MFE_ENS_LEN - 1, s->w, centre + mfe_mul15(mm, depth));
            wet = lp1(&s->lp[c], G, wet);
            lr[2 * i + c] = mode == MFE_ENS_VIBRATO ? wet : x + mfe_mul15(wet - x, mix);
        }
        s->w = (s->w + 1) & (MFE_ENS_LEN - 1);
    }
}

/* -------------------------------------------------------------- BBD ECHO --- */
static void bbd_run(struct bbd_s *b, const mfe_fx_t *p, int32_t *lr, unsigned n, int fresh, uint32_t bpm_q8)
{
    int32_t mix = MFE_CLAMP(p->bbd_mix, 0, 32767), tone = MFE_CLAMP(p->bbd_tone, 0, 32767);
    int32_t fb = MFE_CLAMP(p->bbd_feedback, 0, 32767), fbg = fb + (fb >> 4);   /* up to 1.06 */
    int32_t target;
    if (p->fx_sync >> 4)                                                         /* tempo division (Q8 at 22.05 kHz) */
        target = (int32_t)(sync_len_q8(bpm_q8, (p->fx_sync >> 4) - 1u, BBD_MAX_Q8, 0) >> 1);
    else {
        target = (MFE_CLAMP(p->bbd_time, 0, 32767) * 22051) >> 15;              /* 0 .. 1000 ms at 22.05 kHz */
        if (target > BBD_LIM) target = BBD_LIM;
        target <<= 8;
    }
    if (target < BBD_MIN << 8) target = BBD_MIN << 8;                           /* 10 ms minimum */
    int32_t modq = mfe_mul15(MFE_CLAMP(p->bbd_mod, 0, 32767), 1690 + target / 100);  /* 0.3 ms + 1 % */
    if (fresh) b->time = target;
    int32_t Gf = pole(25364 + ((tone * 4563) >> 15));                  /* in / out filters 2.5 .. 7 kHz */
    int32_t Gl = pole(23100 + (12 << 8) + ((tone * 4347) >> 15));      /* loop 1.5 .. 4 kHz at 22.05 kHz */
    for (unsigned i = 0; i < n; i++) {
        int32_t x = lp1(&b->in_lp, Gf, (lr[2 * i] + lr[2 * i + 1]) >> 1);
        b->time += (target - b->time) >> 11;                            /* time changes glide (pitch sweep) */
        b->ph1 += HZ(0.53); b->ph2 += HZ(2.9);
        int32_t d = b->time + mfe_mul15((mfe_sin(b->ph1) * 3 + mfe_sin(b->ph2)) >> 2, modq);
        int wrote = b->half;
        if (b->half) {                                                  /* one 22.05 kHz bucket step */
            int32_t r = lp1(&b->fb_lp, Gl, rd_lin(b->line, MFE_DELAY_LEN, b->last, d - 256));
            r = (mfe_softclip(r + (r >> 2)) * 26214) >> 15;            /* grit: unity small-signal, clips ~0.8 */
            if (++b->last >= MFE_DELAY_LEN) b->last = 0;
            b->line[b->last] = (int16_t)mfe_sat16(x + mfe_mul15(r, fbg));
        }
        b->half ^= 1;
        int32_t wet = lp1(&b->out_lp, Gf, rd_lin(b->line, MFE_DELAY_LEN, b->last, wrote ? d : d - 128));
        wet = mfe_mul15(wet, mix);
        lr[2 * i] += wet;
        lr[2 * i + 1] += wet;
    }
}

/* --------------------------------------------------------------- OIL CAN --- */
static void oil_run(mfe_fx_state_t *f, const mfe_fx_t *p, int32_t *lr, unsigned n, int fresh)
{
    /* knobs: TIME = time (disc speed), AMOUNT = feedback, DRIFT = warble, smear set directly (MFE change) */
    ModControls c = {knob(p->oil_time), (uint16_t)MFE_CLAMP(p->oil_feedback, 0, 32767),
                     (uint16_t)MFE_CLAMP(p->oil_warble, 0, 32767), 0, 0};
    if (p->fx_sync & 15u) c.tap = oil_tap48(&f->oil, f->bpm_q8, (p->fx_sync & 15u) - 1u, 0);   /* synced: the Chroma tap */
    f->oil.smear_set = MFE_CLAMP(p->oil_smear, 0, 32767) + 1;
    oilcan_control(&f->oil, &c);
    if (fresh) f->oil.delay = f->oil.target;               /* switched on: start at the time set (no glide from 200 ms) */
    int32_t mix = MFE_CLAMP(p->oil_mix, 0, 32767), pcm[32], wet[32];
    for (unsigned k = 0; k + MOD_FRAMES <= n; k += MOD_FRAMES) {
        int32_t *x = lr + 2 * k;
        to_planar(x, pcm);
        oilcan_process(&f->oil, pcm, wet);                 /* wet only: the dry stays as it is */
        for (int i = 0; i < MOD_FRAMES; i++) {
            x[2 * i] += mfe_mul15(wet[i] >> 8, mix);
            x[2 * i + 1] += mfe_mul15(wet[MOD_FRAMES + i] >> 8, mix);
        }
    }
}

/* ---------------------------------------------------------------- REVERB --- */
static void rev_run(mfe_fx_state_t *f, const mfe_fx_t *p, int32_t *lr, unsigned n)
{
    /* Texture reverbs have one knob: AMOUNT (k2) = rev_amount; HOLD = rev_hold */
    FxControls c = {0, knob(p->rev_amount), 0, (uint16_t)(p->rev_hold ? 1 : 0), 0};
    int tank = f->rev_kind == MFE_REV_TWIN_TANK;
    if (tank) tt_control(&f->rev.tt, &c); else so_control(&f->rev.so, &c);
    int32_t pcm[32], dry[32];
    for (unsigned k = 0; k + MOD_FRAMES <= n; k += MOD_FRAMES) {
        int32_t *x = lr + 2 * k;
        to_planar(x, pcm);
        memcpy(dry, pcm, sizeof dry);
        if (tank) tt_process(&f->rev.tt, pcm); else so_process(&f->rev.so, pcm);
        for (int i = 0; i < MOD_FRAMES; i++) {                /* add what the reverb added: the dry keeps its headroom */
            x[2 * i] += (pcm[i] - dry[i]) >> 8;
            x[2 * i + 1] += (pcm[MOD_FRAMES + i] - dry[MOD_FRAMES + i]) >> 8;
        }
    }
}

/* returns 1 when the effect has just been switched on (its state must be set up) */
static int turn(uint8_t *on, int want)
{
    int fresh = want && !*on;
    *on = (uint8_t)(want != 0);
    return fresh;
}

void mfe_fx_process(mfe_fx_state_t *f, const mfe_fx_t *p, int32_t *lr, unsigned n)
{
    /* the Chroma effects run in 16-frame sub-blocks: n is a multiple of 16 (MFE_BLOCK = 128) */
    if (turn(&f->on[ON_FUZZ], p->fuzz_amount > 0)) { memset(&f->fuzz, 0, sizeof f->fuzz); bv_attach(&f->fuzz, 0, 0); }
    if (f->on[ON_FUZZ]) fuzz_run(f, p, lr, n);

    if (turn(&f->on[ON_TAPE], p->tape_drive > 0)) {
        memset(&f->tape, 0, sizeof f->tape);
        for (int c = 0; c < 2; c++) f->tape.ch[c].gain = f->tape.ch[c].gtgt = 32768;
    }
    if (f->on[ON_TAPE]) tape_run(&f->tape, p, lr, n);

    int ens = p->ens_mode == MFE_ENS_VIBRATO || (p->ens_mode >= MFE_ENS_I && p->ens_mode <= MFE_ENS_I_II && p->ens_mix > 0);
    if (turn(&f->on[ON_ENS], ens)) memset(&f->ens, 0, sizeof f->ens);
    if (f->on[ON_ENS]) ens_run(&f->ens, p, lr, n);

    int fresh = turn(&f->on[ON_BBD], p->bbd_mix > 0);
    if (fresh) memset(&f->bbd, 0, sizeof f->bbd);
    if (f->on[ON_BBD]) bbd_run(&f->bbd, p, lr, n, fresh, f->bpm_q8);

    fresh = turn(&f->on[ON_OIL], p->oil_mix > 0);
    if (fresh) {
        memset(&f->oil, 0, sizeof f->oil);
        oilcan_attach(&f->oil, f->oil_mem, 2 * MFE_OILCAN_RING);   /* clears the rings */
    }
    if (f->on[ON_OIL]) oil_run(f, p, lr, n, fresh);

    unsigned kind = p->rev_type < MFE_REV_COUNT ? p->rev_type : MFE_REV_TWIN_TANK;
    if (f->on[ON_REV] && kind != f->rev_kind) f->on[ON_REV] = 0;    /* a new type starts clean */
    if (turn(&f->on[ON_REV], p->rev_amount > 0)) {
        memset(&f->rev, 0, sizeof f->rev);
        memset(f->rev_mem, 0, sizeof f->rev_mem);
        f->rev_kind = (uint8_t)kind;
        if (kind == MFE_REV_TWIN_TANK) tt_attach(&f->rev.tt, f->rev_mem, 5 * SP_RING);
        else so_attach(&f->rev.so, f->rev_mem, 2 * SP_RING);
    }
    if (f->on[ON_REV]) rev_run(f, p, lr, n);
}
