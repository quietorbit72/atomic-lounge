/* SPDX-License-Identifier: AGPL-3.0-only
 * Vintage Chonk ORBIT mode: poly-convolution / cross-synthesis engine (docs/ORBIT_MODE.md).
 *
 * Portable C99, float. The audio path (mfo_render, note and parameter calls) never allocates, locks or
 * waits; everything that allocates (engine creation, impulse kernels, keytrack sets, synth prints) is
 * meant for a worker / message thread and is handed to the audio thread as a pointer between blocks.
 *
 * Signal flow per layer (three layers):
 *   voices: SOURCE (sample player | MFE synth engine) -> amp envelope            (per voice)
 *           [KEYTRACK: per-voice BODY convolution with the impulse re-pitched to the note]
 *   bus:    sum -> tape colour -> BODY (zero-latency partitioned convolution)
 *               -> TONE / GRAIN / PITCH (STFT cross-synthesis, latency mfo_latency_samples())
 *               -> MOTION (impulse loudness contour) -> CHONK filter (ZDF ladder / SVF) -> level, soft sat
 *   ORBIT blender: three layer nodes + DRY on a 2-D field, puck (manual or orbit path) -> weights summing to 1
 *   master: drift, soft saturation, gain.
 * The impulse ("modulator") of a layer is either its own impulse buffer played as a stream (scan rate,
 * position, loop, retriggered at note-on) or LIVE: another layer's source bus.
 *
 * Every continuous parameter is smoothed per internal block (64 samples) and can be driven by the mod
 * matrix (mfo_set_mod / mfo_set_mod_source). */
#ifndef MFO_H
#define MFO_H
#include <stddef.h>
#include <stdint.h>
#include "mfe.h"

#ifdef __cplusplus
extern "C" {
#endif

#define MFO_LAYERS 3
#define MFO_MAX_VOICES 32        /* hard cap; the engine uses mfo_config_t.max_voices */
#define MFO_BLOCK 64             /* internal control / convolution base block (samples) */
#define MFO_NMOD 32              /* mod-matrix slots */

/* ------------------------------------------------------------------ FFT --- */
/* Real FFT, power-of-two n >= 4. Spectra in split form re[0..n/2], im[0..n/2]. */
typedef struct mfo_fft mfo_fft_t;
mfo_fft_t *mfo_fft_create(int n);
void mfo_fft_destroy(mfo_fft_t *f);
int mfo_fft_size(const mfo_fft_t *f);
void mfo_rfft(mfo_fft_t *f, const float *in, float *re, float *im);    /* forward, unscaled */
void mfo_irfft(mfo_fft_t *f, const float *re, const float *im, float *out); /* inverse, unscaled: out = n * x */

/* --------------------------------------------------------- convolution --- */
/* Impulse kernel (read-only once built; shareable between convolvers and threads).
 * Zero-latency non-uniform partitioning: taps [0,64) direct form, then uniform FFT partitions of 64,
 * 512 and 4096 samples covering [64,512), [512,4096), [4096,end) (Gardner-style, the larger partitions'
 * frequency-domain multiply-adds spread over the 64-sample ticks of their period). */
typedef struct mfo_kernel mfo_kernel_t;
/* ir[c] = channel c (channels 1 or 2), len samples at the engine rate. gain scales the taps.
 * Returns NULL on failure. Allocates. */
mfo_kernel_t *mfo_kernel_create(const float *const *ir, int channels, int len, float gain);
void mfo_kernel_destroy(mfo_kernel_t *k);
int mfo_kernel_length(const mfo_kernel_t *k);
int mfo_kernel_channels(const mfo_kernel_t *k);

/* In-place kernels (1.2.0, the live INPUT impulse): mfo_kernel_alloc makes an empty mono or stereo kernel with
 * room for max_len taps (allocates); a loader rebuilds a mono kernel from new taps a few FFT partitions at a time
 * without allocating (audio thread). The kernel must not be given to a convolver until mfo_kload_step returns 1;
 * ir must stay valid until then. */
mfo_kernel_t *mfo_kernel_alloc(int channels, int max_len);
typedef struct mfo_kload mfo_kload_t;
mfo_kload_t *mfo_kload_create(int max_len);
void mfo_kload_destroy(mfo_kload_t *l);
void mfo_kload_begin(mfo_kload_t *l, mfo_kernel_t *k, const float *ir, int len, float gain);
int mfo_kload_step(mfo_kload_t *l, int max_parts);       /* 1 = finished */

typedef struct mfo_conv mfo_conv_t;
/* in_channels 1 or 2, max_len = longest kernel it will run. Output always 2 channels:
 * mono in + stereo kernel = mono-to-stereo; stereo in + mono kernel = both through the same IR;
 * stereo in + stereo kernel = L*IR_L, R*IR_R. Allocates. */
mfo_conv_t *mfo_conv_create(int in_channels, int max_len);
void mfo_conv_destroy(mfo_conv_t *c);
void mfo_conv_reset(mfo_conv_t *c);                   /* silence, time back to 0 */
/* Process n samples (any n). k may be NULL (silence out). Swapping k between calls is allowed
 * (the input history is kept; the new response applies to it at once). out[1] may equal NULL for
 * mono use (then only channel 0 is written). Never allocates. */
void mfo_conv_process(mfo_conv_t *c, const mfo_kernel_t *k, const float *const *in, float *const *out, int n);

/* ----------------------------------------------------------- impulses --- */
/* A float audio buffer owned by the caller (it must outlive its use by the engine). */
typedef struct {
    const float *data[2];        /* data[1] NULL for mono */
    int channels, length;
    float sample_rate;
    float root_note;             /* MIDI note of the recorded pitch (sample sources, keytrack) */
} mfo_buffer_t;

/* Offline impulse shaping (allocates the result in *out_data, channels kept; caller frees with free()):
 * start/length in seconds (length <= 0 = to the end, capped at 6 s), reverse, pitch in semitones
 * (resampled, also converts sample_rate -> target_rate), fade in/out seconds, decay_tilt (-1..1: tilts the
 * decay shorter / longer), damp (0..1: high frequencies die faster along the tail), age (0..1: band loss +
 * slow wow), normalize (1 = unit energy). Returns the new length (0 on failure). */
typedef struct {
    float start, length;
    int reverse;
    float pitch;
    float fade_in, fade_out;
    float decay_tilt, damp, age;
    int normalize;
} mfo_ir_opts_t;
void mfo_ir_opts_default(mfo_ir_opts_t *o);
int mfo_ir_prepare(const mfo_buffer_t *src, float target_rate, const mfo_ir_opts_t *o, float *out_data[2]);

/* Procedural impulses (licence-clean, generated here). Write `len` mono samples at `sr`, seeded. */
enum { MFO_GEN_SPRING, MFO_GEN_PLATE, MFO_GEN_WOOD_BOX, MFO_GEN_BOWED_METAL, MFO_GEN_TAPE_HISS,
       MFO_GEN_FORMANT_VOX, MFO_GEN_METAL_HIT, MFO_GEN_GLASS, MFO_GEN_COUNT };
void mfo_generate_impulse(int type, float *out, int len, float sr, uint32_t seed);
const char *mfo_generate_name(int type);

/* Synth print: render an MFE patch at `note` for `seconds` (note held for `hold` seconds) and resample
 * to `sr`. out_l / out_r hold seconds*sr samples (out_r may be NULL: mono sum). Allocates temporarily
 * (not for the audio thread). Returns the sample count written. */
int mfo_print_synth(const mfe_patch_t *p, int note, int vel, float hold, float seconds, float sr,
                    float *out_l, float *out_r);

/* Keytrack set: the impulse re-pitched for every note lo..hi (per-voice BODY convolution, the IR is
 * capped at max_seconds). Allocates; can be large (about 8 bytes per tap per channel per note). */
typedef struct mfo_keytrack mfo_keytrack_t;
mfo_keytrack_t *mfo_keytrack_create(const mfo_buffer_t *ir, float engine_rate, int lo_note, int hi_note,
                                    float max_seconds, float gain);
void mfo_keytrack_destroy(mfo_keytrack_t *k);

/* ------------------------------------------------------- cross-synthesis --- */
/* STFT matrix for one layer, usable on its own (tests) and inside the engine. Asymmetric low-latency
 * analysis/synthesis windows: analysis frame n (frequency resolution of an n-point window), synthesis
 * window 2*hop at the frame end, latency = 2*hop samples (n=1024, hop=128: 256 samples = 5.3 ms at 48 kHz). */
typedef struct mfo_xsyn mfo_xsyn_t;
typedef struct {
    float tone, grain, pitch;    /* 0..1 */
    float motion;                /* 0..1 (time domain, applied to the output, aligned to the latency) */
    float f0;                    /* played note frequency in Hz for PITCH (<= 0: PITCH passes) */
    int force_stft;              /* 1 = run the STFT even when tone/grain/pitch are 0 (tests) */
} mfo_xsyn_params_t;
mfo_xsyn_t *mfo_xsyn_create(float sample_rate, int n, int hop);
void mfo_xsyn_destroy(mfo_xsyn_t *x);
void mfo_xsyn_reset(mfo_xsyn_t *x);
int mfo_xsyn_latency(const mfo_xsyn_t *x);
/* src: 2 channels in, out: 2 channels (may alias src), mod: mono modulator (may be NULL = silence).
 * n <= MFO_BLOCK and must not cross a multiple of MFO_BLOCK in the running sample count. */
void mfo_xsyn_process(mfo_xsyn_t *x, const mfo_xsyn_params_t *p, const float *const *src, const float *mod,
                      float *const *out, int n);

/* ---------------------------------------------------------------- engine --- */
typedef struct {
    float sample_rate;           /* default 48000 */
    int max_voices;              /* per layer, sample sources (default 16, <= MFO_MAX_VOICES) */
    float max_ir_seconds;        /* longest BODY impulse on the layer bus (default 6) */
    int keytrack_voices;         /* per-voice convolvers per layer (default 8; 0 = no keytrack memory) */
    float keytrack_seconds;      /* longest keytracked impulse (default 1.5) */
    int stft_size, stft_hop;     /* default 1024 / 128 */
    int spectral;                /* 1 = TONE/GRAIN/PITCH/MOTION available (latency 2*hop); 0 = BODY only, zero latency */
    uint32_t seed;
    int live_input;              /* 1 = memory for the live INPUT impulse (BODY on the input, 1.2.0; default 1) */
} mfo_config_t;
void mfo_config_default(mfo_config_t *c);

typedef struct mfo_engine mfo_engine_t;
mfo_engine_t *mfo_engine_create(const mfo_config_t *c);   /* allocates everything; calls mfe_init_tables if needed */
void mfo_engine_destroy(mfo_engine_t *e);
int mfo_latency_samples(const mfo_engine_t *e);           /* constant for the engine's lifetime */
float mfo_latency_ms(const mfo_engine_t *e);
void mfo_engine_reset(mfo_engine_t *e);                   /* silence, all voices off, time 0 */

/* sources and impulses (between blocks; pointers must stay valid while in use) */
enum { MFO_SRC_OFF, MFO_SRC_SAMPLE, MFO_SRC_SYNTH };
void mfo_layer_set_sample(mfo_engine_t *e, int layer, const mfo_buffer_t *b);     /* source = SAMPLE */
void mfo_layer_set_synth(mfo_engine_t *e, int layer, const mfe_patch_t *p);       /* source = SYNTH (patch copied) */
void mfo_layer_set_source_off(mfo_engine_t *e, int layer);
/* impulse of the layer: buffer = the modulator stream for TONE/GRAIN/MOTION/PITCH (static mode),
 * kernel = BODY response (built from the same audio by the caller, may be NULL), keytrack = per-voice set */
void mfo_layer_set_impulse(mfo_engine_t *e, int layer, const mfo_buffer_t *buffer, const mfo_kernel_t *kernel);
void mfo_layer_set_keytrack(mfo_engine_t *e, int layer, const mfo_keytrack_t *k);

/* performance */
void mfo_note_on(mfo_engine_t *e, int note, int vel);
void mfo_note_off(mfo_engine_t *e, int note);
void mfo_all_notes_off(mfo_engine_t *e);
void mfo_pitch_bend(mfo_engine_t *e, float semitones);
void mfo_set_tempo(mfo_engine_t *e, float bpm);
/* render n frames (any n) of stereo float output */
void mfo_render(mfo_engine_t *e, float *left, float *right, int n);
unsigned mfo_active_voices(const mfo_engine_t *e);

/* ---- live INPUT (1.2.0): the host's sidechain audio as a layer's SOURCE and / or IMPULSE ----------------
 * mfo_render_input = mfo_render with n frames of input alongside (in_l / in_r may be NULL = silence; in_r NULL
 * with in_l set = mono). Nothing changes for a layer that does not use the input, so without INPUT the output is
 * identical to mfo_render.
 *  SOURCE INPUT: the layer's bus is the input (instead of its sample / synth voices); gate THRU = always through,
 *    NOTE = through the layer's amp envelope (ATTACK/DECAY/SUSTAIN/RELEASE, VEL SENS) opened by held notes.
 *  IMPULSE INPUT: TONE / GRAIN / PITCH / MOTION use the input (mono) as the modulator; BODY convolves the bus with
 *    the last `window` seconds of the input (0.05..MFO_INPUT_MAX_SECONDS), captured continuously: a new kernel is
 *    built in place every refresh (a few partitions per 64-sample tick), warmed up on a second convolver and then
 *    cross-faded in (MFO_INPUT_XFADE samples), unit energy (quiet input below about -50 dBFS RMS stays quiet).
 *    KEYTRACK does not apply to it. */
#define MFO_INPUT_MAX_SECONDS 1.0f
#define MFO_INPUT_XFADE 2048
void mfo_render_input(mfo_engine_t *e, const float *in_l, const float *in_r, float *left, float *right, int n);
void mfo_layer_set_input(mfo_engine_t *e, int layer, int src_input, int imp_input, int gate_note, float window_seconds);
void mfo_set_input_gain(mfo_engine_t *e, float gain);    /* linear trim applied to the input everywhere (default 1) */
/* samples from a change of the input to the start / end of its effect on a BODY-INPUT kernel (for reporting) */
int mfo_input_body_delay(const mfo_engine_t *e, int layer, int *full);
/* held puck (1.2.0): wobble used while ORBIT PATH is 0 (held); < 0 = ORBIT WOBBLE as in 1.1.0 (default) */
void mfo_set_held_wobble(mfo_engine_t *e, float wobble);
/* freeze the orbit path phase (RATE at its minimum = stopped; default off) */
void mfo_set_orbit_freeze(mfo_engine_t *e, int on);

/* ------------------------------------------------------------ parameters --- */
/* Global parameters, then MFO_LAYERS blocks of layer parameters: id = MFO_LPARAM(layer, MFO_L_xxx). */
enum {
    MFO_P_PUCK_X, MFO_P_PUCK_Y,                  /* -1..1 */
    MFO_P_NODE1_X, MFO_P_NODE1_Y, MFO_P_NODE2_X, MFO_P_NODE2_Y, MFO_P_NODE3_X, MFO_P_NODE3_Y,
    MFO_P_DRY_X, MFO_P_DRY_Y,
    MFO_P_BLEND_SHARP,                           /* inverse-distance exponent 1..6 */
    MFO_P_ORBIT_PATH,                            /* 0 off (puck), 1 circle, 2 ellipse, 3 figure-8 */
    MFO_P_ORBIT_RATE,                            /* Hz, 0.01..20 (free) */
    MFO_P_ORBIT_SYNC,                            /* 0 free, else beats per cycle (1..64) */
    MFO_P_ORBIT_SIZE_X, MFO_P_ORBIT_SIZE_Y,      /* 0..1 radii (circle uses X) */
    MFO_P_ORBIT_WOBBLE,                          /* 0..1 */
    MFO_P_ORBIT_DIR,                             /* 0 clockwise, 1 counter-clockwise */
    MFO_P_ORBIT_ROTATE,                          /* path rotation, turns 0..1 */
    MFO_P_DRIFT,                                 /* 0..1 global slow random walk */
    MFO_P_DRIVE,                                 /* 0..1 master saturation */
    MFO_P_MASTER,                                /* 0..2 gain */
    MFO_P_LFO1_RATE, MFO_P_LFO2_RATE,            /* Hz, internal mod sources */
    MFO_P_GLOBAL_COUNT
};
enum {
    MFO_L_LEVEL,                                 /* 0..2 */
    MFO_L_PAN,                                   /* -1..1 */
    MFO_L_BODY, MFO_L_TONE, MFO_L_GRAIN, MFO_L_MOTION, MFO_L_PITCH,   /* the matrix, 0..1 */
    MFO_L_TUNE,                                  /* semitones -48..48 */
    MFO_L_FINE,                                  /* cents -100..100 */
    MFO_L_ATTACK, MFO_L_DECAY, MFO_L_SUSTAIN, MFO_L_RELEASE,          /* s, s, 0..1, s */
    MFO_L_VEL_SENS,                              /* 0..1 */
    MFO_L_FILTER_TYPE,                           /* 0 off, 1 ladder LP, 2 SVF LP, 3 SVF BP, 4 SVF HP */
    MFO_L_CUTOFF,                                /* Hz 20..20000 (log) */
    MFO_L_RESO, MFO_L_FDRIVE,                    /* 0..1 */
    MFO_L_FENV_AMT,                              /* -1..1 = +-6 octaves */
    MFO_L_FENV_A, MFO_L_FENV_D, MFO_L_FENV_S, MFO_L_FENV_R,
    MFO_L_MOD_SOURCE,                            /* 0 = own impulse (static), 1..3 = LIVE layer 1..3 */
    MFO_L_SCAN_RATE,                             /* 0..4 (0 = freeze at SCAN_POS) */
    MFO_L_SCAN_POS,                              /* 0..1 start / freeze position in the impulse */
    MFO_L_SCAN_LOOP,                             /* 0 one-shot, 1 loop */
    MFO_L_IR_PITCH,                              /* semitones -24..24: impulse stream pitch */
    MFO_L_KEYTRACK,                              /* 0 bus BODY, 1 per-voice BODY (keytrack set) */
    MFO_L_START, MFO_L_VEL_START,                /* sample start 0..1, velocity -> start 0..1 */
    MFO_L_LOOP, MFO_L_LOOP_START, MFO_L_LOOP_END, MFO_L_XFADE,        /* 0/1, 0..1, 0..1, 0..0.5 of the loop */
    MFO_L_REVERSE,                               /* 0/1 */
    MFO_L_STRETCH,                               /* 0.25..4 time stretch (1 = plain playback) */
    MFO_L_TAPE,                                  /* 0/1 tape mode */
    MFO_L_WOW, MFO_L_FLUTTER, MFO_L_BUMP, MFO_L_HISS,                 /* 0..1 */
    MFO_L_COUNT
};
#define MFO_LPARAM(layer, p) (MFO_P_GLOBAL_COUNT + (layer) * MFO_L_COUNT + (p))
#define MFO_PARAM_COUNT (MFO_P_GLOBAL_COUNT + MFO_LAYERS * MFO_L_COUNT)

#define MFO_PF_INT 1u            /* stepped: applied at once, not smoothed or modulated in between */
#define MFO_PF_LOG 2u            /* modulation in octaves of the range (cutoff, rates) */
typedef struct { const char *name; float min, max, def; unsigned flags; } mfo_param_info_t;
const mfo_param_info_t *mfo_param_info(int id);       /* layer params named "L1 BODY" etc. */
void mfo_set_param(mfo_engine_t *e, int id, float value);    /* clamped; any thread between blocks */
float mfo_get_param(const mfo_engine_t *e, int id);          /* the set (unmodulated) value */
float mfo_get_param_effective(const mfo_engine_t *e, int id); /* smoothed + modulated, as rendered */

/* ------------------------------------------------------------ mod matrix --- */
enum {
    MFO_MS_NONE,
    MFO_MS_MACRO1, MFO_MS_MACRO2, MFO_MS_MACRO3, MFO_MS_MACRO4,
    MFO_MS_MODWHEEL, MFO_MS_AFTERTOUCH,
    MFO_MS_VELOCITY, MFO_MS_KEY,                 /* last note: velocity 0..1, key (note-60)/60 */
    MFO_MS_LFO1, MFO_MS_LFO2,                    /* internal sines, -1..1 */
    MFO_MS_ORBIT_X, MFO_MS_ORBIT_Y,              /* the puck position actually used */
    MFO_MS_EXT1, MFO_MS_EXT2, MFO_MS_EXT3, MFO_MS_EXT4,   /* fed by the host (Performer, Stepper, envelopes) */
    MFO_MS_EXT5, MFO_MS_EXT6, MFO_MS_EXT7, MFO_MS_EXT8,
    MFO_MS_COUNT
};
typedef struct { uint8_t src; uint16_t dst; float amount; } mfo_mod_t;   /* amount -1..1 = +- the full range */
void mfo_set_mod(mfo_engine_t *e, int slot, int src, int dst, float amount);
void mfo_set_mod_source(mfo_engine_t *e, int src, float value);   /* host-fed sources (macros, ext, wheel) */

/* the blender, for user interfaces and tests: weights of L1, L2, L3, DRY for a puck position */
void mfo_blend_weights(const mfo_engine_t *e, float x, float y, float w[4]);
void mfo_orbit_position(const mfo_engine_t *e, float *x, float *y);   /* puck as last rendered */

#ifdef __cplusplus
}
#endif
#endif
