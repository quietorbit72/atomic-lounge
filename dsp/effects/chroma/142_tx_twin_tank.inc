/* Copied into Vintage Chonk on 17 Nov 2026 from chroma-mod/fx/texture_reverbs/142_tx_twin_tank.c
 * (the developer's own Chroma Console effects project). MFE changes, each marked "MFE:": CH_T on loop delays, RT60s and wobble depths, CH_INC on the short tank's wobble rates. */
/* Texture 2 (Mod bank): Twin Tank Stereo (Strange Gear Catalog No. 3), Texture-module version.
 * Two mismatched spring tanks side by side, as when a player ran an amp's built-in short tank and an
 * outboard long tank at once: a long THREE-spring tank (35/41/47 ms, 3.2 s decay, darker, 10 all-pass
 * stages per pass) on the LEFT and a short TWO-spring tank (23/29 ms, 1.6 s, brighter, 12 stages) on the
 * RIGHT, slightly detuned, so the drips arrive at different times and argue across the stereo field.
 * Both tanks share one driver (with its transducer resonance) and each output has the pickup resonance.
 * Texture has ONE knob, so AMOUNT is a macro: dwell (how hard the driver shakes the springs; the 6K6-style
 * driver saturates near the top) and wet level rise together. 0 = dry, bit for bit.
 * Fixed (TIME / DRIFT on the catalog page): balance centred, tanks detuned about 1.8 % apart.
 * Texture is stereo in place, so the dry stays L/R and the long tank adds left, the short tank right. */
#include "_spring_core.h"

typedef struct {
    SpTank a, b;
    SpXdcr xd;
    int32_t hp8, lp1, tn1[2], tn2[2], tone[2], up[2];
    int32_t dwell, dwell_t, bias0, mix, mix_t;
} TxTwinTank;

static const uint32_t tt_la[3] = {840, 984, 1128}, tt_lb[2] = {552, 696};
enum { TT_DET = 595, TT_GAB = 24576 };    /* detune (Q15 fraction, catalog DRIFT 30000); balance gain at centre */

static int tt_attach(void *v, int16_t *mem, uint32_t words) {
    TxTwinTank *s = (TxTwinTank *)v;
    if (!mem || words < 5u * SP_RING) return 0;
    sp_init(&s->a, mem, 3, tt_la);
    sp_init(&s->b, mem + 3 * SP_RING, 2, tt_lb);
    s->a.nl = 10;                       /* fewer stages per pass, three springs */
    s->b.nl = 12;
    s->a.damp_a = 16000;                /* long tank darker (about 2.7 kHz per pass) */
    s->b.damp_a = 23000;                /* short tank brighter */
    s->b.inc[0] = CH_INC(205000u); s->b.inc[1] = CH_INC(251000u);   /* MFE: CH_INC */
    for (int i = 0; i < 3; ++i) s->a.dts[i] = s->a.dt[i] = s->a.d[i] = (int32_t)(CH_T(tt_la[i]) * (uint32_t)(32768 - TT_DET) >> 7);   /* MFE: CH_T */
    for (int i = 0; i < 2; ++i) s->b.dts[i] = s->b.dt[i] = s->b.d[i] = (int32_t)(CH_T(tt_lb[i]) * (uint32_t)(32768 + TT_DET) >> 7);   /* MFE: CH_T */
    sp_decay(&s->a, CH_T(76800));       /* 3.2 s (MFE: CH_T) */
    sp_decay(&s->b, CH_T(38400));       /* 1.6 s */
    s->a.wob_t = (int32_t)CH_T(600 + TT_DET * 4);   /* MFE: CH_T */
    s->b.wob_t = (int32_t)CH_T(500 + TT_DET * 5);
    s->dwell = s->dwell_t = 2867;
    s->bias0 = fx_tanhish(4000);
    return 1;
}

static void tt_control(void *v, const FxControls *c) {
    TxTwinTank *s = (TxTwinTank *)v;
    s->dwell_t = 2867 + (int32_t)(((uint32_t)c->k2 * 21709u) >> 16);
    s->mix_t = (tx_mix_curve(c->k2) * 30000) >> 15;
}

static void tt_process(void *v, int32_t pcm[32]) {
    TxTwinTank *s = (TxTwinTank *)v;
    int32_t drive[MOD_FRAMES], o[2][SP_N], y48[MOD_FRAMES];
    for (int n = 0; n < MOD_FRAMES; ++n) {
        int32_t x = (m_in16(pcm[n]) + m_in16(pcm[MOD_FRAMES + n])) >> 1;
        x -= m_lpq(&s->hp8, x, 790);
        x = m_lp(&s->lp1, x, 16000);
        s->dwell += (s->dwell_t - s->dwell) >> 8;
        drive[n] = (fx_tanhish(((x * s->dwell) >> 12) + 4000) - s->bias0) >> 1;
    }
    s->mix = m_slew(s->mix, s->mix_t, 5);
    int32_t d24[SP_N];
    sp_down(&s->a, drive, d24);                                      /* one decimator feeds both tanks */
    sp_driver24(&s->xd, d24);
    sp_tank(&s->a, d24, 0);
    sp_tank(&s->b, d24, 0);
    for (int n = 0; n < SP_N; ++n) {
        int32_t la = (s->a.out[0][n] + s->a.out[1][n] + s->a.out[2][n]) * 3 >> 2;
        int32_t sb = s->b.out[0][n] + s->b.out[1][n];
        la = (la * TT_GAB) >> 15;
        sb = (sb * TT_GAB) >> 15;
        o[0][n] = sp_pickup(&s->xd, 0, m_lp(&s->tn2[0], m_lp(&s->tn1[0], la + (sb >> 3), 25000), 25000));
        o[1][n] = sp_pickup(&s->xd, 1, m_lp(&s->tn2[1], m_lp(&s->tn1[1], sb + (la >> 3), 25000), 25000));
    }
    for (int ch = 0; ch < 2; ++ch) {
        sp_up(&s->up[ch], o[ch], y48);
        for (int n = 0; n < MOD_FRAMES; ++n) {
            int32_t y = m_lp(&s->tone[ch], y48[n], 20000);
            pcm[ch * MOD_FRAMES + n] = tx_add(pcm[ch * MOD_FRAMES + n], m_knee((y * 21000) >> 15, 24000), s->mix);
        }
    }
}

const FxDesc fx_tx_twin_tank = {
    142, "tx_twin_tank", "Twin Tank Stereo (Texture)", FX_TEXT, 0, sizeof(TxTwinTank), 5 * SP_RING,
    tt_attach, tt_control, tt_process,
    0, 44000, 0, 0,
    "AMOUNT: dwell and wet level together (long 3-spring tank left, short 2-spring tank right)"
};
