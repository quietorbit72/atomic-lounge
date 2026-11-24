/* SPDX-License-Identifier: AGPL-3.0-only
 * Vintage Chonk mono step sequencer (17 Nov 2026: "Mono step sequencer"): a classic 303 / SH-101 style
 * pattern sequencer for basses and acid lines.
 *
 *   pattern   1..64 steps; per step a note, a gate (REST, 1..100 % of the step, or TIE), ACCENT and SLIDE
 *   clock     internal 40..240 BPM with swing 50..75 %, rate 1/16 (default), 1/8 or 1/32;
 *             or MIDI clock in (24 ppqn, Start / Stop / Continue, Song Position), followed automatically
 *   play      note on / off into the engine (mfe_note_on / mfe_note_off); ACCENT = velocity 127 (else 90);
 *             SLIDE = the note is held into the next step, which is played legato with glide (mfe_note_slide)
 *   transpose a held key transposes the pattern relative to its first note (as on classic monosynths)
 *
 * Portable C99, integers only, no allocation. Timing: mfe_seq_ctl() advances the clock by MFE_CTL samples;
 * the engine calls it at every control tick inside mfe_render (mfe_engine_attach_seq), so a step starts
 * within 32 samples (0.73 ms) of its exact time, whatever the rest of the system is doing. The internal
 * clock is exact (Bresenham remainder): no drift however long it runs.
 *
 * Threads (the FM1 firmware): mfe_seq_ctl and mfe_seq_midi_rt run in the audio interrupt; the control task
 * talks to it only through mfe_seq_cmd (a single-producer ring) and by writing pattern bytes directly
 * (single-byte writes: a step half-written for one block is harmless; the sequencer releases the note it
 * started by its own record, never by reading the pattern again, so no edit can leave a note stuck). */
#ifndef MFE_SEQ_H
#define MFE_SEQ_H
#include <stdint.h>
#include "mfe.h"

#define MFE_SEQ_STEPS 64
#define MFE_SEQ_SLOTS 8
#define MFE_SEQ_GATE_REST 0          /* gate byte (low 7 bits): 0 = rest, 1..100 = gate %, 127 = tie */
#define MFE_SEQ_GATE_TIE 127
#define MFE_SEQ_SLIDE 0x80           /* gate byte bit 7 */
#define MFE_SEQ_ACCENT 0x80          /* note byte bit 7 */
#define MFE_SEQ_VEL 90               /* velocity of a plain step */
#define MFE_SEQ_VEL_ACCENT 127
#define MFE_SEQ_TEMPO_MIN 40
#define MFE_SEQ_TEMPO_MAX 240
#define MFE_SEQ_CLOCK_TIMEOUT 22050u /* samples (0.5 s) without MIDI clock: the internal clock takes over */

enum { MFE_SEQ_RATE_16, MFE_SEQ_RATE_8, MFE_SEQ_RATE_32, MFE_SEQ_RATE_COUNT };

typedef struct { uint8_t note, gate; } mfe_seq_step_t;   /* note | ACCENT, gate | SLIDE */
typedef struct {
    uint8_t length;              /* 1..64 */
    uint8_t rate;                /* MFE_SEQ_RATE_* */
    uint8_t swing;               /* 50..75: where the second step of each pair starts, in % of the pair */
    uint8_t reserved;
    mfe_seq_step_t step[MFE_SEQ_STEPS];
} mfe_seq_pattern_t;             /* 132 bytes, byte-only (stored as is in Flash) */

#define mfe_seq_note(st) ((st).note & 127u)
#define mfe_seq_accent(st) (((st).note & MFE_SEQ_ACCENT) != 0)
#define mfe_seq_slide(st) (((st).gate & MFE_SEQ_SLIDE) != 0)
#define mfe_seq_gate(st) ((st).gate & 127u)

/* commands from the control task (mfe_seq_cmd) */
enum {
    MFE_SEQ_CMD_START = 1,       /* from step 1 (on MIDI clock: at the next clock tick) */
    MFE_SEQ_CMD_STOP,            /* stop; the sounding note is released */
    MFE_SEQ_CMD_CONTINUE,        /* go on from where it stopped */
    MFE_SEQ_CMD_TEMPO,           /* b = BPM << 8 (40..240) */
    MFE_SEQ_CMD_SLOT,            /* a = pattern slot 0..7: now when stopped, else at the end of the pattern */
    MFE_SEQ_CMD_TRANSPOSE,       /* b = semitones (-48..48), 0 = off */
    MFE_SEQ_CMD_RECORD,          /* a = note: written to the step played now, or the next one (quantised) */
    MFE_SEQ_CMD_CLOCK_IN,        /* a = 1 follow MIDI clock automatically (default), 0 ignore it */
};

/* event log (tests, tools): every note the sequencer sends, with its sample time */
enum { MFE_SEQ_EV_ON = 1, MFE_SEQ_EV_OFF, MFE_SEQ_EV_SLIDE };
typedef struct { uint32_t t; uint8_t type, note, vel, from; } mfe_seq_event_t;

typedef struct mfe_seq {
    mfe_seq_pattern_t pat[MFE_SEQ_SLOTS];
    /* ----- read by the UI (written by the audio side) ----- */
    volatile uint8_t running, slot, ext;     /* ext: following MIDI clock */
    volatile int8_t cur;                     /* step sounding now (-1 none) */
    volatile uint8_t cue;                    /* slot to play next */
    volatile uint32_t ticks;                 /* 24 ppqn ticks played since start (clock out, beat LED) */
    volatile uint32_t starts;                /* incremented at each start (clock out sends FA) */
    /* ----- private (audio side) ----- */
    uint8_t nxt, half, pending, clock_in, sounding, slide_in, ext_wait;
    int8_t transpose;
    uint32_t pos, pair_len, b1, off_at;      /* Q20 ticks within the current pair of steps; off_at ~0 = none */
    uint32_t step_start, step_len;
    uint32_t inc_q, inc_r, rem;              /* internal clock per control tick: inc_q + inc_r / den */
    uint32_t tempo_q8, ext_ivl_q4;           /* internal tempo; measured MIDI clock interval (samples << 4) */
    uint32_t ext_next;                       /* MIDI clock: position of the next tick (the flywheel stops there) */
    uint32_t now, ext_last, ext_t0;          /* sample clock; time of the last tick; start of the estimate */
    uint16_t ext_n, rx_spp;                  /* ticks in the estimate; Song Position + 1 received (0 none) */
    uint8_t ext_have, rx_ticks, rx_transport;   /* received since the last control tick */
    uint8_t cmd_w, cmd_r;
    struct { uint8_t type, a; int16_t b; } cmd[16];
    mfe_seq_event_t *log;                    /* optional (NULL on the device) */
    uint32_t log_n, log_cap;
} mfe_seq_t;

void mfe_seq_init(mfe_seq_t *s);                         /* stopped, 120 BPM, every slot an empty pattern */
void mfe_seq_pattern_clear(mfe_seq_pattern_t *p);        /* 16 steps of rest, 1/16, no swing */
int mfe_seq_pattern_valid(const mfe_seq_pattern_t *p);   /* 1 = every field in range (Flash load check) */
/* control task -> audio side; 0 queued, -1 ring full */
int mfe_seq_cmd(mfe_seq_t *s, uint8_t type, uint8_t a, int16_t b);
/* audio side: one MIDI realtime / song-position message (F8 FA FB FC, F2 lsb msb) */
void mfe_seq_midi_rt(mfe_seq_t *s, uint8_t status, uint8_t d1, uint8_t d2);
/* audio side: advance MFE_CTL samples; notes go to `e` (may be NULL: log only) */
void mfe_seq_ctl(mfe_seq_t *s, mfe_engine_t *e);
uint32_t mfe_seq_tempo_q8(const mfe_seq_t *s);           /* the tempo playing: internal, or measured MIDI clock */

/* engine side (dsp/voices/mfe_engine.c): the engine calls mfe_seq_ctl at each control tick */
void mfe_engine_attach_seq(mfe_engine_t *e, mfe_seq_t *s);
/* legato move of a held note to `to` with glide (the patch glide, or about 60 ms when it has none):
 * the 303 slide. Falls back to a note on when `from` is not held. */
void mfe_note_slide(mfe_engine_t *e, uint8_t from, uint8_t to, uint8_t vel);
#define MFE_SLIDE_TIME_Q15 14256     /* 0.5 ms * 60000^p = about 60 ms */
#endif
