/* SPDX-License-Identifier: AGPL-3.0-only
 * Vintage Chonk: host tests of the MFE engine (measurements, not just "it runs"). */
#include <math.h>
#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "mfe_internal.h"

static int fails, checks;
#define CHECK(cond, ...) do { checks++; if (!(cond)) { fails++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

static mfe_engine_t *eng(const mfe_patch_t *p)
{
    mfe_engine_t *e = calloc(1, mfe_engine_size());
    mfe_engine_init(e);
    mfe_engine_set_patch(e, p);
    return e;
}

/* render n blocks, left channel into out (may be NULL) */
static void run(mfe_engine_t *e, int blocks, float *out)
{
    int32_t b[MFE_BLOCK * 2];
    for (int k = 0; k < blocks; k++) {
        mfe_render(e, b);
        if (out) for (int i = 0; i < MFE_BLOCK; i++) out[k * MFE_BLOCK + i] = b[2 * i] / 32768.0f;
    }
}

static double freq_of(const float *x, int n)
{
    /* rising zero crossings with linear interpolation */
    double first = -1, last = -1;
    int count = 0;
    for (int i = 1; i < n; i++)
        if (x[i - 1] < 0 && x[i] >= 0) {
            double t = i - 1 + x[i - 1] / (x[i - 1] - x[i]);
            if (first < 0) first = t;
            last = t;
            count++;
        }
    return count > 1 ? (count - 1) * (double)MFE_SR / (last - first) : 0;
}

static mfe_patch_t sine_patch(void)
{
    mfe_patch_t p;
    mfe_patch_init(&p);
    p.osc[0].position = 0;          /* BASIC frame 0 = sine */
    p.filt[0].type = MFE_F_OFF;
    p.env[3].attack = 0;
    p.vel_sens = 0;
    return p;
}

static void test_pitch(void)
{
    static const int notes[] = {21, 45, 69, 93, 108};
    static float x[MFE_SR];
    for (unsigned k = 0; k < sizeof notes / sizeof *notes; k++) {
        mfe_patch_t p = sine_patch();
        mfe_engine_t *e = eng(&p);
        mfe_note_on(e, (uint8_t)notes[k], 100);
        run(e, 8, 0);
        int blocks = MFE_SR / MFE_BLOCK;
        run(e, blocks, x);
        double f = freq_of(x, blocks * MFE_BLOCK);
        double want = 440.0 * pow(2.0, (notes[k] - 69) / 12.0);
        double cents = 1200.0 * log2(f / want);
        CHECK(fabs(cents) < 1.0, "note %d: %.3f Hz, want %.3f (%.2f cents)", notes[k], f, want, cents);
        free(e);
    }
}

static void test_band_limit(void)
{
    /* every level's top harmonic stays below Nyquist for the increments that select it */
    for (int note = 0; note <= 135; note++) {
        uint32_t inc = mfe_pitch_inc(note << 8);
        unsigned L = mfe_wt_level(inc);
        double f0 = (double)inc / 4294967296.0 * MFE_SR, top = f0 * (128 >> L);
        CHECK(top < MFE_SR / 2.0 || L == MFE_WT_LEVELS - 1, "note %d level %u top %.0f Hz", note, L, top);
    }
}

static void test_envelope(void)
{
    static const int ps[] = {8000, 16000, 24000};
    static float x[MFE_SR * 4];
    for (int k = 0; k < 3; k++) {
        mfe_patch_t p = sine_patch();
        p.env[3].attack = (int16_t)ps[k];
        mfe_engine_t *e = eng(&p);
        mfe_note_on(e, 93, 127);         /* 1760 Hz sine: a 64-sample window holds 2.5 periods */
        int blocks = (int)(MFE_SR * 4 / MFE_BLOCK), n = blocks * MFE_BLOCK;
        run(e, blocks, x);
        float steady = 0;
        for (int i = n - 4096; i < n; i++) if (fabsf(x[i]) > steady) steady = fabsf(x[i]);
        int t90 = -1;
        for (int i = 0; i + 64 <= n && t90 < 0; i += 8) {
            float w = 0;
            for (int j = 0; j < 64; j++) if (fabsf(x[i + j]) > w) w = fabsf(x[i + j]);
            if (w >= 0.9f * steady) t90 = i + 32;
        }
        double want = 0.0005 * pow(MFE_TIME_RANGE, ps[k] / 32768.0) * 0.9;
        double got = (double)t90 / MFE_SR;
        /* control rate (0.73 ms) plus one tick of amplitude ramp */
        CHECK(fabs(got - want) < 0.05 * want + 0.0025, "attack %d: 90%% at %.4f s, want %.4f s", ps[k], got, want);
        free(e);
    }
    /* release ends the voice */
    mfe_patch_t p = sine_patch();
    p.env[3].release = 8000;
    mfe_engine_t *e = eng(&p);
    mfe_note_on(e, 60, 100);
    run(e, 20, 0);
    mfe_note_off(e, 60);
    run(e, MFE_SR / MFE_BLOCK, 0);
    CHECK(mfe_active_voices(e) == 0, "voice still active after release");
    free(e);
}

static double tone_gain(uint8_t type, int cutoff_note, double freq, int res)
{
    mfe_svf_coef_t c;
    mfe_svf_state_t s = {0, 0};
    mfe_svf_coef(&c, type, cutoff_note << 8, res);
    double in = 0, out = 0;
    for (int i = 0; i < MFE_SR / 2; i++) {
        int32_t x = (int32_t)lrint(16000 * sin(2 * M_PI * freq * i / MFE_SR));
        int32_t y = mfe_svf_tick(&s, &c, x);
        if (i > MFE_SR / 4) { in += (double)x * x; out += (double)y * y; }
    }
    return 10 * log10(out / in);
}

static void test_filters(void)
{
    /* LP2 at note 84 (1046.5 Hz) */
    double pass = tone_gain(MFE_F_LP2, 84, 100, 0), stop = tone_gain(MFE_F_LP2, 84, 10000, 0);
    double at = tone_gain(MFE_F_LP2, 84, 1046.5, 0);
    CHECK(fabs(pass) < 0.5, "LP2 passband %.2f dB", pass);
    CHECK(stop < -35, "LP2 at 10 kHz %.2f dB", stop);
    CHECK(at < -4.0 && at > -8.0, "LP2 at cutoff %.2f dB (no resonance: about -6 dB)", at);
    double hp_low = tone_gain(MFE_F_HP, 84, 100, 0), hp_hi = tone_gain(MFE_F_HP, 84, 10000, 0);
    CHECK(hp_low < -35 && fabs(hp_hi) < 0.5, "HP %.2f / %.2f dB", hp_low, hp_hi);
    double bp_c = tone_gain(MFE_F_BP, 84, 1046.5, 26000), bp_far = tone_gain(MFE_F_BP, 84, 100, 26000);
    CHECK(bp_c - bp_far > 15, "BP centre %.2f vs 100 Hz %.2f dB", bp_c, bp_far);
    double res = tone_gain(MFE_F_LP2, 84, 1046.5, 30000);
    CHECK(res > 6, "resonant peak %.2f dB", res);
    /* stability: full-scale noise into every type at every cutoff, maximum resonance */
    uint32_t r = 1;
    for (uint8_t t = MFE_F_LP2; t < MFE_F_COUNT; t++)
        for (int note = 0; note <= 136; note += 8) {
            mfe_svf_coef_t c;
            mfe_svf_state_t s = {0, 0};
            mfe_svf_coef(&c, t == MFE_F_LP4 ? MFE_F_LP2 : t, note << 8, 32767);
            int32_t mx = 0;
            for (int i = 0; i < 20000; i++) {
                int32_t x = i < 10000 ? (int32_t)(mfe_rand(&r) >> 16) - 32768 : 0;
                int32_t y = mfe_svf_tick(&s, &c, x);
                if (abs(y) > mx) mx = abs(y);
            }
            CHECK(mx < 6 * 32768, "type %u note %d: peak %d", t, note, mx);
        }
}

static double ladder_gain_t(int acid, int cutoff_note, double freq, int res)
{
    mfe_ladder_coef_t c;
    mfe_ladder_state_t s;
    memset(&s, 0, sizeof s);
    if (acid) mfe_acid_coef(&c, cutoff_note << 8, res);
    else mfe_ladder_coef(&c, cutoff_note << 8, res);
    double in = 0, out = 0;
    for (int i = 0; i < MFE_SR / 2; i++) {
        int32_t x = (int32_t)lrint(12000 * sin(2 * M_PI * freq * i / MFE_SR));   /* moderate: little saturation */
        int32_t y = mfe_ladder_tick(&s, &c, x);
        if (i > MFE_SR / 4) { in += (double)x * x; out += (double)y * y; }
    }
    return 10 * log10(out / in);
}
static double ladder_gain(int note, double f, int res) { return ladder_gain_t(0, note, f, res); }

static void test_acid(void)
{
    double l2 = ladder_gain_t(0, 84, 2093, 0), l4 = ladder_gain_t(0, 84, 4186, 0);
    double a2 = ladder_gain_t(1, 84, 2093, 0), a4 = ladder_gain_t(1, 84, 4186, 0);
    /* 18 vs 24 dB/oct: two octaves above the cutoff the acid filter cuts clearly less */
    CHECK(a4 > l4 + 4 && (a4 - a2) > (l4 - l2), "acid at 4x cutoff %.1f dB vs ladder %.1f (slopes %.1f / %.1f dB/oct)",
          a4, l4, a4 - a2, l4 - l2);
    double lb = ladder_gain_t(0, 84, 80, 29000), ab = ladder_gain_t(1, 84, 80, 29000);
    CHECK(ab > lb + 2, "acid keeps bass at high resonance: %.1f dB vs ladder %.1f dB", ab, lb);
    CHECK(ladder_gain_t(1, 84, 1046.5, 29000) > ladder_gain_t(1, 84, 1046.5, 0) + 6, "acid resonance peak");
    mfe_ladder_coef_t c;
    mfe_ladder_state_t s;
    uint32_t r = 5;
    for (int note = 0; note <= 136; note += 8) {
        memset(&s, 0, sizeof s);
        mfe_acid_coef(&c, note << 8, 32767);
        int32_t m2 = 0;
        for (int i = 0; i < 20000; i++) {
            int32_t y = mfe_ladder_tick(&s, &c, (int32_t)(mfe_rand(&r) >> 16) - 32768);
            if (abs(y) > m2) m2 = abs(y);
        }
        CHECK(m2 < 8 * 32768, "acid noise note %d peak %d", note, m2);
    }
}

static void test_ladder(void)
{
    /* cutoff C6 (1046.5 Hz): flat below, 24 dB/oct above */
    double pass = ladder_gain(84, 100, 0), oct2 = ladder_gain(84, 2093, 0), oct3 = ladder_gain(84, 4186, 0);
    CHECK(fabs(pass) < 1.0, "ladder passband %.2f dB", pass);
    CHECK(oct3 - oct2 < -18 && oct3 - oct2 > -30, "ladder slope %.1f dB/oct (want about -24)", oct3 - oct2);
    CHECK(ladder_gain(84, 1046.5, 30000) > ladder_gain(84, 1046.5, 0) + 6, "ladder resonance peak");
    /* full resonance: rings on with no input, bounded */
    mfe_ladder_coef_t c;
    mfe_ladder_state_t s;
    memset(&s, 0, sizeof s);
    mfe_ladder_coef(&c, 72 << 8, 32767);
    int32_t mx = 0, late = 0;
    for (int i = 0; i < MFE_SR; i++) {
        int32_t y = mfe_ladder_tick(&s, &c, i < 64 ? 8000 : 0);
        if (abs(y) > mx) mx = abs(y);
        if (i > MFE_SR / 2 && abs(y) > late) late = abs(y);
    }
    CHECK(late > 3000, "ladder self-oscillates at full resonance (late peak %d)", late);
    CHECK(mx < 8 * 32768, "ladder bounded (peak %d)", mx);
    uint32_t r = 3;
    for (int note = 0; note <= 136; note += 8) {
        memset(&s, 0, sizeof s);
        mfe_ladder_coef(&c, note << 8, 32767);
        int32_t m2 = 0;
        for (int i = 0; i < 20000; i++) {
            int32_t y = mfe_ladder_tick(&s, &c, (int32_t)(mfe_rand(&r) >> 16) - 32768);
            if (abs(y) > m2) m2 = abs(y);
        }
        CHECK(m2 < 8 * 32768, "ladder noise note %d peak %d", note, m2);
    }
}

static void test_modulation(void)
{
    static float x[MFE_SR];
    mfe_patch_t p = sine_patch();
    p.mod[0].src = MFE_S_ONE;
    p.mod[0].dst = MFE_D_PITCH;
    p.mod[0].amount = 32768 * 12 / MFE_PITCH_MOD_RANGE;   /* +12 semitones */
    mfe_engine_t *e = eng(&p);
    mfe_note_on(e, 57, 100);        /* 220 Hz -> 440 */
    run(e, 8, 0);
    run(e, MFE_SR / MFE_BLOCK, x);
    double f = freq_of(x, MFE_SR / MFE_BLOCK * MFE_BLOCK);
    CHECK(fabs(1200 * log2(f / 440.0)) < 2, "ONE->PITCH +12: %.2f Hz", f);
    free(e);
    /* mod wheel -> pitch, live */
    p = sine_patch();
    p.mod[0].src = MFE_S_MODWHEEL;
    p.mod[0].dst = MFE_D_PITCH;
    p.mod[0].amount = 32767 * 7 / MFE_PITCH_MOD_RANGE + 1;   /* 7 semitones at full wheel */
    e = eng(&p);
    mfe_note_on(e, 57, 100);
    uint8_t cc[3] = {0xB0, 1, 127};
    mfe_midi(e, cc, 3);
    run(e, 8, 0);
    run(e, MFE_SR / MFE_BLOCK, x);
    f = freq_of(x, MFE_SR / MFE_BLOCK * MFE_BLOCK);
    double want = 220 * pow(2, 7 / 12.0);
    CHECK(fabs(1200 * log2(f / want)) < 5, "MODWHEEL->PITCH: %.2f Hz want %.2f", f, want);
    free(e);
    /* pitch bend 2 semitones */
    p = sine_patch();
    e = eng(&p);
    mfe_note_on(e, 57, 100);
    uint8_t pb[3] = {0xE0, 0x7F, 0x7F};
    mfe_midi(e, pb, 3);
    run(e, 8, 0);
    run(e, MFE_SR / MFE_BLOCK, x);
    f = freq_of(x, MFE_SR / MFE_BLOCK * MFE_BLOCK);
    want = 220 * pow(2, 2 / 12.0);
    CHECK(fabs(1200 * log2(f / want)) < 3, "bend +2: %.2f Hz want %.2f", f, want);
    free(e);
}

static void test_voices(void)
{
    mfe_patch_t p = sine_patch();
    mfe_engine_t *e = eng(&p);
    for (int i = 0; i < 10; i++) mfe_note_on(e, (uint8_t)(40 + i), 100);
    run(e, 1, 0);
    CHECK(mfe_active_voices(e) == MFE_MAX_VOICES, "poly: %u voices", mfe_active_voices(e));
    mfe_note_on(e, 49, 100);        /* already sounding: reuses, does not steal */
    run(e, 1, 0);
    CHECK(mfe_active_voices(e) == MFE_MAX_VOICES, "same note");
    mfe_all_notes_off(e);
    run(e, MFE_SR / MFE_BLOCK, 0);
    CHECK(mfe_active_voices(e) == 0, "all notes off: %u", mfe_active_voices(e));
    /* sustain pedal */
    uint8_t ped_on[3] = {0xB0, 64, 127}, ped_off[3] = {0xB0, 64, 0};
    mfe_midi(e, ped_on, 3);
    mfe_note_on(e, 60, 100);
    mfe_note_off(e, 60);
    run(e, MFE_SR / MFE_BLOCK, 0);
    CHECK(mfe_active_voices(e) == 1, "sustain holds");
    mfe_midi(e, ped_off, 3);
    run(e, MFE_SR / MFE_BLOCK, 0);
    CHECK(mfe_active_voices(e) == 0, "pedal up releases");
    free(e);
    /* mono / legato: one voice, back to the held note */
    static float x[MFE_SR];
    p = sine_patch();
    p.voice_mode = MFE_LEGATO;
    e = eng(&p);
    mfe_note_on(e, 57, 100);
    mfe_note_on(e, 69, 100);
    run(e, 4, 0);
    CHECK(mfe_active_voices(e) == 1, "legato: %u voices", mfe_active_voices(e));
    mfe_note_off(e, 69);
    run(e, 8, 0);
    run(e, MFE_SR / 2 / MFE_BLOCK, x);
    double f = freq_of(x, MFE_SR / 2 / MFE_BLOCK * MFE_BLOCK);
    CHECK(fabs(f - 220) < 1, "legato back to 57: %.2f Hz", f);
    free(e);
}

static void test_performer_stepper(void)
{
    /* Performer: 4-step loop RAMP UP, SQUARE, ZERO, FULL at half level on step 3; sync 1/16 at 120 BPM:
     * a step lasts 0.125 s = 172 control ticks */
    mfe_lfo_t l;
    memset(&l, 0, sizeof l);
    l.wave = MFE_W_PERFORMER;
    l.flags = MFE_LFO_SYNC;
    l.sync_div = 2;                                   /* 1/4 beat */
    l.loop_start = 0;
    l.loop_end = 3;
    l.step_shape[0] = 0; l.step_shape[1] = 9; l.step_shape[2] = 16; l.step_shape[3] = 17;
    for (int k = 0; k < 16; k++) l.step_level[k] = 127;
    l.step_level[3] = 64;
    mfe_lfo_state_t s;
    memset(&s, 0, sizeof s);
    mfe_lfo_reset(&s, &l);
    uint32_t num, den;
    mfe_sync_beats(2, &num, &den);
    uint32_t inc = (uint32_t)((((uint64_t)(120u << 8) * MFE_CTL * den) << 24) / ((uint64_t)60 * MFE_SR * num));
    int32_t v[800];
    for (int i = 0; i < 800; i++) v[i] = mfe_lfo_tick(&s, &l, 0, inc);
    double ticks_per_step = 0.125 * MFE_SR / MFE_CTL;
    int mid0 = (int)(ticks_per_step * 0.5), mid1a = (int)(ticks_per_step * 1.25), mid1b = (int)(ticks_per_step * 1.75);
    int mid2 = (int)(ticks_per_step * 2.5), mid3 = (int)(ticks_per_step * 3.5), mid4 = (int)(ticks_per_step * 4.5);
    CHECK(abs(v[mid0] - 16384) < 1500, "performer ramp at mid step 0: %d", v[mid0]);
    CHECK(v[mid1a] > 30000 && v[mid1b] < 1000, "performer square in step 1: %d then %d", v[mid1a], v[mid1b]);
    CHECK(abs(v[mid2]) < 200, "performer zero in step 2: %d", v[mid2]);
    CHECK(abs(v[mid3] - 16510) < 600, "performer full at level 64 in step 3: %d", v[mid3]);
    CHECK(abs(v[mid4] - 16384) < 1500, "performer loops back to step 0: %d", v[mid4]);
    /* Stepper: levels jump without glide, approach with glide */
    l.wave = MFE_W_STEPPER;
    l.step_level[0] = 127; l.step_level[1] = -127; l.step_level[2] = 0; l.step_level[3] = 64;
    l.glide = 0;
    memset(&s, 0, sizeof s);
    mfe_lfo_reset(&s, &l);
    for (int i = 0; i < 800; i++) v[i] = mfe_lfo_tick(&s, &l, 0, inc);
    CHECK(v[mid0] > 32000 && v[mid1a] < -32000 && abs(v[mid2]) < 300 && abs(v[mid3] - 16512) < 300,
          "stepper levels %d %d %d %d", v[mid0], v[mid1a], v[mid2], v[mid3]);
    int edge = (int)ticks_per_step + 1;
    l.glide = 30000;
    memset(&s, 0, sizeof s);
    mfe_lfo_reset(&s, &l);
    for (int i = 0; i < 800; i++) v[i] = mfe_lfo_tick(&s, &l, 0, inc);
    CHECK(v[edge + 1] > -20000 && v[mid1b] < -30000, "stepper glide: %d just after the step, %d later",
          v[edge + 1], v[mid1b]);
    /* shapes: endpoints of a few library curves */
    CHECK(mfe_perf_shape(0, 0) < 200 && mfe_perf_shape(0, 65535) > 32000, "RAMP UP endpoints");
    CHECK(mfe_perf_shape(12, 0) > 32000 && mfe_perf_shape(12, 65535) < 400, "DECAY endpoints");
    CHECK(mfe_perf_shape(10, 32768) > 32000, "TRIANGLE peak in the middle");
    CHECK(abs(mfe_perf_shape(11, 32768) - 32767) < 200 && mfe_perf_shape(11, 0) < 200, "BUMP raised cosine");
    CHECK(mfe_perf_shape(8, 0) > 32600 && mfe_perf_shape(8, 65000) > 32600, "HIGH held");
    CHECK(mfe_perf_shape(3, 0) > 32600 && mfe_perf_shape(3, 65535) < 300, "EXP DOWN falls to 0");
    CHECK(!strcmp(mfe_perf_shape_name(15), "DECAY X4") && !strcmp(mfe_perf_shape_name(3), "EXP DOWN"), "shape names");
}

static void test_moog_features(void)
{
    static float x[MFE_SR * 4];
    int n4 = MFE_SR * 4 / MFE_BLOCK;
    /* LO range: about 2.04 Hz (note -24) whatever key is played */
    for (int k = 0; k < 2; k++) {
        mfe_patch_t p = sine_patch();
        p.osc[0].flags |= MFE_OSC_LO;
        mfe_engine_t *e = eng(&p);
        mfe_note_on(e, (uint8_t)(k ? 72 : 48), 100);
        run(e, n4, x);
        double f = freq_of(x, n4 * MFE_BLOCK);
        CHECK(fabs(f - 2.04) < 0.1, "LO range on note %d: %.3f Hz", k ? 72 : 48, f);
        free(e);
    }
    /* drift: moves, but stays within about +-12 cents */
    {
        mfe_patch_t p = sine_patch();
        p.osc[0].drift = 127;
        mfe_engine_t *e = eng(&p);
        mfe_note_on(e, 69, 100);
        double lo = 1e9, hi = 0;
        for (int sec = 0; sec < 6; sec++) {
            run(e, MFE_SR / MFE_BLOCK, x);
            double f = freq_of(x, MFE_SR / MFE_BLOCK * MFE_BLOCK);
            double c = 1200 * log2(f / 440.0);
            if (c < lo) lo = c;
            if (c > hi) hi = c;
        }
        CHECK(hi - lo > 0.5 && lo > -13 && hi < 13, "drift range %.2f .. %.2f cents", lo, hi);
        free(e);
    }
    /* osc 3 as a modulator: LO-range osc 3 (silent) into pitch -> a periodic vibrato */
    {
        mfe_patch_t p = sine_patch();
        p.osc[2].flags = MFE_OSC_LO;        /* runs, but its amp is 0: not heard */
        p.osc[2].amp = 0;
        p.osc[2].semis = 24;                /* about 8 Hz */
        p.mod[0].src = MFE_S_OSC3;
        p.mod[0].dst = MFE_D_PITCH;
        p.mod[0].amount = 32768 / MFE_PITCH_MOD_RANGE;   /* +-1 semitone */
        mfe_engine_t *e = eng(&p);
        mfe_note_on(e, 69, 100);
        run(e, 8, 0);
        double lo = 1e9, hi = 0;
        for (int w = 0; w < 40; w++) {
            run(e, 4, x);                    /* 11.6 ms windows */
            double f = freq_of(x, 4 * MFE_BLOCK);
            if (f > 0 && f < lo) lo = f;
            if (f > hi) hi = f;
        }
        CHECK(hi / lo > 1.06 && hi / lo < 1.2, "osc 3 vibrato %.1f .. %.1f Hz", lo, hi);
        free(e);
    }
}

static double band_energy(const float *x, int n, double f0)
{
    /* energy outside the harmonics of f0 (sidebands), relative to total, by a simple DFT probe */
    double tot = 0, off = 0;
    for (double f = 40; f < 4000; f += 10) {
        double re = 0, im = 0;
        for (int i = 0; i < n; i++) { re += x[i] * cos(2 * M_PI * f * i / MFE_SR); im += x[i] * sin(2 * M_PI * f * i / MFE_SR); }
        double e = re * re + im * im, h = f / f0;
        tot += e;
        if (fabs(h - floor(h + 0.5)) > 0.2) off += e;
    }
    return off / (tot + 1e-12);
}

static void test_modosc(void)
{
    static float x[8192];
    /* ring mod with the mod osc a fifth above (ratio 1.5): energy between the harmonics of the note */
    mfe_patch_t p = sine_patch();
    mfe_engine_t *e = eng(&p);
    mfe_note_on(e, 57, 100);
    run(e, 8, 0);
    run(e, 64, x);
    double plain = band_energy(x, 8192, 220.0);
    free(e);
    p.modosc.semis = 7;
    p.modosc.rm_osc = 1;
    p.modosc.rm = 32767;
    e = eng(&p);
    mfe_note_on(e, 57, 100);
    run(e, 8, 0);
    run(e, 64, x);
    double rm = band_energy(x, 8192, 220.0);
    CHECK(plain < 0.01 && rm > 0.3, "ring mod sidebands: %.3f plain, %.3f ring", plain, rm);
    free(e);
    /* phase mod at 1:1 adds harmonics to a sine; position mod changes the waveform over time */
    p = sine_patch();
    p.modosc.pm_osc = 1;
    p.modosc.pm = 8000;
    e = eng(&p);
    mfe_note_on(e, 57, 100);
    run(e, 8, 0);
    run(e, 64, x);
    double h2 = 0, h1 = 0;
    for (int k = 1; k <= 2; k++) {
        double re = 0, im = 0;
        for (int i = 0; i < 8192; i++) { re += x[i] * cos(2 * M_PI * 220.0 * k * i / MFE_SR); im += x[i] * sin(2 * M_PI * 220.0 * k * i / MFE_SR); }
        if (k == 1) h1 = re * re + im * im; else h2 = re * re + im * im;
    }
    CHECK(h2 > 0.01 * h1, "phase mod adds a 2nd harmonic (%.4f of the fundamental)", h2 / h1);
    free(e);
    /* the preset round trip keeps the mod osc */
    mfe_patch_t a, b;
    mfe_patch_init(&a);
    a.modosc.semis = -12; a.modosc.pos_osc = 3; a.modosc.pos = 1234; a.modosc.ffm_filter = 2;
    uint8_t buf[1024];
    size_t n = mfe_preset_write(&a, buf, sizeof buf);
    CHECK(mfe_preset_read(&b, buf, n) == 0 && !memcmp(&a.modosc, &b.modosc, sizeof a.modosc), "mod osc round trip");
}

static void test_live_macros(void)
{
    mfe_patch_t p = sine_patch();
    p.mod[0].src = MFE_S_MACRO1;
    p.mod[0].dst = MFE_D_PITCH;
    p.mod[0].amount = 32768 * 12 / MFE_PITCH_MOD_RANGE;   /* macro 1 full = +12 semitones */
    mfe_engine_t *e = eng(&p);
    uint8_t cc[3] = {0xB0, 16, 127};
    mfe_midi(e, cc, 3);
    p.osc[0].amp = 20000;                                  /* a panel edit of something else */
    mfe_engine_set_patch(e, &p);
    static float x[MFE_SR / 2];
    mfe_note_on(e, 57, 100);
    run(e, 8, 0);
    run(e, MFE_SR / 2 / MFE_BLOCK, x);
    double f = freq_of(x, MFE_SR / 2 / MFE_BLOCK * MFE_BLOCK);
    CHECK(fabs(1200 * log2(f / 440.0)) < 5, "live macro survives a panel edit: %.2f Hz", f);
    p.macro[0] = 0;                                        /* unchanged in the patch: still live */
    p.macro[0] = 100;                                      /* changed in the patch: patch value wins */
    mfe_engine_set_patch(e, &p);
    run(e, 8, 0);
    run(e, MFE_SR / 2 / MFE_BLOCK, x);
    f = freq_of(x, MFE_SR / 2 / MFE_BLOCK * MFE_BLOCK);
    CHECK(f < 230, "a changed patch macro resets the live value: %.2f Hz", f);
    free(e);
}

static void test_preset(void)
{
    mfe_patch_t a, b;
    mfe_patch_init(&a);
    strcpy(a.name, "ROUND TRIP");
    a.osc[1].semis = -12; a.osc[2].fine = -37; a.filt[1].type = MFE_F_BP; a.mod[15].src = MFE_S_MACRO8;
    a.mod[15].dst = MFE_D_F2_CUTOFF; a.mod[15].amount = -1234; a.fx.rev_amount = 999; a.fx.ens_mode = MFE_ENS_VIBRATO; a.fx.oil_smear = 77;
    uint8_t buf[1024];
    size_t n = mfe_preset_write(&a, buf, sizeof buf);
    CHECK(n == mfe_preset_size() && n == 508, "preset size %zu", n);
    CHECK(mfe_preset_read(&b, buf, n) == 0 && !memcmp(&a, &b, sizeof a), "round trip");
    buf[100] ^= 1;
    CHECK(mfe_preset_read(&b, buf, n) == -4, "crc detects corruption");
    buf[100] ^= 1;
    CHECK(mfe_preset_read(&b, buf, n - 1) == -1, "short");
    buf[0] = 'X';
    CHECK(mfe_preset_read(&b, buf, n) == -2, "magic");
}

/* ------------------------------------------------------------ master effects (preset v5) --- */
#ifndef MFE_DELAY_LEN
#define MFE_DELAY_LEN 22050          /* as dsp/effects/mfe_fx.c */
#endif
static mfe_fx_state_t *fx_new(void)
{
    mfe_fx_state_t *f = malloc(mfe_fx_size());
    mfe_fx_init(f);
    return f;
}
static void fx_run(mfe_fx_state_t *f, const mfe_fx_t *p, int32_t *lr, long frames)
{
    for (long k = 0; k + MFE_BLOCK <= frames; k += MFE_BLOCK) mfe_fx_process(f, p, lr + 2 * k, MFE_BLOCK);
}
/* magnitude of frequency hz in channel c of an interleaved buffer (Goertzel) */
static double tone_mag(const int32_t *lr, int c, long from, long n, double hz)
{
    double w = 2 * M_PI * hz / MFE_SR, k = 2 * cos(w), s1 = 0, s2 = 0;
    for (long i = from; i < from + n; i++) { double s = lr[2 * i + c] + k * s1 - s2; s2 = s1; s1 = s; }
    return sqrt(fabs(s1 * s1 + s2 * s2 - k * s1 * s2)) / n;
}
static double rms(const int32_t *lr, int c, long from, long n)
{
    double e = 0;
    for (long i = from; i < from + n; i++) e += (double)lr[2 * i + c] * lr[2 * i + c];
    return sqrt(e / n);
}
static void fill_sine(int32_t *lr, long from, long n, double hz, double amp)
{
    for (long i = from; i < from + n; i++) lr[2 * i] = lr[2 * i + 1] = (int32_t)lrint(amp * 32767 * sin(2 * M_PI * hz * i / MFE_SR));
}
static void click(int32_t *lr, long at)
{
    for (long i = at; i < at + 44; i++) lr[2 * i] = lr[2 * i + 1] = (int32_t)(20000 * sin(i * 0.9));   /* a bright 1 ms burst */
}
/* rising zero crossings of channel c in [from, from + n): (max - min) / mean of the periods */
static double period_spread(const int32_t *lr, int c, long from, long n)
{
    double last = -1, mn = 1e9, mx = 0, sum = 0;
    int cnt = 0;
    for (long i = from + 1; i < from + n; i++) {
        double a = lr[2 * (i - 1) + c], b = lr[2 * i + c];
        if (a < 0 && b >= 0) {
            double t = i - 1 + a / (a - b);
            if (last >= 0) { double q = t - last; if (q < mn) mn = q; if (q > mx) mx = q; sum += q; cnt++; }
            last = t;
        }
    }
    return cnt > 2 ? (mx - mn) / (sum / cnt) : 0;
}
static long peak_at(const int32_t *lr, int c, long from, long n)
{
    long at = from;
    for (long i = from; i < from + n; i++) if (abs(lr[2 * i + c]) > abs(lr[2 * at + c])) at = i;
    return at;
}
/* first-difference energy over signal energy: the share of high frequencies in a window */
static double hf_share(const int32_t *lr, long from, long n)
{
    double e = 0, d = 0;
    for (long i = from + 1; i < from + n; i++) {
        double x = lr[2 * i], y = x - lr[2 * (i - 1)];
        e += x * x; d += y * y;
    }
    return e > 0 ? d / e : 0;
}

static void test_fx_bypass(void)
{
    enum { N = MFE_SR };
    static int32_t a[2 * N], b[2 * N];
    uint32_t r = 99;
    for (long i = 0; i < 2 * N; i++) a[i] = (int32_t)(mfe_rand(&r) % 98304) - 49152;   /* +-1.5 FS: headroom kept */
    mfe_fx_t p;
    mfe_fx_defaults(&p);
    mfe_fx_state_t *f = fx_new();
    memcpy(b, a, sizeof a);
    fx_run(f, &p, b, N);
    CHECK(!memcmp(a, b, sizeof a), "all effects at 0: bit-exact bypass");
    for (int k = 0; k < 6; k++) {             /* every other knob random, the on-parameters 0 */
        mfe_fx_t q;
        uint8_t *u = (uint8_t *)&q;
        for (size_t i = 0; i < sizeof q; i++) u[i] = (uint8_t)mfe_rand(&r);
        q.fuzz_amount = q.tape_drive = q.bbd_mix = q.oil_mix = q.rev_amount = 0;
        q.ens_mode = (uint8_t)(k & 1 ? MFE_ENS_OFF : MFE_ENS_I);
        if (q.ens_mode) q.ens_mix = 0;
        q.rev_type %= MFE_REV_COUNT;
        memcpy(b, a, sizeof a);
        fx_run(f, &q, b, N);
        CHECK(!memcmp(a, b, sizeof a), "on-parameters 0, random other knobs (%d): bit-exact", k);
    }
    mfe_fx_t on = p;
    on.fuzz_amount = on.tape_drive = on.bbd_mix = on.oil_mix = on.rev_amount = 20000;
    on.ens_mode = MFE_ENS_I_II;
    memcpy(b, a, sizeof a);
    fx_run(f, &on, b, N);
    CHECK(memcmp(a, b, sizeof a) != 0, "effects on change the signal");
    memcpy(b, a, sizeof a);
    fx_run(f, &p, b, N);
    CHECK(!memcmp(a, b, sizeof a), "switched off again: bit-exact");
    free(f);
}

static void test_fx_fuzz(void)
{
    enum { N = MFE_SR };
    static int32_t x[2 * N];
    mfe_fx_t p;
    mfe_fx_defaults(&p);
    p.fuzz_amount = 20000; p.fuzz_tone = 20000;
    double sub[2], fund[2], h2[2];
    for (int o = 0; o < 2; o++) {
        p.fuzz_octave = (int16_t)(o ? 32767 : 0);
        mfe_fx_state_t *f = fx_new();
        fill_sine(x, 0, N, 220, 0.5);
        fx_run(f, &p, x, N);
        sub[o] = tone_mag(x, 0, N / 4, N / 2, 110);
        fund[o] = tone_mag(x, 0, N / 4, N / 2, 220);
        h2[o] = tone_mag(x, 0, N / 4, N / 2, 440);
        int mono = 1;
        for (long i = 0; i < N; i++) if (x[2 * i] != x[2 * i + 1]) mono = 0;
        CHECK(mono, "OCT FUZZ output is mono (L = R)");
        free(f);
    }
    CHECK(sub[1] > 0.4 * fund[1], "OCT FUZZ octave down: 110 Hz %.0f vs 220 Hz %.0f", sub[1], fund[1]);
    CHECK(sub[0] < 0.1 * fund[0], "octave level 0: no sub-octave (%.0f vs %.0f)", sub[0], fund[0]);
    CHECK(h2[0] > 0.05 * fund[0], "lopsided clip: 2nd harmonic %.0f vs fundamental %.0f", h2[0], fund[0]);
    p.fuzz_octave = 0; p.fuzz_amount = 4000;   /* the starved stage gates: a quiet note sputters to nothing */
    mfe_fx_state_t *f = fx_new();
    fill_sine(x, 0, N, 330, 0.002);
    fx_run(f, &p, x, N);
    double quiet = rms(x, 0, N / 2, N / 2);
    fill_sine(x, 0, N, 330, 0.3);
    fx_run(f, &p, x, N);
    double loud = rms(x, 0, N / 2, N / 2);
    CHECK(loud > 3000 && quiet < loud / 300, "gate: -54 dBFS in gives rms %.1f, -10 dBFS gives %.0f", quiet, loud);
    free(f);
}

static void test_fx_tape(void)
{
    enum { N = MFE_SR / 2 };
    static int32_t x[2 * N];
    mfe_fx_t p;
    mfe_fx_defaults(&p);
    double thd[2];
    for (int k = 0; k < 2; k++) {
        p.tape_drive = (int16_t)(k ? 32767 : 2000);
        mfe_fx_state_t *f = fx_new();
        fill_sine(x, 0, N, 1000, 0.5);
        fx_run(f, &p, x, N);
        thd[k] = tone_mag(x, 0, N / 2, N / 2, 3000) / tone_mag(x, 0, N / 2, N / 2, 1000);
        free(f);
    }
    CHECK(thd[1] > 0.03 && thd[1] > 4 * thd[0], "TAPE saturates: 3rd harmonic %.4f at full drive, %.4f at low", thd[1], thd[0]);
    p.tape_drive = 16000;
    double out[2];
    for (int k = 0; k < 2; k++) {
        mfe_fx_state_t *f = fx_new();
        fill_sine(x, 0, N, 200, k ? 0.4 : 0.1);
        fx_run(f, &p, x, N);
        out[k] = rms(x, 0, N / 2, N / 2);
        free(f);
    }
    double gdb = 20 * log10(out[1] / out[0]);
    CHECK(gdb < 9.0 && gdb > 2.0, "TAPE compresses: +12 dB in gives %+.1f dB out", gdb);
    double hi[2];
    for (int k = 0; k < 2; k++) {
        p.tape_drive = 1000; p.tape_tone = (int16_t)(k ? 32767 : 0);
        mfe_fx_state_t *f = fx_new();
        fill_sine(x, 0, N, 10000, 0.1);
        fx_run(f, &p, x, N);
        hi[k] = rms(x, 0, N / 2, N / 2);
        free(f);
    }
    CHECK(hi[0] < 0.75 * hi[1], "TAPE tone: 10 kHz dark %.0f vs bright %.0f", hi[0], hi[1]);
    p.tape_tone = 16384; p.tape_drive = 8000; p.tape_wow = 32767;
    mfe_fx_state_t *f = fx_new();
    fill_sine(x, 0, N, 1000, 0.3);
    fx_run(f, &p, x, N);
    double s = 0;
    for (long w = N / 4; w + 2205 <= N; w += 2205) { double v = rms(x, 0, w, 2205); s = v > s ? v : s; }
    CHECK(s > 0 && rms(x, 0, N / 4, N - N / 4) > 0, "TAPE wow runs");
    free(f);
}

static void test_fx_ensemble(void)
{
    enum { N = 2 * MFE_SR };
    static int32_t x[2 * N];
    mfe_fx_t p;
    mfe_fx_defaults(&p);
    p.ens_mode = MFE_ENS_I; p.ens_mix = 32767;
    mfe_fx_state_t *f = fx_new();
    fill_sine(x, 0, N, 440, 0.5);
    fx_run(f, &p, x, N);
    double d = 0;
    for (long i = N / 4; i < N; i++) d += (double)(x[2 * i] - x[2 * i + 1]) * (x[2 * i] - x[2 * i + 1]);
    d = sqrt(d / (N - N / 4));
    CHECK(d > 0.1 * rms(x, 0, N / 4, N - N / 4), "ENSEMBLE I: L/R difference rms %.0f", d);
    double mn = 1e9, mx = 0;
    for (long w = N / 4; w + 2205 <= N; w += 2205) { double v = rms(x, 0, w, 2205); if (v < mn) mn = v; if (v > mx) mx = v; }
    CHECK(mx > 1.1 * mn, "ENSEMBLE I modulates: window rms %.0f .. %.0f", mn, mx);
    free(f);
    p.ens_mode = MFE_ENS_VIBRATO; p.ens_rate = 8000; p.ens_depth = 20000;
    f = fx_new();
    fill_sine(x, 0, N, 440, 0.5);
    fx_run(f, &p, x, N);
    double s = period_spread(x, 0, N / 4, N - N / 4);
    CHECK(s > 0.03, "VIBRATO pitch modulation: period spread %.3f", s);
    free(f);
    p.ens_mode = MFE_ENS_OFF;
    f = fx_new();
    fill_sine(x, 0, N, 440, 0.5);
    fx_run(f, &p, x, N);
    CHECK(period_spread(x, 0, N / 4, N - N / 4) < 0.002, "ENSEMBLE off: steady pitch");
    free(f);
}

static void test_fx_bbd(void)
{
    enum { N = 2 * MFE_SR };
    static int32_t x[2 * N];
    long w = (long)(0.03 * MFE_SR), at[3];
    for (int k = 0; k < 3; k++) at[k] = 100 + (long)(0.3 * (k + 1) * MFE_SR) - w / 2;
    mfe_fx_t p;
    mfe_fx_defaults(&p);
    p.bbd_mix = 32767; p.bbd_time = 9830; p.bbd_feedback = 16384; p.bbd_mod = 0; p.bbd_tone = 20000;   /* 300 ms */
    mfe_fx_state_t *f = fx_new();
    memset(x, 0, sizeof x);
    click(x, 100);
    fx_run(f, &p, x, N);
    long t1 = peak_at(x, 0, 100 + MFE_SR / 10, MFE_SR * 4 / 10);
    double ms = (t1 - 100) * 1000.0 / MFE_SR;
    CHECK(fabs(ms - 300) < 4, "BBD ECHO first repeat at %.1f ms (want 300)", ms);
    double e1 = rms(x, 0, at[0], w), e2 = rms(x, 0, at[1], w), e3 = rms(x, 0, at[2], w);
    CHECK(e2 < 0.8 * e1 && e3 < 0.8 * e2 && e3 > 0, "repeats decay with feedback: %.0f %.0f %.0f", e1, e2, e3);
    double h1 = hf_share(x, at[0], w), h2 = hf_share(x, at[1], w), h3 = hf_share(x, at[2], w);
    CHECK(h2 < h1 && h3 < h2, "each repeat darker: HF share %.4f %.4f %.4f", h1, h2, h3);
    free(f);
    p.bbd_feedback = 0;
    f = fx_new();
    memset(x, 0, sizeof x);
    click(x, 100);
    fx_run(f, &p, x, N);
    double z1 = rms(x, 0, at[0], w), z2 = rms(x, 0, at[1], w);
    CHECK(z2 < 0.01 * z1, "feedback 0: no second repeat (%.1f vs %.0f)", z2, z1);
    free(f);
    p.bbd_time = 32767; p.bbd_feedback = 0;     /* 1000 ms: clamped to the line, never wrapped */
    f = fx_new();
    memset(x, 0, sizeof x);
    click(x, 100);
    fx_run(f, &p, x, N);
    long tl = peak_at(x, 0, 100 + MFE_SR / 10, N - 100 - MFE_SR / 10);
    double want = (MFE_DELAY_LEN - 16 - MFE_DELAY_LEN / 50) * 1000.0 / 22050;
    if (want > 1000) want = 1000;
    CHECK(fabs((tl - 100) * 1000.0 / MFE_SR - want) < 5, "longest time: repeat at %.1f ms (want %.1f)", (tl - 100) * 1000.0 / MFE_SR, want);
    free(f);
    p.bbd_time = 9830; p.bbd_feedback = 32767; p.bbd_mod = 32767;   /* top feedback: runs away but stays held */
    f = fx_new();
    memset(x, 0, sizeof x);
    click(x, 100);
    fx_run(f, &p, x, N);
    int32_t pk = 0;
    for (long i = 0; i < 2 * N; i++) if (abs(x[i]) > pk) pk = abs(x[i]);
    CHECK(pk < 2 * 32768, "BBD ECHO at full feedback held below 2 FS (peak %d)", pk);
    free(f);
}

static void test_fx_oilcan(void)
{
    enum { N = MFE_SR };
    static int32_t x[2 * N];
    mfe_fx_t p;
    mfe_fx_defaults(&p);
    p.oil_mix = 32767; p.oil_time = 16384; p.oil_feedback = 0; p.oil_smear = 0;
    double sp[2];
    for (int k = 0; k < 2; k++) {
        p.oil_warble = (int16_t)(k ? 32767 : 0);
        mfe_fx_state_t *f = fx_new();
        memset(x, 0, sizeof x);
        fill_sine(x, 0, N / 2, 1000, 0.5);
        fx_run(f, &p, x, N);
        sp[k] = period_spread(x, 0, N / 2 + 200, (long)(0.11 * MFE_SR));   /* after the input: the repeat alone */
        CHECK(rms(x, 0, N / 2 + 200, (long)(0.11 * MFE_SR)) > 1000, "OIL CAN repeat present (warble %d)", k);
        free(f);
    }
    CHECK(sp[0] > 0.004 && sp[1] > 1.5 * sp[0], "OIL CAN wobble: period spread %.4f (warble 0), %.4f (full)", sp[0], sp[1]);
}

/* OIL CAN / BBD ECHO repeats and tempo sync (17 Nov 2026: "it should sync and it should have proper delay
 * repeats"): an impulse in gives distinct peaks at k * delay (+-2 %), each lower than the one before, for free and
 * synced times, in whichever build this is (desktop, or the FM1's half-rate MFE_OILCAN_LITE). */
static void thump(int32_t *lr, long at)
{
    for (long i = 0; i < 44; i++) { double h = sin(M_PI * i / 44); lr[2 * (at + i)] = lr[2 * (at + i) + 1] = (int32_t)(24000 * h * h); }
}
static void check_repeats(const char *what, const mfe_fx_t *p, unsigned which, uint32_t bpm_q8, int nrep)
{
    enum { N = 3 * MFE_SR, T0 = 1000 };
    static int32_t x[2 * N];
    unsigned div;
    double D = mfe_fx_delay_ms(p, which, bpm_q8, &div) * (MFE_SR / 1000.0), pk[8] = {0};
    mfe_fx_state_t *f = fx_new();
    mfe_fx_set_tempo(f, bpm_q8);
    memset(x, 0, sizeof x);
    thump(x, T0);
    fx_run(f, p, x, N);
    long ref = peak_at(x, 0, T0, 44);
    int ok_pos = 1, ok_decay = 1, ok_gap = 1;
    for (int k = 1; k <= nrep && ref + (k + 0.6) * D < N; k++)
        for (int c = 0; c < 2; c++) {
            long from = ref + (long)(k * D * 0.9), at = peak_at(x, c, from, (long)(k * D * 0.2));
            double err = fabs((at - ref) - k * D) / (k * D), a = abs(x[2 * at + c]);
            double gap = 0;
            for (long i = ref + (long)((k + 0.3) * D); i < ref + (long)((k + 0.7) * D); i++)
                if (abs(x[2 * i + c]) > gap) gap = abs(x[2 * i + c]);
            if (err > 0.02) { ok_pos = 0; printf("  %s ch%d repeat %d at %.1f ms (want %.1f)\n", what, c, k, (at - ref) * 1000.0 / MFE_SR, k * D * 1000.0 / MFE_SR); }
            if (k > 1 && !c && a >= pk[k - 1]) ok_decay = 0;
            if (gap > 0.25 * a) { ok_gap = 0; printf("  %s ch%d repeat %d: %.0f between repeats vs peak %.0f\n", what, c, k, gap, a); }
            if (!c) pk[k] = a;
        }
    CHECK(ok_pos, "%s: repeats at k x %.1f ms (+-2 %%)", what, D * 1000.0 / MFE_SR);
    CHECK(ok_decay && pk[nrep] > 0.03 * pk[1], "%s: %d repeats decaying, %.0f %.0f %.0f %.0f", what, nrep, pk[1], pk[2], pk[3], pk[4]);
    CHECK(ok_gap, "%s: repeats distinct (quiet between them)", what);
    free(f);
}
static void test_fx_delay_sync(void)
{
    mfe_fx_t p;
    unsigned div;
    mfe_fx_defaults(&p);
    CHECK(p.fx_sync == 0 && p.oil_smear < 8192, "delay defaults: free time, low smear");
    /* the time arithmetic: 1/8 at 120 BPM = 250 ms, 1/8 dotted = 375 ms, 1/4 at 90 = 667 ms */
    p.fx_sync = (uint8_t)((5 + 1) << 4);
    CHECK(mfe_fx_delay_ms(&p, 0, 120u << 8, &div) == 250 && div == 5, "BBD 1/8 at 120 BPM = 250 ms");
    p.fx_sync = 11u << 4;                                       /* 1 BAR: longer than any line */
    unsigned ms = mfe_fx_delay_ms(&p, 0, 120u << 8, &div);
    CHECK(div < 10 && ms <= (MFE_DELAY_LEN * 1000u) / 22050u && ms * 3 > (MFE_DELAY_LEN * 1000u) / 22050u,
          "BBD 1 BAR at 120 BPM does not fit: longest that fits = division %u, %u ms", div, ms);
    p.fx_sync = 7 + 1;
    ms = mfe_fx_delay_ms(&p, 1, 120u << 8, &div);
    printf("OIL CAN 1/8 dotted at 120 BPM: %u ms (division %u); free top %u ms\n", ms, div,
           (p.fx_sync = 0, p.oil_time = 32767, mfe_fx_delay_ms(&p, 1, 120u << 8, 0)));
#if MFE_OILCAN_RING >= 32768
    p.fx_sync = 7 + 1;
    CHECK(mfe_fx_delay_ms(&p, 1, 120u << 8, &div) == 375 && div == 7, "OIL CAN 1/8 dotted at 120 BPM = 375 ms");
#endif
    /* impulse responses */
    mfe_fx_defaults(&p);
    p.oil_mix = 32767;
    check_repeats("OIL CAN free 130 ms", &p, 1, 120u << 8, 4);
    p.oil_time = 26000;
    check_repeats("OIL CAN free long", &p, 1, 120u << 8, 4);
    p.fx_sync = 7 + 1;
    check_repeats("OIL CAN synced 1/8. at 120", &p, 1, 120u << 8, 4);
    p.fx_sync = 5 + 1;
    check_repeats("OIL CAN synced 1/8 at 140", &p, 1, 140u << 8, 4);
    p.fx_sync = 2 + 1;
    check_repeats("OIL CAN synced 1/16 at 100", &p, 1, 100u << 8, 4);
    mfe_fx_defaults(&p);
    p.bbd_mix = 32767; p.bbd_feedback = 16384;
    check_repeats("BBD ECHO free 300 ms", &p, 0, 120u << 8, 4);
    p.fx_sync = (5 + 1) << 4;
    check_repeats("BBD ECHO synced 1/8 at 120", &p, 0, 120u << 8, 4);
    p.fx_sync = (4 + 1) << 4;
    check_repeats("BBD ECHO synced 1/16. at 90", &p, 0, 90u << 8, 4);
    /* tempo changes glide the time: no clicks (a steady sine through, sample steps stay smooth) */
    for (unsigned which = 0; which < 2; which++) {
        enum { N = 3 * MFE_SR };
        static int32_t x[2 * N];
        mfe_fx_defaults(&p);
        if (which) { p.oil_mix = 26000; p.fx_sync = 5 + 1; } else { p.bbd_mix = 26000; p.fx_sync = (5 + 1) << 4; }
        mfe_fx_state_t *f = fx_new();
        fill_sine(x, 0, N, 220, 0.3);
        mfe_fx_set_tempo(f, 120u << 8);
        long h = N / 2 / MFE_BLOCK * MFE_BLOCK, end = h + (N - h) / MFE_BLOCK * MFE_BLOCK;
        fx_run(f, &p, x, h);
        mfe_fx_set_tempo(f, 97u << 8);
        fx_run(f, &p, x + 2 * h, N - h);
        double before = 0, after = 0;
        for (long i = MFE_SR; i < end; i++) {
            double d = fabs((double)x[2 * i] - x[2 * i - 2]);
            if (i < h) { if (d > before) before = d; } else if (d > after) after = d;
        }
        CHECK(after < 3.5 * before, "%s: tempo change 120 -> 97 BPM glides (largest step %.0f, before %.0f)",
              which ? "OIL CAN" : "BBD ECHO", after, before);
        free(f);
    }
    /* the engine passes its tempo on (mfe_set_tempo) */
    mfe_patch_t pa;
    mfe_patch_init(&pa);
    pa.fx.oil_mix = 32767; pa.fx.fx_sync = 5 + 1;
    mfe_engine_t *e = eng(&pa);
    mfe_set_tempo(e, 140u << 8);
    CHECK(e != 0, "engine with synced OIL CAN");
    free(e);
}

static void test_fx_reverb(void)
{
    enum { N = 4 * MFE_SR };
    static int32_t x[2 * N];
    mfe_fx_t p;
    mfe_fx_defaults(&p);
    p.rev_type = MFE_REV_TWIN_TANK; p.rev_amount = 26000;
    mfe_fx_state_t *f = fx_new();
    memset(x, 0, sizeof x);
    fx_run(f, &p, x, N);
    int silent = 1;
    for (long i = 0; i < 2 * N; i++) if (x[i]) silent = 0;
    CHECK(silent, "TWIN TANK: silence in, exact silence out");
    for (long i = 1000; i < 1000 + 441; i++) x[2 * i] = x[2 * i + 1] = (int32_t)(16000 * sin(i * 0.31) * sin(i * 0.0071));
    fx_run(f, &p, x, N);
    double l = rms(x, 0, (long)(0.75 * MFE_SR), MFE_SR / 2), r = rms(x, 1, (long)(0.75 * MFE_SR), MFE_SR / 2);
    double l0 = rms(x, 0, (long)(0.1 * MFE_SR), MFE_SR / 2);
    CHECK(l > 0 && l < l0, "TWIN TANK tail decays (%.0f -> %.0f)", l0, l);
    CHECK(l > 1.5 * r, "TWIN TANK: long tank left outlasts the short right (%.1f vs %.1f at 0.75-1.25 s)", l, r);
    double lo = tone_mag(x, 0, 2000, MFE_SR, 50), mid = tone_mag(x, 0, 2000, MFE_SR, 1200), hi = tone_mag(x, 0, 2000, MFE_SR, 12000);
    CHECK(mid > 4 * lo && mid > 10 * hi, "TWIN TANK band-limited: 50 Hz %.2f, 1.2 kHz %.2f, 12 kHz %.2f", lo, mid, hi);
    free(f);
    double tail[2];
    for (int h = 0; h < 2; h++) {
        p.rev_type = MFE_REV_SPRING; p.rev_amount = (int16_t)(h ? 20000 : 12000); p.rev_hold = (uint8_t)h;
        f = fx_new();
        memset(x, 0, sizeof x);
        fill_sine(x, 1000, MFE_SR / 4, 300, 0.4);
        fx_run(f, &p, x, N);
        tail[h] = rms(x, 0, 3 * MFE_SR, MFE_SR / 2);
        if (!h) CHECK(rms(x, 0, MFE_SR / 3, MFE_SR / 4) > 10 * tail[0], "SPRING tail decays (%.1f late)", tail[0]);
        free(f);
    }
    CHECK(tail[1] > 10 * tail[0] + 50, "SPRING HOLD: sustained howl %.0f vs %.1f", tail[1], tail[0]);
}

static void test_fx_bounded(void)
{
    enum { N = MFE_SR / 2 };
    static int32_t x[2 * N];
    uint32_t r = 4242;
    int32_t worst = 0;
    mfe_fx_state_t *f = fx_new();
    for (int k = 0; k < 120; k++) {
        mfe_fx_t p;
        uint8_t *u = (uint8_t *)&p;
        for (size_t i = 0; i < sizeof p; i++) u[i] = (uint8_t)mfe_rand(&r);
        if (k % 4 == 0) {           /* every knob at the top, feedback and hold included */
            int16_t *s = (int16_t *)&p;
            for (size_t i = 0; i < sizeof p / 2; i++) s[i] = 32767;
            p.ens_mode = (uint8_t)(k / 4 % MFE_ENS_COUNT); p.rev_type = (uint8_t)(k / 4 % 2); p.rev_hold = 1;
        }
        for (long i = 0; i < 2 * N; i++) x[i] = i < N ? (int32_t)(mfe_rand(&r) % 131072) - 65536 : 0;   /* +-2 FS, then silence */
        fx_run(f, &p, x, N);
        for (long i = 0; i < 2 * N; i++) if (abs(x[i]) > worst) worst = abs(x[i]);
    }
    CHECK(worst < 16 * 32768, "random and extreme effect settings stay bounded (peak %.2f FS)", worst / 32768.0);
    free(f);
    printf("FX state: %zu B (MFE_DELAY_LEN %d)\n", mfe_fx_size(), MFE_DELAY_LEN);
}

static void test_fx_drive_mod(void)
{
    /* MFE_D_DRIVE drives the master TAPE: macro 1 -> DRIVE turns it on with the patch drive at 0 */
    mfe_patch_t p = sine_patch();
    p.mod[0].src = MFE_S_MACRO1; p.mod[0].dst = MFE_D_DRIVE; p.mod[0].amount = 32767;
    p.osc[0].amp = 32767;
    static float a[MFE_BLOCK * 64], b[MFE_BLOCK * 64];
    mfe_engine_t *e = eng(&p);
    mfe_note_on(e, 57, 127);
    run(e, 64, a);
    free(e);
    p.macro[0] = 32767;
    e = eng(&p);
    mfe_note_on(e, 57, 127);
    run(e, 64, b);
    free(e);
    double d = 0;
    for (int i = MFE_BLOCK * 32; i < MFE_BLOCK * 64; i++) d += fabs(a[i] - b[i]);
    CHECK(d > 100, "macro -> DRIVE changes the sound through TAPE (%.1f)", d);
}

static void test_preset_v4_effects(void)
{
    /* a v4 preset (old effects block) reads into the new effects */
    mfe_patch_t a, b;
    mfe_patch_init(&a);
    uint8_t buf[1024];
    size_t n = mfe_preset_write(&a, buf, sizeof buf);
    /* build the v4 image by hand: same fields up to the mods, the old 10-value FX block, then the mod osc */
    size_t fx_at = n - 4 - 14 - 48;           /* v5: ... mods | fx (48 B) | mod osc (14 B) | crc */
    uint8_t v4[1024];
    memcpy(v4, buf, fx_at);
    int16_t old[10] = {5000, 12000, 1, 1, 9000, 20000, 15000, 7000, 1, 1};
    for (int k = 0; k < 10; k++) { v4[fx_at + 2 * k] = (uint8_t)old[k]; v4[fx_at + 2 * k + 1] = (uint8_t)(old[k] >> 8); }
    memcpy(v4 + fx_at + 20, buf + n - 4 - 14, 14);
    size_t n4 = fx_at + 20 + 14 + 4;
    v4[4] = 4; v4[5] = 0; v4[6] = (uint8_t)(n4 - 12); v4[7] = (uint8_t)((n4 - 12) >> 8);
    uint32_t c = 0xFFFFFFFFu;
    for (size_t i = 0; i < n4 - 4; i++) { c ^= v4[i]; for (int k = 0; k < 8; k++) c = (c >> 1) ^ (0xEDB88320u & (0u - (c & 1u))); }
    c = ~c;
    for (int k = 0; k < 4; k++) v4[n4 - 4 + k] = (uint8_t)(c >> (8 * k));
    CHECK(n4 == 480 && mfe_preset_read(&b, v4, n4) == 0, "v4 preset (480 B) still reads");
    CHECK(b.fx.tape_drive == 5000 && b.fx.ens_mode == MFE_ENS_I && b.fx.ens_mix == 12000 && b.fx.bbd_mix == 9000 &&
          b.fx.bbd_time == 10000 && b.fx.bbd_feedback == 15000 && b.fx.rev_type == MFE_REV_TWIN_TANK && b.fx.rev_amount == 7000 &&
          b.fx.oil_mix == 0 && b.fx.fuzz_amount == 0 && b.fx.tape_level == 16384,
          "v4 effects mapped: drive->TAPE, chorus->ENSEMBLE I, delay->BBD (time halved), reverb->TWIN TANK");
}

static void test_random_patches(void)
{
    /* 200 random patches: output stays finite and bounded, voices end after release */
    uint32_t r = 7;
    for (int k = 0; k < 200; k++) {
        mfe_patch_t p;
        uint8_t *b = (uint8_t *)&p;
        for (size_t i = 0; i < sizeof p; i++) b[i] = (uint8_t)mfe_rand(&r);
        for (int i = 0; i < MFE_NOSC; i++) p.osc[i].table %= MFE_WT_COUNT;
        for (int i = 0; i < MFE_NFILT; i++) p.filt[i].type %= MFE_F_COUNT;
        for (int i = 0; i < MFE_NLFO; i++) p.lfo[i].wave %= MFE_W_COUNT;
        for (int i = 0; i < MFE_NMOD; i++) { p.mod[i].src %= MFE_S_COUNT; p.mod[i].dst %= MFE_D_COUNT; }
        p.voice_mode %= 3;
        p.env[3].release = (int16_t)(abs(p.env[3].release) % 20000);
        p.env[3].flags = 0;
        p.glide = (int16_t)(abs(p.glide) % 20000);
        mfe_engine_t *e = eng(&p);
        for (int i = 0; i < 4; i++) mfe_note_on(e, (uint8_t)(36 + mfe_rand(&r) % 60), 100);
        run(e, 200, 0);
        mfe_all_notes_off(e);
        run(e, 3 * MFE_SR / MFE_BLOCK, 0);
        CHECK(mfe_active_voices(e) == 0, "random patch %d: %u voices left", k, mfe_active_voices(e));
        free(e);
    }
}

int main(void)
{
    mfe_init_tables();
    test_pitch();
    test_band_limit();
    test_envelope();
    test_filters();
    test_ladder();
    test_acid();
    test_modulation();
    test_voices();
    test_performer_stepper();
    test_moog_features();
    test_modosc();
    test_live_macros();
    test_preset();
    test_fx_bypass();
    test_fx_fuzz();
    test_fx_tape();
    test_fx_ensemble();
    test_fx_bbd();
    test_fx_oilcan();
    test_fx_delay_sync();
    test_fx_reverb();
    test_fx_bounded();
    test_fx_drive_mod();
    test_preset_v4_effects();
    test_random_patches();
    printf("%d checks, %d failed\n", checks, fails);
    return fails != 0;
}
