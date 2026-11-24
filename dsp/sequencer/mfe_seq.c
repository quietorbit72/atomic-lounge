/* SPDX-License-Identifier: AGPL-3.0-only
 * Vintage Chonk mono step sequencer (dsp/include/mfe_seq.h). Integer only, no allocation, no libc calls
 * (the FM1 runs it in the audio interrupt from internal RAM: Build-MFM1.ps1 "sram" unit).
 *
 * Clock model. One MIDI clock tick (1/24 beat) is U = 2^20 position units. The sequencer walks through
 * pairs of steps (a pair = 2 x 6 ticks at 1/16): the first step of a pair starts at 0, the second at
 * b1 = pair x swing / 100, the next pair at pair_len. Every MFE_CTL samples the position moves on by the
 * tempo's exact rational increment (internal clock), or by the measured MIDI clock rate, never past the
 * next MIDI tick that has not arrived yet (a flywheel at most one tick ahead; a late tick snaps it).
 * Events inside a control period fire at its start: at most 32 samples early.
 * Only what the audio side runs is here; the control-task calls are in mfe_seq_edit.c (Flash on the FM1). */
#include "mfe_seq.h"

#define U (1u << 20)
#define NONE 0xFFFFFFFFu
#define NO_NOTE 0xFFu
#define INT_DEN 55125u               /* (BPM << 8) << 16 / 55125 = Q20 ticks per MFE_CTL samples at 44.1 kHz */
_Static_assert(MFE_SR == 44100 && MFE_CTL == 32, "INT_DEN assumes 44.1 kHz and 32-sample control ticks");
_Static_assert(sizeof(mfe_seq_pattern_t) == 132, "patterns are stored byte for byte");

static const uint8_t TPS[MFE_SEQ_RATE_COUNT] = {6, 12, 3};    /* ticks per step: 1/16, 1/8, 1/32 */

static unsigned clampu(unsigned v, unsigned lo, unsigned hi) { return v < lo ? lo : v > hi ? hi : v; }

static void set_tempo(mfe_seq_t *s, uint32_t q8)
{
    q8 = clampu(q8, MFE_SEQ_TEMPO_MIN << 8, MFE_SEQ_TEMPO_MAX << 8);
    s->tempo_q8 = q8;
    s->inc_q = (q8 << 16) / INT_DEN;           /* fits: 240 << 24 < 2^32 */
    s->inc_r = (q8 << 16) % INT_DEN;
}

uint32_t mfe_seq_tempo_q8(const mfe_seq_t *s)
{
    if (s->ext && s->ext_ivl_q4) return 451584000u / s->ext_ivl_q4;   /* 60 * 44100 * 256 * 16 / 24 / ivl_q4 */
    return s->tempo_q8;
}

/* ------------------------------------------------------------ output --- */
static void emit(mfe_seq_t *s, mfe_engine_t *e, uint8_t type, uint8_t note, uint8_t vel, uint8_t from)
{
    if (e) {
        if (type == MFE_SEQ_EV_ON) mfe_note_on(e, note, vel);
        else if (type == MFE_SEQ_EV_OFF) mfe_note_off(e, note);
        else mfe_note_slide(e, from, note, vel);
    }
    if (s->log && s->log_n < s->log_cap) {
        mfe_seq_event_t *v = &s->log[s->log_n++];
        v->t = s->now; v->type = type; v->note = note; v->vel = vel; v->from = from;
    }
}

static void release(mfe_seq_t *s, mfe_engine_t *e)
{
    if (s->sounding != NO_NOTE) emit(s, e, MFE_SEQ_EV_OFF, s->sounding, 0, 0);
    s->sounding = NO_NOTE;
    s->off_at = NONE;
}

/* the current pattern's rate and swing, read at each pair start (changes never tear a pair) */
static void pair_setup(mfe_seq_t *s)
{
    const mfe_seq_pattern_t *p = &s->pat[s->slot];
    unsigned r = p->rate < MFE_SEQ_RATE_COUNT ? p->rate : 0;
    s->pair_len = 2u * TPS[r] * U;
    s->b1 = s->pair_len * clampu(p->swing, 50, 75) / 100u;     /* <= 24 x 2^20 x 75: fits 32 bits */
}

/* a step boundary at `pos`: step `nxt` starts (s->half, step_start, step_len already set) */
static void boundary(mfe_seq_t *s, mfe_engine_t *e)
{
    if (s->nxt >= clampu(s->pat[s->slot].length, 1, MFE_SEQ_STEPS)) s->nxt = 0;   /* the pattern got shorter */
    if (s->nxt == 0 && s->cue != s->slot && s->cue < MFE_SEQ_SLOTS) {     /* the cued pattern starts here */
        s->slot = s->cue;
        if (!s->half) { pair_setup(s); s->step_len = s->b1; }
    }
    const mfe_seq_pattern_t *p = &s->pat[s->slot];
    unsigned len = clampu(p->length, 1, MFE_SEQ_STEPS);
    unsigned idx = s->nxt < len ? s->nxt : 0;
    mfe_seq_step_t st = p->step[idx], next = p->step[(idx + 1) % len];
    s->cur = (int8_t)idx;
    s->nxt = (uint8_t)((idx + 1) % len);
    unsigned g = mfe_seq_gate(st);
    uint8_t slide_prev = s->slide_in;
    s->off_at = NONE;
    if (g == MFE_SEQ_GATE_REST) { release(s, e); s->slide_in = 0; return; }
    if (g == MFE_SEQ_GATE_TIE) {                     /* hold what sounds (after a rest: a rest) */
        s->slide_in = s->sounding != NO_NOTE && mfe_seq_slide(st);
        return;
    }
    int n = (int)mfe_seq_note(st) + s->transpose;
    uint8_t note = (uint8_t)(n < 0 ? 0 : n > 127 ? 127 : n);
    uint8_t vel = mfe_seq_accent(st) ? MFE_SEQ_VEL_ACCENT : MFE_SEQ_VEL;
    if (s->sounding != NO_NOTE && slide_prev) {      /* slid into: legato with glide */
        if (s->sounding != note) emit(s, e, MFE_SEQ_EV_SLIDE, note, vel, s->sounding);
    } else {
        release(s, e);
        emit(s, e, MFE_SEQ_EV_ON, note, vel, 0);
    }
    s->sounding = note;
    s->slide_in = mfe_seq_slide(st);
    if (!mfe_seq_slide(st) && mfe_seq_gate(next) != MFE_SEQ_GATE_TIE && g < 100)
        s->off_at = s->step_start + s->step_len * g / 100u;       /* <= 18 x 2^20 x 99: fits 32 bits */
}

/* move to `target` (relative to this pair), firing every event before it (or at it, when `incl`) */
static void advance(mfe_seq_t *s, mfe_engine_t *e, uint32_t target, int incl)
{
    for (;;) {
        if (s->pending) { s->pending = 0; boundary(s, e); continue; }
        uint32_t nb = s->half ? s->pair_len : s->b1;
        uint32_t ev = s->off_at != NONE && s->off_at <= nb ? s->off_at : nb;
        if (ev < target || (incl && ev == target)) {
            s->ticks += (ev >> 20) - (s->pos >> 20);
            s->pos = ev;
            if (ev == s->off_at) { release(s, e); continue; }
            if (s->half) {                           /* next pair */
                s->pos = 0;
                target -= s->pair_len;
                s->ext_next -= s->pair_len;
                pair_setup(s);
                s->half = 0;
                s->step_start = 0;
                s->step_len = s->b1;
            } else {
                s->half = 1;
                s->step_start = s->b1;
                s->step_len = s->pair_len - s->b1;
            }
            boundary(s, e);
            continue;
        }
        s->ticks += (target >> 20) - (s->pos >> 20);
        s->pos = target;
        return;
    }
}

static void stop(mfe_seq_t *s, mfe_engine_t *e)
{
    release(s, e);
    s->running = 0;
    s->slide_in = 0;
    s->cur = -1;
    s->ext_wait = 0;
}

static void start(mfe_seq_t *s, mfe_engine_t *e)
{
    release(s, e);
    s->slot = s->cue < MFE_SEQ_SLOTS ? s->cue : 0;
    pair_setup(s);
    s->pos = 0; s->half = 0; s->nxt = 0; s->pending = 1; s->slide_in = 0;
    s->step_start = 0; s->step_len = s->b1;
    s->rem = 0;
    s->ticks = 1;
    s->starts++;
    s->running = 1;
    s->ext_wait = s->ext;                            /* on MIDI clock: step 1 at the next tick */
}

/* Song Position Pointer (stopped): sixteenths from the song start */
static void locate(mfe_seq_t *s, unsigned spp)
{
    const mfe_seq_pattern_t *p = &s->pat[s->slot];
    unsigned len = clampu(p->length, 1, MFE_SEQ_STEPS);
    pair_setup(s);
    uint32_t t = (uint32_t)spp * 6u, pt = s->pair_len >> 20;
    uint32_t pairs = t / pt;
    s->pos = (t % pt) << 20;
    s->half = s->pos >= s->b1;
    s->step_start = s->half ? s->b1 : 0;
    s->step_len = s->half ? s->pair_len - s->b1 : s->b1;
    uint32_t started = pairs * 2u + 1u + s->half;    /* steps begun at or before pos */
    s->pending = s->pos == 0 || s->pos == s->b1;
    s->nxt = (uint8_t)((s->pending ? started - 1u : started) % len);
    s->cur = -1;
}

void mfe_seq_midi_rt(mfe_seq_t *s, uint8_t st, uint8_t d1, uint8_t d2)
{
    if (!s->clock_in) return;
    uint32_t now = s->now;
    if (st == 0xF8) {
        /* tempo: the mean interval over up to 24 ticks (arrival times are quantised to audio blocks) */
        if (!s->ext_have || (uint32_t)(now - s->ext_last) > MFE_SEQ_CLOCK_TIMEOUT) {
            s->ext_t0 = now; s->ext_n = 0; s->ext_ivl_q4 = 0;
        } else {
            s->ext_n++;
            uint32_t v = ((now - s->ext_t0) << 4) / s->ext_n;
            if ((!s->ext_ivl_q4 || s->ext_n >= 6) && v >= (64u << 4)) s->ext_ivl_q4 = v;   /* <= 1000 BPM */
            if (s->ext_n >= 24) { s->ext_t0 = now; s->ext_n = 0; }
        }
        s->ext_have = 1;
        s->ext_last = now;
        if (s->rx_ticks < 255) s->rx_ticks++;
    } else if (st == 0xFA || st == 0xFB || st == 0xFC) {
        s->rx_transport = st;
        s->rx_ticks = 0;                             /* a tick before Start belongs to the old run */
        if (!s->ext_have) { s->ext_have = 1; s->ext_t0 = now; s->ext_n = 0; }
        s->ext_last = now;
    } else if (st == 0xF2) {
        s->rx_spp = (uint16_t)(1u + ((d1 & 127u) | (d2 & 127u) << 7));
    }
}

static void commands(mfe_seq_t *s, mfe_engine_t *e)
{
    while (s->cmd_r != s->cmd_w) {
        uint8_t type = s->cmd[s->cmd_r & 15u].type, a = s->cmd[s->cmd_r & 15u].a;
        int16_t b = s->cmd[s->cmd_r & 15u].b;
        s->cmd_r = (uint8_t)(s->cmd_r + 1u);
        switch (type) {
        case MFE_SEQ_CMD_START: start(s, e); break;
        case MFE_SEQ_CMD_STOP: stop(s, e); break;
        case MFE_SEQ_CMD_CONTINUE:
            if (!s->running) { s->running = 1; s->ext_wait = s->ext; if (s->ext) s->ext_next = s->pos; if (!s->pair_len) start(s, e); }
            break;
        case MFE_SEQ_CMD_TEMPO: set_tempo(s, (uint16_t)b); break;
        case MFE_SEQ_CMD_SLOT:
            if (a < MFE_SEQ_SLOTS) { s->cue = a; if (!s->running) { s->slot = a; s->nxt = 0; } }
            break;
        case MFE_SEQ_CMD_TRANSPOSE: s->transpose = (int8_t)(b < -48 ? -48 : b > 48 ? 48 : b); break;
        case MFE_SEQ_CMD_RECORD: {
            if (!s->running || s->cur < 0) break;
            uint32_t into = s->pos - s->step_start;
            unsigned idx = into < s->step_len / 2u ? (unsigned)s->cur : s->nxt;   /* quantise to the nearest step */
            mfe_seq_step_t *st = &s->pat[s->slot].step[idx % MFE_SEQ_STEPS];
            st->note = (uint8_t)((st->note & MFE_SEQ_ACCENT) | (a & 127u));
            unsigned g = mfe_seq_gate(*st);
            if (g == MFE_SEQ_GATE_REST || g == MFE_SEQ_GATE_TIE) st->gate = (uint8_t)((st->gate & MFE_SEQ_SLIDE) | 50u);
            break;
        }
        case MFE_SEQ_CMD_CLOCK_IN: s->clock_in = a != 0; if (!a) { s->ext = 0; s->ext_wait = 0; } break;
        default: break;
        }
    }
}

void mfe_seq_ctl(mfe_seq_t *s, mfe_engine_t *e)
{
    commands(s, e);
    /* MIDI clock received since the last tick (the audio interrupt delivers it at block starts) */
    if (s->rx_transport) {
        uint8_t t = s->rx_transport;
        s->rx_transport = 0;
        s->ext = 1;
        if (t == 0xFC) stop(s, e);
        else if (t == 0xFA) start(s, e);
        else if (!s->running) { s->running = 1; s->ext_wait = 1; if (!s->pair_len) start(s, e); }
    }
    if (s->rx_spp) {
        if (!s->running) locate(s, s->rx_spp - 1u);
        s->rx_spp = 0;
    }
    if (s->rx_ticks) {
        uint8_t n = s->rx_ticks;
        s->rx_ticks = 0;
        if (!s->ext && s->running) { s->ext_next = s->pos; }   /* clock appeared mid-run: follow it from here */
        s->ext = 1;
        while (n-- && s->running) {
            if (s->ext_wait) { s->ext_wait = 0; s->ext_next = s->pos; }
            if (s->ext_next >= s->pos) advance(s, e, s->ext_next, 1);   /* a late tick snaps the position */
            s->ext_next += U;
        }
    }
    if (s->ext && (uint32_t)(s->now - s->ext_last) > MFE_SEQ_CLOCK_TIMEOUT) {   /* the clock went away */
        s->ext = 0;
        s->ext_wait = 0;
    }
    if (s->running && !s->ext_wait) {
        uint32_t target;
        int incl = 0;
        if (s->ext) {
            uint32_t inc = s->ext_ivl_q4 ? (1u << 29) / s->ext_ivl_q4 : s->inc_q;   /* 32 x 2^20 / ivl */
            target = s->pos + inc;
            if (target >= s->ext_next) { target = s->ext_next; incl = 1; }   /* at most up to the next tick */
        } else {
            target = s->pos + s->inc_q;
            s->rem += s->inc_r;
            if (s->rem >= INT_DEN) { s->rem -= INT_DEN; target++; }
        }
        advance(s, e, target, incl);
    }
    s->now += MFE_CTL;
}
