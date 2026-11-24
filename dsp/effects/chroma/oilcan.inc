/* Copied into Vintage Chonk on 17 Nov 2026 from chroma-mod/src/oilcan.c
 * (the developer's own Chroma Console effects project). MFE changes, each marked "MFE:": rings from 4096 words, knob top limited by the ring, CH_T / CH_INC rescale, direct smear,
 * tapped / synced times up to the whole ring, and the FM1's half-rate mono ring (MFE_OILCAN_LITE, 17 Nov 2026). */
/* Oil can delay, after the Tel-Ray / Morley "Adineko" electrostatic disc (also in the Fender Dimension IV).
 * Short-to-mid repeats with heavy disc warble; repeats smear into a wash through two short all-passes.
 * TIME: disc speed (40-400 ms; faster disc = shorter delay and faster wobble, 6 Hz down to 1 Hz).
 * AMOUNT: feedback (to 0.92). DRIFT: motor wander and oil dropouts, more wobble depth and smear.
 * HOLD: "smear": the repeats dissolve into a wash, feedback 0.96. */
#include "mod_delays.h"

/* MFE: MFE_OILCAN_LITE (the FM1 build, 17 Nov 2026: RAM is short, the SDK needs its heap). The oil can keeps ONE
 * mono ring at half rate (22.05 kHz) in the same memory: input (L+R)/2, low-passed (~4.9 kHz) and averaged over
 * sample pairs; each output channel reads the shared ring with its own wobble phase (stereo kept); the feedback is
 * the mean of the two outputs. 2 x 4096 words then hold about 370 ms instead of 2 x 93 ms. The echo is low-passed
 * at ~2.2 kHz anyway, so the half rate loses nothing audible. Without MFE_OILCAN_LITE the code is the original.
 * OC_ROOM: usable delay in engine samples (wobble + wander + interpolation headroom kept free). */
#ifdef MFE_OILCAN_LITE
#define OC_ROOM(s) (2u * ((s)->mask + 1u) - 640u)
#else
#define OC_ROOM(s) (((s)->mask + 1u) - 320u)
#endif

/* MFE: tapped / synced delays may use the whole ring (the TIME knob still stops at 400 ms) */
uint32_t oilcan_tap_top(const OilCan *s) {
    return (uint32_t)(((uint64_t)OC_ROOM(s) * 19200u) / CH_T(19200));
}

int oilcan_attach(OilCan *s, int16_t *mem, uint32_t words) {
#ifdef MFE_OILCAN_LITE
    uint32_t n = words;                                    /* MFE: one mono ring of all the memory */
#else
    uint32_t n = words / 2;
#endif
    if (!s || !mem || !m_pow2(n) || n < 4096) return 0;   /* MFE: smaller rings allowed (shorter top time) */
    for (uint32_t i = 0; i < words; ++i) mem[i] = 0;
    s->ring[0] = mem;
#ifdef MFE_OILCAN_LITE
    s->ring[1] = mem;                                      /* MFE: shared */
#else
    s->ring[1] = mem + n;
#endif
    s->mask = n - 1;
    s->seed = 0x0c0ffee1u;
    s->gain = s->gain_target = 32767;
    s->delay = s->target = (int32_t)(CH_T(9600) << 8);   /* MFE: CH_T */
    return 1;
}

void oilcan_control(OilCan *s, const ModControls *c) {
    /* MFE: the knob spans 40 ms .. the ring's top (400 ms with a 32768 ring); times rescaled to the engine rate */
    uint32_t top = 19200, room = OC_ROOM(s);                /* 320: wobble + wander headroom (MFE: OC_ROOM) */
    if (CH_T(top) > room) top = (uint32_t)(((uint64_t)room * 19200u) / CH_T(19200));
    uint32_t d = c->tap ? c->tap : m_span(1920, top, c->time);
    if (c->tap) top = oilcan_tap_top(s);                    /* MFE: a tapped / synced time may use the whole ring */
    if (d < 1920) d = 1920;
    if (d > top) d = top;
    s->target = (int32_t)(CH_T(d) << 8);
    uint32_t t = ((d - 1920) * 3793u) >> 16;               /* 0..1000 */
    if (t > 1000) t = 1000;                                /* MFE: past 400 ms the disc wobble stays at 1 Hz */
    s->inc = CH_INC(536871u - 447u * t);                   /* 6 Hz .. 1 Hz */
    s->depth = (int32_t)CH_T(7373 + ((29491 * (int32_t)c->drift) >> 15)); /* 0.6 .. 3 ms, Q8 */
    s->fb = c->hold ? 31457 : ((int32_t)c->amount * 30147) >> 15;
    s->smear_target = c->hold ? 32767 : s->smear_set ? s->smear_set - 1   /* MFE: direct SMEAR */
                                      : 9830 + ((9830 * (int32_t)c->drift) >> 15);
    s->drift = c->drift;
}

void oilcan_process(OilCan *s, const int32_t in[32], int32_t wet[32]) {
    /* Block-rate motor wander and oil dropouts. */
    if ((m_rand(&s->seed) >> 24) < 6) {
        int32_t amp = (18432 * s->drift) >> 15;
        s->wander_target = (m_noise(&s->seed) * amp) >> 15;
    }
    if (s->dropout_left) {
        --s->dropout_left;
        s->gain_target = 9830;
    } else {
        s->gain_target = 32767;
        if ((int32_t)(m_rand(&s->seed) >> 16) < (s->drift >> 10))
            s->dropout_left = 90 + (((m_rand(&s->seed) >> 24) * 180u) >> 8);
    }
    s->wander += (s->wander_target - s->wander) >> 6;
    s->smear += (s->smear_target - s->smear) >> 5;

#ifdef MFE_OILCAN_LITE
    /* MFE: half-rate mono ring. Ring slot j holds the mean of engine samples 2j and 2j+1 (time 2j + 0.5), written
     * after the reads of sample 2j+1. At sample w (W = w >> 1) a delay of d (Q8 engine samples) is slot position
     * W - (d + 128) / 2 for even w and W - (d - 128) / 2 for odd w: m_read with delay_q8 = d/2 + 64 - 128 * odd.
     * Slot W is not yet written at either sample, so delay_q8 >= 256 (reads W-1 and older). */
    for (unsigned n = 0; n < MOD_FRAMES; ++n) {
        s->delay = m_slew(s->delay, s->target, 11);
        s->gain += (s->gain_target - s->gain) >> 7;
        int32_t mono = m_lp(&s->lite_lp, (m_in16(in[n]) + m_in16(in[MOD_FRAMES + n])) >> 1, 16384);
        int32_t outsum = 0;
        uint32_t odd = s->w & 1u;
        for (unsigned ch = 0; ch < 2; ++ch) {
            int32_t mod = (m_sin(s->phase + (ch ? 0x40000000u : 0u)) * s->depth) >> 14;
            int32_t d = s->delay + mod + s->wander;
            if (d < 512) d = 512;
            int32_t dh = (d >> 1) + 64 - (odd ? 128 : 0);
            if (dh < 256) dh = 256;
            int32_t echo = m_read(s->ring[0], s->mask, s->w >> 1, (uint32_t)dh);
            echo = m_lp(&s->lp[ch], echo, 8192);                 /* about 2.2 kHz */
            echo = m_sat16(echo - m_lpq(&s->lo[ch], echo, 511));  /* lose below about 120 Hz */
            int16_t *a1 = &s->ap1[ch][s->ap1_pos];
            int32_t v = m_sat16(echo - ((*a1 * 19661) >> 15));
            int32_t y = m_sat16(*a1 + ((v * 19661) >> 15));
            *a1 = (int16_t)v;
            int16_t *a2 = &s->ap2[ch][s->ap2_pos];
            v = m_sat16(y - ((*a2 * 19661) >> 15));
            y = m_sat16(*a2 + ((v * 19661) >> 15));
            *a2 = (int16_t)v;
            int32_t out = echo + (((y - echo) * s->smear) >> 15);
            out = (out * s->gain) >> 15;
            if (!ch) outsum = out;
            wet[ch * MOD_FRAMES + n] = m_out23(out);
        }
        int32_t rec = m_knee(mono + ((outsum * s->fb) >> 15), 20000);
        if (odd) {
            s->ring[0][(s->w >> 1) & s->mask] = (int16_t)m_sat16((s->lite_acc + rec) >> 1);
        } else {
            s->lite_acc = rec;
        }
        ++s->w;
        s->phase += s->inc;
        if (++s->ap1_pos == OC_AP1) s->ap1_pos = 0;
        if (++s->ap2_pos == OC_AP2) s->ap2_pos = 0;
    }
#else
    for (unsigned n = 0; n < MOD_FRAMES; ++n) {
        s->delay = m_slew(s->delay, s->target, 11);
        s->gain += (s->gain_target - s->gain) >> 7;
        for (unsigned ch = 0; ch < 2; ++ch) {
            int32_t mod = (m_sin(s->phase + (ch ? 0x40000000u : 0u)) * s->depth) >> 14;
            int32_t d = s->delay + mod + s->wander;
            if (d < 512) d = 512;
            int32_t echo = m_read(s->ring[ch], s->mask, s->w, (uint32_t)d);
            echo = m_lp(&s->lp[ch], echo, 8192);                 /* about 2.2 kHz */
            echo = m_sat16(echo - m_lpq(&s->lo[ch], echo, 511));  /* lose below about 120 Hz */
            int16_t *a1 = &s->ap1[ch][s->ap1_pos];
            int32_t v = m_sat16(echo - ((*a1 * 19661) >> 15));
            int32_t y = m_sat16(*a1 + ((v * 19661) >> 15));
            *a1 = (int16_t)v;
            int16_t *a2 = &s->ap2[ch][s->ap2_pos];
            v = m_sat16(y - ((*a2 * 19661) >> 15));
            y = m_sat16(*a2 + ((v * 19661) >> 15));
            *a2 = (int16_t)v;
            int32_t out = echo + (((y - echo) * s->smear) >> 15);
            out = (out * s->gain) >> 15;
            int32_t rec = m_in16(in[ch * MOD_FRAMES + n]) + ((out * s->fb) >> 15);
            s->ring[ch][s->w & s->mask] = (int16_t)m_sat16(m_knee(rec, 20000));
            wet[ch * MOD_FRAMES + n] = m_out23(out);
        }
        ++s->w;
        s->phase += s->inc;
        if (++s->ap1_pos == OC_AP1) s->ap1_pos = 0;
        if (++s->ap2_pos == OC_AP2) s->ap2_pos = 0;
    }
#endif
}
