/* SPDX-License-Identifier: AGPL-3.0-only
 * Vintage Chonk step sequencer tests (dsp/sequencer/mfe_seq.c): timing exactness at several tempos and
 * rates, swing, gate / tie / slide / accent output, MIDI clock follow (start, stop, continue, song position,
 * tempo changes, clock loss), transpose, record, pattern length wrap, pattern change, and no stuck notes. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "mfe.h"
#include "mfe_seq.h"

static int fails, checks;
#define CHECK(c, ...) do { checks++; if (!(c)) { fails++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

static mfe_seq_t S;
static mfe_seq_event_t LOG[20000];

static void fresh(void)
{
    mfe_seq_init(&S);
    S.log = LOG;
    S.log_cap = sizeof LOG / sizeof LOG[0];
    S.log_n = 0;
}
static void set_step(unsigned slot, unsigned i, unsigned note, unsigned gate, int accent, int slide)
{
    S.pat[slot].step[i].note = (uint8_t)(note | (accent ? MFE_SEQ_ACCENT : 0));
    S.pat[slot].step[i].gate = (uint8_t)(gate | (slide ? MFE_SEQ_SLIDE : 0));
}
static void fill(unsigned slot, unsigned len, unsigned gate)
{
    S.pat[slot].length = (uint8_t)len;
    for (unsigned i = 0; i < len; i++) set_step(slot, i, 36 + i % 12, gate, 0, 0);
}
static void run(unsigned ctl) { for (unsigned i = 0; i < ctl; i++) mfe_seq_ctl(&S, 0); }
static void cmd(uint8_t t, uint8_t a, int16_t b) { mfe_seq_cmd(&S, t, a, b); }
static void tempo(double bpm) { cmd(MFE_SEQ_CMD_TEMPO, 0, (int16_t)(uint16_t)lround(bpm * 256.0)); }

/* events of one type in order */
static int nth(int type, int k)
{
    for (uint32_t i = 0; i < S.log_n; i++) if (LOG[i].type == type && k-- == 0) return (int)i;
    return -1;
}
static int count(int type) { int n = 0; for (uint32_t i = 0; i < S.log_n; i++) n += LOG[i].type == type; return n; }
/* notes on minus notes off (a slide moves the held note, it neither starts nor ends one) */
static int balance(void) { return count(MFE_SEQ_EV_ON) - count(MFE_SEQ_EV_OFF); }

/* starts must fall in the control period that contains their exact time: 0 <= exact - t < 32 samples */
static void check_timing(double bpm, unsigned rate, unsigned steps, const char *what)
{
    static const double SPB[3] = {4, 2, 8};       /* steps per beat */
    fresh();
    fill(0, 16, 50);
    S.pat[0].rate = (uint8_t)rate;
    tempo(bpm);
    cmd(MFE_SEQ_CMD_START, 0, 0);
    double bq = lround(bpm * 256.0) / 256.0, step = 60.0 * MFE_SR / (bq * SPB[rate]);
    run((unsigned)(step * steps / MFE_CTL) + 2);
    double worst = 0, worst_early = 0;
    int ok = 1;
    for (unsigned k = 0; k < steps; k++) {
        int i = nth(MFE_SEQ_EV_ON, (int)k);
        if (i < 0) { ok = 0; break; }
        double d = k * step - LOG[i].t;              /* how early it fired */
        if (d > worst) worst = d;
        if (d < worst_early) worst_early = d;
    }
    CHECK(ok && worst < MFE_CTL + 0.01 && worst_early > -0.01, "%s: %u steps within one control period (early by up to %.2f, late by %.2f samples)",
          what, steps, worst, -worst_early);
    /* the gate: off at 50 % of the step, within a control period */
    int on = nth(MFE_SEQ_EV_ON, 7), off = nth(MFE_SEQ_EV_OFF, 7);
    double d = (7 + 0.5) * step - LOG[off].t;
    CHECK(on >= 0 && off >= 0 && d >= -0.01 && d < MFE_CTL + 0.01, "%s: gate 50 %% off on time (%.2f)", what, d);
}

int main(void)
{
    static uint8_t engine_mem[512 * 1024] __attribute__((aligned(16)));
    mfe_init_tables();
    CHECK(sizeof(mfe_seq_t) < 1400, "sequencer state %zu B (8 patterns included)", sizeof(mfe_seq_t));

    /* ---- timing exactness: several tempos and the three rates, hundreds of steps (no drift) ---- */
    check_timing(120, MFE_SEQ_RATE_16, 512, "120 BPM 1/16");
    check_timing(40, MFE_SEQ_RATE_16, 128, "40 BPM 1/16");
    check_timing(240, MFE_SEQ_RATE_16, 1024, "240 BPM 1/16");
    check_timing(133.33, MFE_SEQ_RATE_16, 700, "133.33 BPM 1/16");
    check_timing(97, MFE_SEQ_RATE_8, 300, "97 BPM 1/8");
    check_timing(174, MFE_SEQ_RATE_32, 1500, "174 BPM 1/32");
    {   /* a long run: 10 minutes at 128 BPM, the last step still inside its control period */
        fresh();
        fill(0, 16, 50);
        tempo(128);
        cmd(MFE_SEQ_CMD_START, 0, 0);
        double step = 60.0 * MFE_SR / (128.0 * 4);
        unsigned n = 0;
        int ok = 1;
        for (unsigned c = 0; c < 600u * MFE_SR / MFE_CTL; c++) {
            uint32_t before = S.log_n;
            mfe_seq_ctl(&S, 0);
            for (uint32_t i = before; i < S.log_n; i++)
                if (LOG[i].type == MFE_SEQ_EV_ON) {
                    double d = n * step - LOG[i].t;
                    if (d < -0.01 || d >= MFE_CTL + 0.01) ok = 0;
                    n++;
                }
            S.log_n = 0;
        }
        CHECK(ok && n == 5120, "10 minutes at 128 BPM: %u steps, none off its control period", n);
    }

    /* ---- swing ---- */
    {
        fresh();
        fill(0, 16, 50);
        S.pat[0].swing = 66;
        tempo(120);
        cmd(MFE_SEQ_CMD_START, 0, 0);
        run(4000);
        double pair = 2 * 60.0 * MFE_SR / (120.0 * 4);
        int ok = 1;
        for (int k = 0; k < 16; k++) {
            double exact = (k / 2) * pair + (k & 1 ? pair * 0.66 : 0);
            double d = exact - LOG[nth(MFE_SEQ_EV_ON, k)].t;
            if (d < -0.01 || d >= MFE_CTL + 0.01) ok = 0;
        }
        CHECK(ok, "swing 66 %%: odd steps at 66 %% of the pair");
        fresh();
        fill(0, 16, 50);
        S.pat[0].swing = 75;
        cmd(MFE_SEQ_CMD_START, 0, 0);
        run(400);
        double d1 = LOG[nth(MFE_SEQ_EV_ON, 1)].t - LOG[nth(MFE_SEQ_EV_ON, 0)].t;
        double d2 = LOG[nth(MFE_SEQ_EV_ON, 2)].t - LOG[nth(MFE_SEQ_EV_ON, 1)].t;
        CHECK(fabs(d1 / (d1 + d2) - 0.75) < 0.02, "swing 75 %%: %.0f / %.0f", d1, d2);
    }

    /* ---- gate, rest, tie, slide, accent ---- */
    {
        fresh();
        S.pat[0].length = 8;
        set_step(0, 0, 36, 25, 1, 0);                     /* accent, short gate */
        set_step(0, 1, 38, MFE_SEQ_GATE_REST, 0, 0);
        set_step(0, 2, 40, 50, 0, 0);
        set_step(0, 3, 40, MFE_SEQ_GATE_TIE, 0, 0);       /* holds step 3's note through step 4 */
        set_step(0, 4, 43, 60, 0, 1);                     /* slide into step 5 */
        set_step(0, 5, 48, 50, 0, 0);
        set_step(0, 6, 36, 100, 0, 0);                    /* full gate: released when step 7 starts */
        set_step(0, 7, 36, 50, 0, 0);
        tempo(120);
        cmd(MFE_SEQ_CMD_START, 0, 0);
        double st = 60.0 * MFE_SR / (120.0 * 4);
        run((unsigned)(st * 8 / MFE_CTL));
        const mfe_seq_event_t *e0 = &LOG[nth(MFE_SEQ_EV_ON, 0)];
        CHECK(e0->note == 36 && e0->vel == MFE_SEQ_VEL_ACCENT, "accent = velocity 127 (%d)", e0->vel);
        CHECK(LOG[nth(MFE_SEQ_EV_ON, 1)].vel == MFE_SEQ_VEL, "plain step = velocity 90");
        double off0 = LOG[nth(MFE_SEQ_EV_OFF, 0)].t;
        CHECK(fabs(off0 - 0.25 * st) < MFE_CTL, "gate 25 %%: off at %.0f of %.0f", off0, st);
        int on1 = nth(MFE_SEQ_EV_ON, 1);
        CHECK(LOG[on1].note == 40 && LOG[on1].t >= 2 * st - MFE_CTL, "rest: nothing on step 2; step 3 plays 40");
        /* tie: 40 is not released at 50 % of step 3, not restarted at step 4 */
        int off1 = nth(MFE_SEQ_EV_OFF, 1);
        CHECK(LOG[off1].note == 40 && LOG[off1].t >= 4 * st - MFE_CTL, "tie: 40 held to the end of step 4 (%u)", LOG[off1].t);
        int on2 = nth(MFE_SEQ_EV_ON, 2);
        CHECK(LOG[on2].note == 43 && fabs(LOG[on2].t - 4 * st) < MFE_CTL, "step 5 plays 43 after the tie");
        int sl = nth(MFE_SEQ_EV_SLIDE, 0);
        CHECK(sl >= 0 && LOG[sl].from == 43 && LOG[sl].note == 48 && fabs(LOG[sl].t - 5 * st) < MFE_CTL,
              "slide: 43 -> 48 legato at step 6");
        int off2 = nth(MFE_SEQ_EV_OFF, 2);
        CHECK(LOG[off2].note == 48 && fabs(LOG[off2].t - 5.5 * st) < MFE_CTL, "no off of 43; 48 off at its gate");
        int on4 = nth(MFE_SEQ_EV_ON, 3), off3 = nth(MFE_SEQ_EV_OFF, 3), on5 = nth(MFE_SEQ_EV_ON, 4);
        CHECK(LOG[on4].note == 36 && off3 >= 0 && LOG[off3].t == LOG[on5].t && off3 < on5,
              "gate 100 %%: off and the next note at the same tick, off first");
    }
    {   /* slide into a rest releases; slide into a tie holds */
        fresh();
        S.pat[0].length = 4;
        set_step(0, 0, 36, 50, 0, 1);
        set_step(0, 1, 0, MFE_SEQ_GATE_REST, 0, 0);
        set_step(0, 2, 40, 50, 0, 1);
        set_step(0, 3, 0, MFE_SEQ_GATE_TIE, 0, 0);
        cmd(MFE_SEQ_CMD_START, 0, 0);
        double st = 60.0 * MFE_SR / (120.0 * 4);
        run((unsigned)(st * 4.5 / MFE_CTL));
        CHECK(fabs(LOG[nth(MFE_SEQ_EV_OFF, 0)].t - st) < MFE_CTL, "slide into a rest: released at the rest");
        int off = nth(MFE_SEQ_EV_OFF, 1);
        CHECK(LOG[off].note == 40 && fabs(LOG[off].t - 4 * st) < MFE_CTL, "slide into a tie: held to the pattern end");
        CHECK(count(MFE_SEQ_EV_SLIDE) == 0, "no slide event without a note to slide to");
    }

    /* ---- pattern length wrap, length change, pattern change, transpose, stop: no stuck notes ---- */
    {
        fresh();
        fill(0, 5, 50);
        cmd(MFE_SEQ_CMD_START, 0, 0);
        run(2000);
        int ok = 1;
        for (int k = 0; k < 12; k++) if (LOG[nth(MFE_SEQ_EV_ON, k)].note != 36 + k % 5) ok = 0;
        CHECK(ok, "length 5 wraps: 36..40, 36..");
        fill(0, 16, 100);                         /* every gate full: notes held up to the next step */
        S.pat[0].length = 12;
        run(1000);
        S.pat[0].length = 3;                      /* shorter than the step playing */
        run(1000);
        uint32_t m = S.log_n;
        run(800);
        ok = 1;
        for (uint32_t i = m; i < S.log_n; i++) if (LOG[i].type == MFE_SEQ_EV_ON && (LOG[i].note < 36 || LOG[i].note > 38)) ok = 0;
        CHECK(ok, "length 12 -> 3 while running: wraps at once");
        cmd(MFE_SEQ_CMD_STOP, 0, 0);
        run(10);
        CHECK(balance() == 0 && !S.running && S.cur == -1, "stop: every note released (balance %d)", balance());
        uint32_t n = S.log_n;
        run(2000);
        CHECK(S.log_n == n, "stopped: silent");
        /* pattern change: cued while running, it starts at the end of the pattern */
        fill(1, 4, 50);
        for (int i = 0; i < 4; i++) set_step(1, (unsigned)i, 60 + i, 50, 0, 0);
        fill(0, 6, 100);
        S.log_n = 0;
        cmd(MFE_SEQ_CMD_START, 0, 0);
        double st = 60.0 * MFE_SR / (120.0 * 4);
        run((unsigned)(st * 2.5 / MFE_CTL));
        cmd(MFE_SEQ_CMD_SLOT, 1, 0);
        run((unsigned)(st * 5 / MFE_CTL));
        int k6 = nth(MFE_SEQ_EV_ON, 6), k5 = nth(MFE_SEQ_EV_ON, 5);
        CHECK(LOG[k5].note == 41 && LOG[k6].note == 60 && S.slot == 1, "pattern 2 cued: starts after step 6 of pattern 1 (%d %d)",
              LOG[k5].note, LOG[k6].note);
        CHECK(balance() <= 1, "pattern change: at most the current note open (%d)", balance());
        /* transpose while running */
        cmd(MFE_SEQ_CMD_TRANSPOSE, 0, 5);
        uint32_t t0 = S.log_n;
        run((unsigned)(st * 4 / MFE_CTL));
        ok = 0;
        for (uint32_t i = t0; i < S.log_n; i++) if (LOG[i].type == MFE_SEQ_EV_ON) ok += LOG[i].note >= 65 && LOG[i].note <= 68;
        CHECK(ok >= 3, "transpose +5: 65..68 (%d)", ok);
        /* every OFF matches the ON before it */
        int held = -1, pairs_ok = 1;
        for (uint32_t i = 0; i < S.log_n; i++) {
            if (LOG[i].type == MFE_SEQ_EV_ON) { if (held >= 0) pairs_ok = 0; held = LOG[i].note; }
            else if (LOG[i].type == MFE_SEQ_EV_SLIDE) { if (held != LOG[i].from) pairs_ok = 0; held = LOG[i].note; }
            else { if (held != LOG[i].note) pairs_ok = 0; held = -1; }
        }
        CHECK(pairs_ok, "mono: never two notes at once, every off names the sounding note");
        cmd(MFE_SEQ_CMD_STOP, 0, 0);
        run(1);
        CHECK(balance() == 0, "stop after transpose: nothing stuck");
    }

    /* ---- record: quantised to the nearest step ---- */
    {
        fresh();
        fill(0, 8, MFE_SEQ_GATE_REST);
        cmd(MFE_SEQ_CMD_START, 0, 0);
        double st = 60.0 * MFE_SR / (120.0 * 4);
        run((unsigned)(st * 2.1 / MFE_CTL));          /* early in step 3 */
        cmd(MFE_SEQ_CMD_RECORD, 50, 0);
        run((unsigned)(st * 0.7 / MFE_CTL));          /* late in step 3: belongs to step 4 */
        cmd(MFE_SEQ_CMD_RECORD, 53, 0);
        run(2);
        CHECK(mfe_seq_note(S.pat[0].step[2]) == 50 && mfe_seq_gate(S.pat[0].step[2]) == 50, "record early in step 3 -> step 3");
        CHECK(mfe_seq_note(S.pat[0].step[3]) == 53 && mfe_seq_gate(S.pat[0].step[3]) == 50, "record late in step 3 -> step 4");
        run((unsigned)(st * 8 / MFE_CTL));
        CHECK(count(MFE_SEQ_EV_ON) >= 2, "recorded notes play the next time round");
    }

    /* ---- MIDI clock follow ---- */
    {
        fresh();
        fill(0, 16, 50);
        tempo(90);                                      /* internal tempo differs from the clock */
        double bpm = 120, tick = 60.0 * MFE_SR / (bpm * 24), next = 0;
        uint32_t t = 0;
        /* clock running before Start (as most gear sends it): followed, but nothing plays */
        for (int i = 0; i < 48; i++) {
            while (t + MFE_BLOCK <= next) { for (int c = 0; c < 4; c++) { mfe_seq_ctl(&S, 0); t += MFE_CTL; } }
            mfe_seq_midi_rt(&S, 0xF8, 0, 0);
            next += tick;
        }
        CHECK(S.ext && !S.running && S.log_n == 0, "clock while stopped: followed, silent");
        CHECK(abs((int)mfe_seq_tempo_q8(&S) - (120 << 8)) < 3 * 256, "measured tempo %.2f BPM", mfe_seq_tempo_q8(&S) / 256.0);
        /* Start, then 64 steps at 120; block-quantised arrival (as the FM1 delivers it) */
        mfe_seq_midi_rt(&S, 0xFA, 0, 0);
        double start_at = next;
        int nticks = 0;
        for (; nticks < 64 * 6; nticks++) {
            while (t + MFE_BLOCK <= next) { for (int c = 0; c < 4; c++) { mfe_seq_ctl(&S, 0); t += MFE_CTL; } }
            mfe_seq_midi_rt(&S, 0xF8, 0, 0);
            next += tick;
        }
        for (int c = 0; c < 4; c++) { mfe_seq_ctl(&S, 0); t += MFE_CTL; }
        double worst = 0;
        int got = 0;
        for (int k = 0; k < 64; k++) {
            int i = nth(MFE_SEQ_EV_ON, k);
            if (i < 0) break;
            got++;
            double d = fabs(LOG[i].t - (start_at + k * 6 * tick));
            if (d > worst) worst = d;
        }
        CHECK(got == 64 && worst <= MFE_BLOCK + MFE_CTL, "MIDI clock 120: 64 steps, worst %.0f samples off (block quantised)", worst);
        /* tempo change to 140 */
        bpm = 140; tick = 60.0 * MFE_SR / (bpm * 24);
        uint32_t m = S.log_n;
        for (int i = 0; i < 24 * 12; i++) {
            while (t + MFE_BLOCK <= next) { for (int c = 0; c < 4; c++) { mfe_seq_ctl(&S, 0); t += MFE_CTL; } }
            mfe_seq_midi_rt(&S, 0xF8, 0, 0);
            next += tick;
        }
        CHECK(abs((int)mfe_seq_tempo_q8(&S) - (140 << 8)) < 3 * 256, "follows 140 BPM (%.2f)", mfe_seq_tempo_q8(&S) / 256.0);
        int a = -1, b = -1, k = 0;
        for (uint32_t i = m; i < S.log_n; i++) if (LOG[i].type == MFE_SEQ_EV_ON) { if (k == 8) a = (int)i; if (k == 32) b = (int)i; k++; }
        double sp = a >= 0 && b >= 0 ? (LOG[b].t - LOG[a].t) / 24.0 : 0;
        CHECK(fabs(sp - 6 * tick) < 12, "step spacing at 140: %.1f (want %.1f)", sp, 6 * tick);
        /* Stop: released, silent although the clock keeps coming */
        mfe_seq_midi_rt(&S, 0xFC, 0, 0);
        mfe_seq_ctl(&S, 0); t += MFE_CTL;
        int stopped_cur = S.nxt;
        CHECK(!S.running && balance() == 0, "MIDI Stop: released (balance %d)", balance());
        uint32_t n = S.log_n;
        for (int i = 0; i < 48; i++) {
            while (t + MFE_BLOCK <= next) { for (int c = 0; c < 4; c++) { mfe_seq_ctl(&S, 0); t += MFE_CTL; } }
            mfe_seq_midi_rt(&S, 0xF8, 0, 0);
            next += tick;
        }
        CHECK(S.log_n == n, "stopped: clock ticks play nothing");
        /* Continue: goes on with the next step, not step 1 */
        mfe_seq_midi_rt(&S, 0xFB, 0, 0);
        for (int i = 0; i < 12; i++) {
            while (t + MFE_BLOCK <= next) { for (int c = 0; c < 4; c++) { mfe_seq_ctl(&S, 0); t += MFE_CTL; } }
            mfe_seq_midi_rt(&S, 0xF8, 0, 0);
            next += tick;
        }
        CHECK(S.running, "MIDI Continue: running");
        {
            int first_after = -1;
            for (uint32_t i = n; i < S.log_n; i++) if (LOG[i].type == MFE_SEQ_EV_ON) { first_after = (int)i; break; }
            CHECK(first_after >= 0 && LOG[first_after].note == 36 + stopped_cur % 12, "Continue: step %d, not step 1 (note %d)",
                  stopped_cur + 1, first_after >= 0 ? LOG[first_after].note : -1);
        }
        /* Song Position while stopped: Continue starts at that sixteenth */
        mfe_seq_midi_rt(&S, 0xFC, 0, 0);
        mfe_seq_ctl(&S, 0); t += MFE_CTL;
        mfe_seq_midi_rt(&S, 0xF2, 5, 0);                 /* sixteenth 5 = step 6 */
        mfe_seq_ctl(&S, 0); t += MFE_CTL;
        n = S.log_n;
        mfe_seq_midi_rt(&S, 0xFB, 0, 0);
        for (int i = 0; i < 6; i++) {
            while (t + MFE_BLOCK <= next) { for (int c = 0; c < 4; c++) { mfe_seq_ctl(&S, 0); t += MFE_CTL; } }
            mfe_seq_midi_rt(&S, 0xF8, 0, 0);
            next += tick;
        }
        int sp_on = -1;
        for (uint32_t i = n; i < S.log_n; i++) if (LOG[i].type == MFE_SEQ_EV_ON) { sp_on = (int)i; break; }
        CHECK(sp_on >= 0 && LOG[sp_on].note == 36 + 5, "Song Position 5 + Continue: step 6 (%d)", sp_on >= 0 ? LOG[sp_on].note : -1);
        /* the clock goes away while running: the internal clock (90 BPM) takes over, no stall */
        uint32_t before = S.log_n;
        run((unsigned)(MFE_SR * 2 / MFE_CTL));
        CHECK(!S.ext && S.running && count(MFE_SEQ_EV_ON) > 0 && S.log_n > before + 4, "clock lost: internal clock carries on");
        CHECK(mfe_seq_tempo_q8(&S) == 90u << 8, "back to the internal tempo");
        cmd(MFE_SEQ_CMD_STOP, 0, 0);
        run(1);
        CHECK(balance() == 0, "nothing stuck after the clock tests");
        /* clock in OFF: ignored */
        cmd(MFE_SEQ_CMD_CLOCK_IN, 0, 0);
        run(1);
        mfe_seq_midi_rt(&S, 0xFA, 0, 0);
        mfe_seq_midi_rt(&S, 0xF8, 0, 0);
        run(10);
        CHECK(!S.running && !S.ext, "clock in OFF: MIDI Start ignored");
        /* clock appears mid-run (no Start): the sequencer follows it from where it is */
        fresh();
        fill(0, 16, 50);
        tempo(100);
        cmd(MFE_SEQ_CMD_START, 0, 0);
        run(1000);
        t = 0; next = 0;
        tick = 60.0 * MFE_SR / (150.0 * 24);
        for (int i = 0; i < 24 * 8; i++) {
            while (t + MFE_BLOCK <= next) { for (int c = 0; c < 4; c++) { mfe_seq_ctl(&S, 0); t += MFE_CTL; } }
            mfe_seq_midi_rt(&S, 0xF8, 0, 0);
            next += tick;
        }
        CHECK(S.ext && S.running && abs((int)mfe_seq_tempo_q8(&S) - (150 << 8)) < 3 * 256, "clock mid-run: follows 150 BPM (%.1f)",
              mfe_seq_tempo_q8(&S) / 256.0);
        cmd(MFE_SEQ_CMD_STOP, 0, 0);
        run(1);
        CHECK(balance() == 0, "stop: nothing stuck");
    }

    /* ---- through the engine: an acid patch, slides, accents; voices released at stop ---- */
    {
        mfe_engine_t *e = (mfe_engine_t *)engine_mem;
        CHECK(mfe_engine_size() <= sizeof engine_mem, "engine fits");
        mfe_engine_init(e);
        mfe_patch_t p;
        mfe_patch_init(&p);
        p.voice_mode = MFE_MONO;
        p.filt[0].type = MFE_F_ACID;
        mfe_engine_set_patch(e, &p);
        fresh();
        S.log = 0;
        S.pat[0].length = 4;
        set_step(0, 0, 36, 50, 1, 1);
        set_step(0, 1, 48, 50, 0, 0);
        set_step(0, 2, 36, MFE_SEQ_GATE_TIE, 0, 0);
        set_step(0, 3, 39, 40, 1, 0);
        mfe_engine_attach_seq(e, &S);
        cmd(MFE_SEQ_CMD_START, 0, 0);
        static int32_t out[MFE_BLOCK * 2];
        int32_t pk = 0;
        unsigned maxv = 0;
        for (int b = 0; b < 2000; b++) {
            mfe_render(e, out);
            for (int i = 0; i < MFE_BLOCK * 2; i++) if (abs(out[i]) > pk) pk = abs(out[i]);
            if (mfe_active_voices(e) > maxv) maxv = mfe_active_voices(e);
            if (b == 700) {                               /* a patch change while running: POLY now */
                p.voice_mode = MFE_POLY;
                mfe_engine_set_patch(e, &p);
            }
            if (b == 1400) {                              /* and back to MONO with a note held from POLY */
                p.voice_mode = MFE_MONO;
                mfe_engine_set_patch(e, &p);
            }
        }
        CHECK(pk > 1000 && maxv >= 1, "engine plays the pattern (peak %d)", pk);
        cmd(MFE_SEQ_CMD_STOP, 0, 0);
        for (int b = 0; b < 600; b++) mfe_render(e, out);
        CHECK(mfe_active_voices(e) == 0, "stop: every voice released and finished (%u)", mfe_active_voices(e));
        mfe_engine_attach_seq(e, 0);
        /* slide: the pitch glides (legato, no retrigger) */
        mfe_engine_init(e);
        p.voice_mode = MFE_MONO;
        p.glide = 0;
        mfe_engine_set_patch(e, &p);
        mfe_note_on(e, 36, 100);
        for (int b = 0; b < 20; b++) mfe_render(e, out);
        mfe_note_slide(e, 36, 48, 100);
        mfe_render(e, out);
        CHECK(mfe_active_voices(e) == 1, "slide keeps one voice");
        mfe_note_off(e, 48);
        for (int b = 0; b < 600; b++) mfe_render(e, out);
        CHECK(mfe_active_voices(e) == 0, "the slid note is released by its new number");
        mfe_note_slide(e, 50, 52, 100);                   /* nothing held: a plain note on */
        CHECK(mfe_active_voices(e) == 1, "slide from nothing = note on");
    }
    {   /* the command ring refuses when full instead of overwriting */
        fresh();
        int r = 0;
        for (int i = 0; i < 20; i++) r += mfe_seq_cmd(&S, MFE_SEQ_CMD_TRANSPOSE, 0, 1) != 0;
        CHECK(r == 4, "command ring holds 16 (%d refused)", r);
        mfe_seq_pattern_t pt;
        mfe_seq_pattern_clear(&pt);
        CHECK(mfe_seq_pattern_valid(&pt), "an empty pattern is valid");
        pt.step[3].gate = 101;
        CHECK(!mfe_seq_pattern_valid(&pt), "gate 101 refused");
    }
    printf("sequencer: %d checks, %d failed\n", checks, fails);
    return fails != 0;
}
