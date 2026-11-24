/* SPDX-License-Identifier: AGPL-3.0-only
 * Vintage Chonk Engine (MFE): a Massive-inspired, independently written wavetable synth voice.
 *
 * Portable C99. The audio path uses integers only (no float): the FM1's JieLi PI32v2 FPU is
 * unverified and the proven FM1 firmwares (Clipper, fm1_open) are fixed-point. Table
 * generation at init (mfe_init_tables) uses double maths; on the FM1 those tables will be
 * generated offline as const data (see docs/DSP_DESIGN.md).
 *
 * Conventions: audio Q15 in int32 (headroom above +-1.0), parameters Q15 (0..32767 = 0..1,
 * or -32768..32767 = -1..1 where stated), phases u32 (2^32 = one cycle).
 * Sample rate 44100 Hz, block MFE_BLOCK frames, control tick every MFE_CTL samples. */
#ifndef MFE_H
#define MFE_H
#include <stdint.h>
#include <stddef.h>

#define MFE_SR 44100
#define MFE_BLOCK 128            /* = FM1_BLOCK_FRAMES */
#define MFE_CTL 32               /* control-rate period in samples (as Clipper's CTL) */
#ifndef MFE_MAX_VOICES
#define MFE_MAX_VOICES 8
#endif
#define MFE_NOSC 3
#define MFE_NFILT 2
#define MFE_NENV 4               /* ENV 4 is the amplitude envelope, as in Massive */
#define MFE_NLFO 4
#define MFE_NMOD 16
#define MFE_NMACRO 8
#define MFE_NAME_LEN 24
#define MFE_PITCH_MOD_RANGE 64       /* full-scale pitch modulation in semitones (Massive: 128 st per depth 1) */
#define MFE_TIME_RANGE 60000.0   /* envelope/glide times: 0.5 ms * 60000^p = 0.5 ms .. 30 s */

/* ---------------------------------------------------------------- tables --- */
#define MFE_WT_FRAME 256         /* samples per frame at level 0 (the fullest band-limited copy) */
#define MFE_WT_FRAMES 8          /* frames per table (position morphs between them) */
#define MFE_WT_LEVELS 8          /* band-limited copies: 128, 64, ... 1 harmonics */
enum { MFE_WT_BASIC, MFE_WT_PWM, MFE_WT_SYNC, MFE_WT_FORMANT, MFE_WT_ORGAN, MFE_WT_SQUASH,
       MFE_WT_MOOG,             /* classic-Moog-style oscillator: position picks the 7 Model D waveforms */
       MFE_WT_COUNT };
/* MFE_WT_MOOG frames (position = frame / 7): 0 triangle, 1 shark (tri-saw), 2 reverse saw, 3 saw,
 * 4 square, 5 wide pulse (30 %), 6 narrow pulse (12 %), 7 narrowest pulse (6 %) */

void mfe_init_tables(void);      /* once, before anything else (everything at once: desktop) */
/* Device boot in pieces (16 Nov 2026, after the first hardware boot did not reach USB): the small
 * tables at once (fast), then the wavetables one frame per call from the control loop (each call
 * is about 1.3 M float operations); the engine is silent until mfe_tables_ready(). */
void mfe_init_tables_small(void);
int mfe_init_tables_step(void);  /* 1 when every wavetable frame is built */
int mfe_tables_ready(void);
const char *mfe_wavetable_name(unsigned t);

/* ----------------------------------------------------------------- patch --- */
enum { MFE_POLY, MFE_MONO, MFE_LEGATO };
enum { MFE_F_OFF, MFE_F_LP2, MFE_F_LP4, MFE_F_BP, MFE_F_HP, MFE_F_NOTCH,
       MFE_F_LADDER,            /* classic Moog-style 24 dB transistor ladder: 4 one-poles, resonance feedback,
                                 * input saturation; self-oscillates at full resonance */
       MFE_F_ACID,              /* TB-303-style diode ladder: poles spread (about 18 dB/oct), a high-pass
                                 * in the resonance path (bass kept at high resonance), hotter saturation */
       MFE_F_COUNT };
/* Not provided on purpose (16 Nov 2026): Massive's Double Notch (converted to one NOTCH). */
enum { MFE_W_SINE, MFE_W_TRI, MFE_W_SAW_UP, MFE_W_SAW_DOWN, MFE_W_SQUARE, MFE_W_SH,
       MFE_W_PERFORMER,         /* Massive-style Performer: 16 steps, each a curve shape x a level */
       MFE_W_STEPPER,           /* Massive-style Stepper: 16 step levels with glide */
       MFE_W_COUNT };
#define MFE_PERF_STEPS 16
#define MFE_PERF_SHAPES 32       /* 0-15 recreate Massive's Performer library, 16-31 Vintage Chonk's own */
#define MFE_SYNC_DIVS 13         /* tempo divisions, see mfe_sync_beats() */
#define MFE_OSC_ON 1u
#define MFE_OSC_RESET 2u         /* phase 0 at each note */
#define MFE_OSC_LO 4u            /* Model D "LO" range: 7 octaves down, no keyboard tracking (an LFO) */
#define MFE_LFO_KEYSYNC 1u
#define MFE_LFO_UNIPOLAR 2u
#define MFE_LFO_SYNC 4u          /* rate is a tempo division (sync_div) instead of Hz */
#define MFE_ENV_LOOP 1u

/* modulation sources */
enum {
    MFE_S_NONE, MFE_S_ENV1, MFE_S_ENV2, MFE_S_ENV3, MFE_S_ENV4,
    MFE_S_LFO1, MFE_S_LFO2, MFE_S_LFO3, MFE_S_LFO4,
    MFE_S_VEL, MFE_S_KEY, MFE_S_MODWHEEL, MFE_S_BEND, MFE_S_AFTERTOUCH,
    MFE_S_MACRO1, MFE_S_MACRO2, MFE_S_MACRO3, MFE_S_MACRO4,
    MFE_S_MACRO5, MFE_S_MACRO6, MFE_S_MACRO7, MFE_S_MACRO8,
    MFE_S_RANDOM, MFE_S_ONE,
    MFE_S_OSC3,                  /* oscillator 3's own waveform as a modulator (the Model D trick) */
    MFE_S_COUNT
};
/* modulation destinations (full-scale amount = the range given) */
enum {
    MFE_D_NONE,
    MFE_D_PITCH,                                 /* all oscillators, +-MFE_PITCH_MOD_RANGE semitones */
    MFE_D_OSC1_PITCH, MFE_D_OSC2_PITCH, MFE_D_OSC3_PITCH,   /* +-MFE_PITCH_MOD_RANGE semitones */
    MFE_D_OSC1_POS, MFE_D_OSC2_POS, MFE_D_OSC3_POS,         /* +-1 of the table */
    MFE_D_OSC1_WARP, MFE_D_OSC2_WARP, MFE_D_OSC3_WARP,
    MFE_D_OSC1_AMP, MFE_D_OSC2_AMP, MFE_D_OSC3_AMP,
    MFE_D_NOISE_AMP,
    MFE_D_F1_CUTOFF, MFE_D_F2_CUTOFF,            /* +-1 = the whole cutoff range */
    MFE_D_F1_RES, MFE_D_F2_RES,
    MFE_D_SERPAR, MFE_D_DRIVE, MFE_D_AMP, MFE_D_PAN,
    MFE_D_LFO1_RATE, MFE_D_LFO2_RATE,
    MFE_D_COUNT
};

typedef struct {
    uint8_t table, flags;
    int8_t semis;                /* -64..63 (Model D footage: 32' = -24, 16' = -12, 8' = 0, 4' = +12, 2' = +24) */
    int8_t drift;                /* 0..127: analogue pitch drift, up to about +-12 cents per voice and oscillator */
    int16_t fine;                /* cents, -100..100 */
    int16_t position;            /* Q15 0..1 across the table's frames */
    int16_t warp;                /* Q15 0..1: phase bend ("intensity", an approximation) */
    int16_t amp;                 /* Q15 */
    int16_t route;               /* Q15: 0 = filter 1, 1 = filter 2 */
} mfe_osc_t;

typedef struct {
    uint8_t type, reserved;
    int16_t cutoff;              /* Q15 0..1 = MIDI note 0..136 (8 Hz .. ~21 kHz, clamped below Nyquist) */
    int16_t res;                 /* Q15 */
    int16_t drive;               /* Q15: input gain 1..8 into a soft clip */
    int16_t keytrack;            /* Q15 -1..1: 1 = the cutoff follows the key 1:1 (around C3) */
} mfe_filter_t;

typedef struct {
    int16_t delay, attack, decay, sustain, release;   /* times Q15 (0.5 ms .. 30 s, exponential), sustain level Q15 */
    uint8_t flags, reserved;
    /* Massive-style stages (preset v2): attack rises to att_level, decay falls to dec_level, then the
     * level moves to `sustain` over sust_time. dec_level < 0 = "same as sustain" (a plain ADSR). */
    int16_t att_level, dec_level, sust_time;
} mfe_env_t;

typedef struct {
    uint8_t wave, flags;
    int16_t rate;                /* Q15: 0.02 .. 40 Hz, exponential (one cycle; for Performer/Stepper one step) */
    int16_t phase;               /* Q15 start phase (key sync) */
    /* preset v3: Performer / Stepper data (ignored by the plain LFO waves) */
    uint8_t sync_div;            /* with MFE_LFO_SYNC: index into mfe_sync_beats() */
    uint8_t loop_start, loop_end;            /* steps played: loop_start .. loop_end (inclusive) */
    uint8_t reserved;
    int16_t glide;               /* Stepper: glide between steps, Q15 of a step */
    uint8_t step_shape[MFE_PERF_STEPS];      /* Performer: curve shape per step, 0..MFE_PERF_SHAPES-1 */
    int8_t step_level[MFE_PERF_STEPS];       /* Performer amplitude / Stepper value per step, -127..127 */
} mfe_lfo_t;

typedef struct {
    uint8_t src, dst;
    int16_t amount;              /* Q15 -1..1 */
} mfe_mod_t;

/* Massive-style modulation oscillator (preset v4): one sine per voice at the note pitch + semis,
 * feeding ring modulation, phase modulation and wavetable-position modulation of one oscillator
 * each (0 = off, 1..3). Filter FM is stored but not rendered yet. */
typedef struct {
    int8_t semis;                /* -48..48 relative to the note */
    uint8_t rm_osc, pm_osc, pos_osc, ffm_filter, reserved;
    int16_t rm, pm, pos, ffm;    /* amounts, Q15 */
} mfe_modosc_t;

/* Master effects (preset v5, 17 Nov 2026), on the stereo bus in this order:
 *   OCT FUZZ -> TAPE -> ENSEMBLE -> BBD ECHO -> OIL CAN -> REVERB (TWIN TANK or SPRING)
 * Every parameter is Q15 0..32767 unless stated; each effect is bypassed (bit for bit, no CPU) when its
 * on-parameter (the first of its group) is 0. OCT FUZZ, OIL CAN and REVERB are the developer's Chroma Console
 * effects (dsp/effects/chroma, copied from the developer's chroma-mod project); TAPE, ENSEMBLE and BBD ECHO
 * are MFE's own (docs/DSP_DESIGN.md, "Master effects"). */
enum { MFE_ENS_OFF, MFE_ENS_I, MFE_ENS_II, MFE_ENS_I_II, MFE_ENS_VIBRATO, MFE_ENS_COUNT };
enum { MFE_REV_TWIN_TANK, MFE_REV_SPRING, MFE_REV_COUNT };
typedef struct {
    /* OCT FUZZ (Chroma No. 103): octave-down divider into a starved, gated fuzz and a blown speaker */
    int16_t fuzz_amount;         /* 0 = off; fuzz gain (low = sputtery and gated, high = fat) */
    int16_t fuzz_octave;         /* octave-down level into the fuzz (32767 = the Chroma original) */
    int16_t fuzz_tone;           /* dead-speaker roll-off, dark .. bright */
    int16_t fuzz_level;          /* output, 0..4 (8192 = 1; default 16384 = 2, as the Chroma fuzz runs quiet) */
    /* TAPE: warm tape saturation (soft knee, gentle 2:1 compression, high roll-off, optional wow) */
    int16_t tape_drive;          /* 0 = off; 1..8x into the tape. MFE_D_DRIVE modulates this */
    int16_t tape_tone;           /* roll-off 5 .. 16 kHz; darker settings bias the tape (more even harmonics) */
    int16_t tape_level;          /* output, 0..2 (16384 = 1) */
    int16_t tape_wow;            /* 0 = none; wow + flutter on a lagged doubletrack copy */
    /* ENSEMBLE: 1980s polysynth BBD chorus */
    uint8_t ens_mode;            /* MFE_ENS_*: 0 off, 1 I (0.5 Hz), 2 II (0.8 Hz), 3 I+II (fast, shallow), 4 VIBRATO */
    uint8_t fx_sync;             /* tempo sync of the delays (17 Nov 2026; was a spare byte, so older v5 files read 0 = free):
                                  * low nibble = OIL CAN division + 1, high nibble = BBD ECHO division + 1, 0 = free time
                                  * (divisions = mfe_sync_beats() indices; one that does not fit the line steps down) */
    int16_t ens_mix;             /* modes I..I+II: dry .. 50/50 (vibrato is wet only) */
    int16_t ens_rate, ens_depth; /* VIBRATO only: 2 .. 12 Hz, 0 .. 3 ms */
    /* BBD ECHO: bucket-brigade delay, mono line at 22.05 kHz, darker and grittier each repeat */
    int16_t bbd_mix;             /* 0 = off; wet level added to the dry */
    int16_t bbd_time;            /* absolute: 0 .. 1000 ms (10 ms minimum), clamped to the line (MFE_DELAY_LEN); unused when synced */
    int16_t bbd_feedback;        /* 0 .. 1.06 (a little self-oscillation at the top, held by the clip) */
    int16_t bbd_mod;             /* clock wobble depth */
    int16_t bbd_tone;            /* dark .. bright (filters 2.5 .. 7 kHz, the loop 1.5 .. 4 kHz) */
    /* OIL CAN (Chroma oil can delay): short warbly repeats that smear into a wash */
    int16_t oil_mix;             /* 0 = off; wet level added to the dry */
    int16_t oil_time;            /* disc speed: 40 .. 400 ms (top limited by MFE_OILCAN_RING) and wobble rate; unused when synced */
    int16_t oil_feedback;        /* 0 .. 0.92 */
    int16_t oil_warble;          /* the Chroma DRIFT: motor wander, oil dropouts, wobble depth */
    int16_t oil_smear;           /* all-pass smear of the repeats */
    /* REVERB (Chroma Texture No. 142 / 141) */
    uint8_t rev_type;            /* MFE_REV_TWIN_TANK (long 3-spring tank left, short 2-spring right) or MFE_REV_SPRING
                                  * (self-oscillating spring: sings above about 3/4) */
    uint8_t rev_hold;            /* 1 = HOLD (SPRING: open loop, sustained howl; TWIN TANK: no effect) */
    int16_t rev_amount;          /* 0 = off; the Chroma AMOUNT macro (dwell / loop gain and wet level together) */
} mfe_fx_t;

typedef struct {
    char name[MFE_NAME_LEN];
    uint8_t voice_mode, max_voices, bend_range;
    uint8_t vel_sens;            /* 0..127: amplitude gain = 1 - s + s * velocity */
    int16_t glide;               /* Q15 time, 0 = off */
    int16_t master;              /* output gain, Q13: 8192 = 1.0, 32767 = 4.0 (Massive levels need > 1) */
    int16_t macro[MFE_NMACRO];   /* initial macro values, Q15 */
    mfe_osc_t osc[MFE_NOSC];
    int16_t noise_amp, noise_color, noise_route;
    mfe_filter_t filt[MFE_NFILT];
    int16_t serpar;              /* Q15: 0 = parallel, 1 = serial (filter 1 into filter 2) */
    int16_t f1_mix, f2_mix;      /* Q15 output levels of the filters */
    mfe_env_t env[MFE_NENV];
    mfe_lfo_t lfo[MFE_NLFO];
    mfe_mod_t mod[MFE_NMOD];
    mfe_fx_t fx;
    mfe_modosc_t modosc;
} mfe_patch_t;

void mfe_patch_init(mfe_patch_t *p);     /* a plain saw through an open low-pass */
void mfe_fx_defaults(mfe_fx_t *x);       /* every effect off, the other knobs at their usual settings */
#define MFE_FX_SYNC_OIL(x) ((x)->fx_sync & 15u)          /* 0 free, else division + 1 */
#define MFE_FX_SYNC_BBD(x) ((x)->fx_sync >> 4)
/* the delay time playing for these settings at a tempo (BPM << 8), in ms; which 0 = BBD ECHO, 1 = OIL CAN.
 * *div (may be 0) = the division used (a synced one that does not fit the line steps down), MFE_SYNC_DIVS when free */
unsigned mfe_fx_delay_ms(const mfe_fx_t *x, unsigned which, uint32_t bpm_q8, unsigned *div);

/* Binary preset (.mfp), versioned, little-endian, CRC-32 protected (docs/FIRMWARE_ARCHITECTURE.md) */
#define MFE_PRESET_MAGIC "MFP1"
#define MFE_PRESET_VERSION 5u      /* v2: envelope stages; v3: Performer/Stepper; v4: mod osc; v5: new master
                                    * effects (v1-v4 effects mapped: drive->TAPE, chorus->ENSEMBLE I, delay->BBD,
                                    * reverb->TWIN TANK); older read */
size_t mfe_preset_size(void);
size_t mfe_preset_write(const mfe_patch_t *p, uint8_t *out, size_t cap);
/* 0 ok, -1 short, -2 magic, -3 version, -4 crc, -5 value out of range */
int mfe_preset_read(mfe_patch_t *p, const uint8_t *in, size_t len);

/* ---------------------------------------------------------------- engine --- */
typedef struct mfe_engine mfe_engine_t;
size_t mfe_engine_size(void);
void mfe_engine_init(mfe_engine_t *e);
void mfe_engine_set_patch(mfe_engine_t *e, const mfe_patch_t *p);   /* between blocks */
void mfe_note_on(mfe_engine_t *e, uint8_t note, uint8_t vel);
void mfe_note_off(mfe_engine_t *e, uint8_t note);
void mfe_midi(mfe_engine_t *e, const uint8_t *msg, size_t len);   /* one complete short message */
void mfe_all_notes_off(mfe_engine_t *e);
/* Render MFE_BLOCK stereo frames, interleaved L R, Q15 in int32 (saturated to int16 range). */
void mfe_render(mfe_engine_t *e, int32_t *out);
unsigned mfe_active_voices(const mfe_engine_t *e);
void mfe_set_tempo(mfe_engine_t *e, uint32_t bpm_q8);   /* tempo for synced LFOs and delays (BPM << 8; default 120) */
const char *mfe_perf_shape_name(unsigned id);
/* previews for user interfaces: one frame sample of `table` at `position_q15` (i over 0..255) and a
 * Performer shape value (x 0..65535 across a step), both Q15 */
int32_t mfe_preview_wave(unsigned table, int32_t position_q15, unsigned i);
int32_t mfe_preview_shape(unsigned id, uint32_t x);
/* tempo division `d` as beats per cycle (or per step): numerator / denominator */
void mfe_sync_beats(unsigned d, uint32_t *num, uint32_t *den);

#endif
