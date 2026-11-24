// SPDX-License-Identifier: AGPL-3.0-only
// Vintage Chonk plugin: the host-automatable parameters and the patch fields behind them.
// Every parameter is a patch field (int16 Q15 / Q13); its host value is 0..1 and maps to the field
// exactly (int -> value -> int is the identity), so a patch loaded from the panel and the host's view
// of it never drift apart.
#pragma once
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <string>
extern "C" {
#include "mfe.h"
}

namespace mfm1 {

enum class Curve { linear, square };   // square: field = 32767 v^2 (the panel's VOLUME knob)
enum class Show { percent, cutoffHz, volumeDb, glideTime };

struct ParamDef
{
    const char* id;
    const char* name;
    int16_t* (*field) (mfe_patch_t&);
    Curve curve;
    Show show;
};

#define MFM1_F(expr) [] (mfe_patch_t& p) -> int16_t* { return &(expr); }
inline const ParamDef paramDefs[] = {
    { "macro1", "Macro 1", MFM1_F (p.macro[0]), Curve::linear, Show::percent },
    { "macro2", "Macro 2", MFM1_F (p.macro[1]), Curve::linear, Show::percent },
    { "macro3", "Macro 3", MFM1_F (p.macro[2]), Curve::linear, Show::percent },
    { "macro4", "Macro 4", MFM1_F (p.macro[3]), Curve::linear, Show::percent },
    { "macro5", "Macro 5", MFM1_F (p.macro[4]), Curve::linear, Show::percent },
    { "macro6", "Macro 6", MFM1_F (p.macro[5]), Curve::linear, Show::percent },
    { "macro7", "Macro 7", MFM1_F (p.macro[6]), Curve::linear, Show::percent },
    { "macro8", "Macro 8", MFM1_F (p.macro[7]), Curve::linear, Show::percent },
    { "master", "Master Volume", MFM1_F (p.master), Curve::square, Show::volumeDb },
    { "f1_cutoff", "Filter 1 Cutoff", MFM1_F (p.filt[0].cutoff), Curve::linear, Show::cutoffHz },
    { "f1_res", "Filter 1 Emphasis", MFM1_F (p.filt[0].res), Curve::linear, Show::percent },
    { "f2_cutoff", "Filter 2 Cutoff", MFM1_F (p.filt[1].cutoff), Curve::linear, Show::cutoffHz },
    { "mo_rm", "Mod Osc Ring", MFM1_F (p.modosc.rm), Curve::linear, Show::percent },
    { "mo_pm", "Mod Osc Phase", MFM1_F (p.modosc.pm), Curve::linear, Show::percent },
    { "mo_pos", "Mod Osc Wave", MFM1_F (p.modosc.pos), Curve::linear, Show::percent },
    { "fx_fuzz", "Oct Fuzz Amount", MFM1_F (p.fx.fuzz_amount), Curve::linear, Show::percent },
    { "fx_tape", "Tape Drive", MFM1_F (p.fx.tape_drive), Curve::linear, Show::percent },
    { "fx_ensemble", "Ensemble Mix", MFM1_F (p.fx.ens_mix), Curve::linear, Show::percent },
    { "fx_bbd", "BBD Echo Mix", MFM1_F (p.fx.bbd_mix), Curve::linear, Show::percent },
    { "fx_oilcan", "Oil Can Mix", MFM1_F (p.fx.oil_mix), Curve::linear, Show::percent },
    { "fx_reverb", "Reverb Amount", MFM1_F (p.fx.rev_amount), Curve::linear, Show::percent },
    { "glide", "Glide", MFM1_F (p.glide), Curve::linear, Show::glideTime },
};
#undef MFM1_F
constexpr int numParams = (int) (sizeof (paramDefs) / sizeof (paramDefs[0]));

inline int16_t fieldFromValue (const ParamDef& d, float v)
{
    v = v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v);
    const double x = d.curve == Curve::square ? (double) v * v : (double) v;
    return (int16_t) std::lround (x * 32767.0);
}

inline float valueFromField (const ParamDef& d, int16_t f)
{
    const double x = (f < 0 ? 0 : f) / 32767.0;
    return (float) (d.curve == Curve::square ? std::sqrt (x) : x);
}

inline std::string paramText (const ParamDef& d, float v)
{
    char b[48];
    const double q = fieldFromValue (d, v) / 32767.0;
    switch (d.show)
    {
        case Show::cutoffHz:
        {
            const double hz = 440.0 * std::pow (2.0, (q * 136.0 - 69.0) / 12.0);
            if (hz >= 1000.0) std::snprintf (b, sizeof b, "%.2f kHz", hz / 1000.0);
            else std::snprintf (b, sizeof b, "%.0f Hz", hz);
            break;
        }
        case Show::volumeDb:
        {
            const double g = 4.0 * q;    // Q13 gain: 32767 = 4.0
            if (g <= 0.0001) std::snprintf (b, sizeof b, "-inf dB");
            else std::snprintf (b, sizeof b, "%.1f dB", 20.0 * std::log10 (g));
            break;
        }
        case Show::glideTime:
        {
            if (q <= 0.0) { std::snprintf (b, sizeof b, "off"); break; }
            const double ms = 0.5 * std::pow (MFE_TIME_RANGE, q);
            if (ms >= 1000.0) std::snprintf (b, sizeof b, "%.2f s", ms / 1000.0);
            else std::snprintf (b, sizeof b, "%.0f ms", ms);
            break;
        }
        default: std::snprintf (b, sizeof b, "%.0f %%", q * 100.0); break;
    }
    return b;
}

}
