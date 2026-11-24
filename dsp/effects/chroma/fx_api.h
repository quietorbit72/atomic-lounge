/* Copied into Vintage Chonk on 17 Nov 2026 from chroma-mod/fx/fx_api.h
 * (the developer's own Chroma Console effects project). MFE changes, each marked "MFE:": include path of mod_common.h. */
/* Strange Gear Catalog effects: one API for all 100 (14 Nov 2026).
 *
 * Audio: 48 kHz, 16-frame planar stereo blocks of signed 24-bit samples in int32 (Q23): L = pcm[0..15],
 * R = pcm[16..31]. Same block format as the Chroma Console 1.04 audio path (see ../README.md).
 *
 * Output convention, per module (matches where each would sit in the pedal):
 *   FX_CHAR, FX_MOVE, FX_TEXT : process IN PLACE and return the full signal (the effect owns its dry/wet).
 *   FX_DIFF                   : replace pcm with the WET signal only; the pedal's MIX knob (the harness) adds dry.
 *
 * Controls (all knobs 0..65535, already de-noised by the pedal):
 *   k1  = TILT (Character) / RATE (Movement) / TIME (Diffusion) / unused (Texture has one knob)
 *   k2  = AMOUNT
 *   k3  = secondary control: SENSITIVITY (Character) or DRIFT (Movement, Diffusion); Texture: unused
 *   hold = 1 while TAP is held past 187.5 ms (see ../TAP-HOLD.md)
 *   tap  = tapped beat in samples (0 = none yet); from 0x080540f0 on the pedal
 *
 * Rules for every effect (the pedal is a fixed-point Blackfin with a tight budget):
 *   - integer only; no float, no division or libm in the per-sample loop (block-rate division is allowed);
 *   - every product must fit in 32 bits (Blackfin int is 32-bit; the host build must match the target bit for bit);
 *   - no allocation; large buffers come from `mem` (int16 words, cleared by the caller) via attach;
 *   - attach() must fully initialise the state from a zeroed struct and return 1, or 0 if mem is too small;
 *   - control() may be called any block; process() must be safe with any knob values;
 *   - silence in must give silence out (exact zeros) unless the descriptor sets FX_NOISY (e.g. an amp that hums);
 *   - stay bounded (no runaway) at every setting, including hold.
 */
#ifndef FX_API_H
#define FX_API_H
#include <stdint.h>
#include "mod_common.h"   /* MFE: same folder */

enum { FX_CHAR = 'C', FX_MOVE = 'M', FX_DIFF = 'D', FX_TEXT = 'T' };
enum { FX_NOISY = 1 };   /* flag: produces output (hum, hiss, drum machine) with silent input */

typedef struct {
    uint16_t k1, k2, k3, hold;
    uint32_t tap;
} FxControls;

typedef struct {
    int number;                 /* catalog number 1..100 */
    const char *id;             /* short id, e.g. "drip_tank" */
    const char *name;           /* catalog name */
    int module;                 /* FX_CHAR / FX_MOVE / FX_DIFF / FX_TEXT */
    unsigned flags;
    unsigned state_bytes;       /* sizeof(state struct) */
    unsigned mem_words;         /* int16 words of delay memory needed (0 if none) */
    int (*attach)(void *state, int16_t *mem, uint32_t words);
    void (*control)(void *state, const FxControls *c);
    void (*process)(void *state, int32_t pcm[32]);
    uint16_t demo_k1, demo_k2, demo_k3;   /* showcase knob settings for the audition render */
    uint16_t has_hold;                    /* 1 = HOLD does something; a second render holds TAP from 4 s to 8 s */
    const char *controls;                 /* one line: what k1/k2/k3/HOLD do, for the listening page */
} FxDesc;

/* Handy shared DSP helpers on top of mod_common.h. */
static inline int32_t fx_mul(int32_t a, int32_t q15) { return (a * q15) >> 15; }       /* a: 16-bit, q15 <= 32767 */
static inline int32_t fx_k15(uint16_t k) { return (int32_t)(k >> 1); }                    /* knob -> 0..32767 */
/* Smooth cubic soft clip: unity slope at 0, reaches full scale at |x| = 1.5 FS and stays there. 16-bit in/out. */
static inline int32_t fx_tanhish(int32_t x) {
    if (x > 49152) x = 49152;
    if (x < -49152) x = -49152;
    int32_t u = (x * 21845) >> 15;            /* 2/3 x, |u| <= 32768 */
    int32_t u2 = (u * u) >> 15;
    int32_t u3 = (u2 * u) >> 15;
    return m_sat16((3 * u - u3) >> 1);
}

#endif
