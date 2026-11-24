/* Copied into Vintage Chonk on 17 Nov 2026 from chroma-mod/src/mod_delays.h
 * (the developer's own Chroma Console effects project). MFE changes, each marked "MFE:": OilCan.smear_set (direct SMEAR control), OilCan.lite_*
 * (the FM1's half-rate mono ring, MFE_OILCAN_LITE). */
/* Chroma Console Mod-bank delays: Oil can, Echoplex, Disintegration loop, Phonograph echo, Shortwave relay.
 * Each effect: zero the state struct, attach caller-owned delay memory (it is cleared), call *_control once
 * per block when controls change, then *_process per 16-frame block. Output is WET only; the pedal's own
 * MIX/EFFECT VOL mixes it with dry, as for the factory Cascade and Reels. */
#ifndef MOD_DELAYS_H
#define MOD_DELAYS_H
#include "mod_common.h"

enum { OC_AP1 = 113, OC_AP2 = 337 };

typedef struct {
    int16_t *ring[2];
    uint32_t mask, w, phase, inc, dropout_left, seed;
    int32_t delay, target, depth, wander, wander_target;
    int32_t fb, smear, smear_target, gain, gain_target, drift;
    int32_t lp[2], lo[2];
    uint32_t ap1_pos, ap2_pos;
    int16_t ap1[2][OC_AP1], ap2[2][OC_AP2];
    int32_t smear_set;   /* MFE: 0 = smear from DRIFT (original); 1..32768 = smear (Q15) + 1 set directly */
    int32_t lite_acc, lite_lp;   /* MFE: MFE_OILCAN_LITE only: half-rate pair sum, input anti-alias low-pass */
} OilCan;

typedef struct {
    int16_t *ring[2];
    uint32_t mask, w, wow, flut, seed;
    int32_t delay, target, wow_depth, flut_depth, fb, hiss, tone_a;
    int32_t pre_lo[2], lp1[2], lp2[2], env[2];
} Echoplex;

typedef struct {
    int16_t *loop;
    uint32_t mask, w, gap_left, seed;
    int32_t len, target, max_len, fb, shed_a, gap_gain, gap_target, in_gain, in_target, drift;
    int32_t shed, dc;
} Disintegrate;

typedef struct {
    int16_t *ring[2];
    uint32_t mask, w, speed, warp, seed;
    int32_t lag, target, wgain, warp_depth, fb, chance, hiss, held;
    int32_t hp[2], lp[2], lp2[2], env[2], click[2];
} Phonograph;

typedef struct {
    int16_t *ring[2];
    uint32_t mask, w, wphase, wdrift, fade_timer, seed;
    int32_t delay, target, fb, fade_depth, static_lvl, whistle_lvl, tune, tune_target;
    int32_t fade_a, fade_b, fade_a_t, fade_b_t;
    int32_t hp[2], hp2[2], lp[2], lp2[2], lo[2], env[2], nlp[2];
} Shortwave;

/* Memory: words of int16 the caller supplies (power-of-two halves for stereo rings). */
enum {
    OILCAN_WORDS = 2 * 32768,       /* 128 kB: 40-400 ms */
    ECHOPLEX_WORDS = 2 * 32768,     /* 128 kB: 60-650 ms */
    DISINTEGRATE_WORDS = 131072,    /* 256 kB mono loop: 0.25-2.7 s */
    PHONOGRAPH_WORDS = 2 * 32768,   /* 128 kB: 80-450 ms plus the stop */
    SHORTWAVE_WORDS = 2 * 32768     /* 128 kB: 60-640 ms */
};

int oilcan_attach(OilCan *s, int16_t *mem, uint32_t words);
void oilcan_control(OilCan *s, const ModControls *c);
void oilcan_process(OilCan *s, const int32_t in[32], int32_t wet[32]);
uint32_t oilcan_tap_top(const OilCan *s);   /* MFE: longest tapped / synced delay the ring holds, 48 kHz samples */

int echoplex_attach(Echoplex *s, int16_t *mem, uint32_t words);
void echoplex_control(Echoplex *s, const ModControls *c);
void echoplex_process(Echoplex *s, const int32_t in[32], int32_t wet[32]);

int disintegrate_attach(Disintegrate *s, int16_t *mem, uint32_t words);
void disintegrate_control(Disintegrate *s, const ModControls *c);
void disintegrate_process(Disintegrate *s, const int32_t in[32], int32_t wet[32]);

int phonograph_attach(Phonograph *s, int16_t *mem, uint32_t words);
void phonograph_control(Phonograph *s, const ModControls *c);
void phonograph_process(Phonograph *s, const int32_t in[32], int32_t wet[32]);

int shortwave_attach(Shortwave *s, int16_t *mem, uint32_t words);
void shortwave_control(Shortwave *s, const ModControls *c);
void shortwave_process(Shortwave *s, const int32_t in[32], int32_t wet[32]);

unsigned mod_state_bytes(int which); /* 0 oilcan .. 4 shortwave */

#endif
