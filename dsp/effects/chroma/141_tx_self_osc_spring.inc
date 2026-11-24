/* Copied into Vintage Chonk on 17 Nov 2026 from chroma-mod/fx/texture_reverbs/141_tx_self_osc_spring.c
 * (the developer's own Chroma Console effects project). MFE changes, each marked "MFE:": CH_T on the RT60 and wobble depth. */
/* Texture 1 (Mod bank): Self-Oscillating Spring (Strange Gear Catalog No. 2), Texture-module version.
 * A 2-spring tank whose pickup is fed back into its own driver through a limiter, as players and sound
 * artists have done with real tanks (output patched back to input): the springs ring at their own
 * dispersive modes, and past unity loop gain they sing, the limiter holding the howl at a steady level.
 * Texture has ONE knob, so AMOUNT is a macro:
 *   0 = dry (bit for bit); up to ~3/4 = more spring in the mix and a longer, ringing feedback loop;
 *   above ~3/4 the loop passes unity and the tank sings (limited), still mixed under the dry guitar.
 * Fixed (would have been TIME / DRIFT on the catalog page): spring length 1.0x, moderate wobble.
 * HOLD: open the loop: a sustained spring howl from what you last played (swells in, falls back on release).
 * Output: dry + wet, stereo (the two springs spread L/R). */
#include "_spring_core.h"

typedef struct {
    SpTank t;
    SpXdcr xd;
    int32_t hp8, lp1, tone[2], tn1[2], tn2[2], up[2];
    int32_t lg, lg_t, env, lim, hold_gain, mix, mix_t;
    int32_t fbk[SP_N];
    int hold;
} TxSelfOsc;

static int so_attach(void *v, int16_t *mem, uint32_t words) {
    TxSelfOsc *s = (TxSelfOsc *)v;
    static const uint32_t dl[2] = {792, 984};
    if (!mem || words < 2u * SP_RING) return 0;
    sp_init(&s->t, mem, 2, dl);
    sp_decay(&s->t, CH_T(72000));   /* MFE: CH_T */                                         /* the tank alone: RT60 3 s */
    s->t.wob_t = (int32_t)CH_T(450 + ((20000 * 8500) >> 16));   /* MFE: CH_T */                      /* fixed moderate wobble */
    s->lim = 32767;
    return 1;
}

static void so_control(void *v, const FxControls *c) {
    TxSelfOsc *s = (TxSelfOsc *)v;
    s->lg_t = (int32_t)(((uint32_t)c->k2 * 7000u) >> 16);           /* external loop gain 0 .. 0.21 (unity near 3/4) */
    s->mix_t = (tx_mix_curve(c->k2) * 27000) >> 15;                 /* wet up to ~0.82 */
    s->hold = c->hold;
}

static void so_process(void *v, int32_t pcm[32]) {
    TxSelfOsc *s = (TxSelfOsc *)v;
    int32_t drive[MOD_FRAMES], o[2][SP_N], y48[MOD_FRAMES];
    for (int n = 0; n < MOD_FRAMES; ++n) {
        int32_t x = (m_in16(pcm[n]) + m_in16(pcm[MOD_FRAMES + n])) >> 1;
        x -= m_lpq(&s->hp8, x, 790);
        x = m_lp(&s->lp1, x, 16000);
        drive[n] = fx_tanhish(x) >> 1;
    }
    s->hold_gain = m_slew(s->hold_gain, s->hold ? 32767 : 0, 8);
    s->lg = m_slew(s->lg, s->lg_t + ((((int32_t)7600 - s->lg_t) * s->hold_gain) >> 15), 6);
    s->mix = m_slew(s->mix, s->mix_t + (((27000 - s->mix_t) * s->hold_gain) >> 15), 5);
    sp_run_x(&s->t, &s->xd, drive, s->fbk);
    /* Limiter on the pickup (instant attack, about 170 ms release), then back into the springs. */
    int32_t pk = 0;
    for (int n = 0; n < SP_N; ++n) {
        int32_t m = s->t.out[0][n] + s->t.out[1][n];
        int32_t a = m < 0 ? -m : m;
        if (a > pk) pk = a;
    }
    if (pk > s->env) s->env = pk; else s->env -= (s->env >> 9) + 1;
    if (s->env < 0) s->env = 0;
    int32_t lim_t = s->env > 9000 ? (int32_t)((9000u << 15) / (uint32_t)s->env) : 32767;
    s->lim = lim_t < s->lim ? lim_t : m_slew(s->lim, lim_t, 5);
    for (int n = 0; n < SP_N; ++n) {
        int32_t a = s->t.out[0][n], b = s->t.out[1][n];
        int32_t m = m_sat16(sp_g(m_sat16(a + b), s->lim));
        s->fbk[n] = sp_g(m, s->lg);
        a = sp_g(a, s->lim);
        b = sp_g(b, s->lim);
        o[0][n] = sp_pickup(&s->xd, 0, m_lp(&s->tn2[0], m_lp(&s->tn1[0], a + (b >> 1), 25000), 25000));
        o[1][n] = sp_pickup(&s->xd, 1, m_lp(&s->tn2[1], m_lp(&s->tn1[1], b + (a >> 1), 25000), 25000));
    }
    for (int ch = 0; ch < 2; ++ch) {
        sp_up(&s->up[ch], o[ch], y48);
        for (int n = 0; n < MOD_FRAMES; ++n) {
            int32_t y = m_lp(&s->tone[ch], y48[n], 20000);
            pcm[ch * MOD_FRAMES + n] = tx_add(pcm[ch * MOD_FRAMES + n], m_knee(y, 24000), s->mix);
        }
    }
}

const FxDesc fx_tx_self_osc_spring = {
    141, "tx_self_osc_spring", "Self-Oscillating Spring (Texture)", FX_TEXT, 0, sizeof(TxSelfOsc), 2 * SP_RING,
    so_attach, so_control, so_process,
    0, 40000, 0, 1,
    "AMOUNT: spring mix and loop gain together (sings above ~3/4) · HOLD open the loop: sustained spring howl"
};
