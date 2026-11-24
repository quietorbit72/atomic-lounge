// SPDX-License-Identifier: AGPL-3.0-only
// Atomic Lounge plugin (formerly Vintage Chonk), ORBIT mode: settings, host parameter mapping, serialising, sample decoding and root
// detection, Chonk preset references, factory and user presets (see OrbitModel.h).
#include "OrbitModel.h"
#include <juce_audio_formats/juce_audio_formats.h>
#include <algorithm>
#include <cmath>
#include <mutex>
#include "PresetStore.h"
#include "factory_presets.h"

namespace orbit {

// ----------------------------------------------------------------------------------- settings ---
Settings::Settings()
{
    for (int i = 0; i < numCore; i++) core[(size_t) i] = mfo_param_info (i)->def;
    for (int l = 0; l < layers; l++)
    {
        src[l].kind = l == 0 ? Src::synth : Src::off;
        src[l].patch.name = "Velvet Pad";
        imp[l].kind = Imp::sample;
        imp[l].from = SampleFrom::generated;
        imp[l].genType = l == 0 ? MFO_GEN_PLATE : l == 1 ? MFO_GEN_SPRING : MFO_GEN_GLASS;
        imp[l].genSeconds = 2.5f;
        imp[l].seed = (uint32_t) (11 + l * 7);
        imp[l].liveLayer = (l + 1) % layers;
        layerName[l] = "LAYER " + juce::String (l + 1);
        core[(size_t) MFO_LPARAM (l, MFO_L_BODY)] = 0.6f;
    }
    core[MFO_P_PUCK_X] = -0.45f;
    core[MFO_P_PUCK_Y] = 0.45f;
    core[MFO_P_MASTER] = 0.8f;
    drawn.n = 0;
}

// --------------------------------------------------------------------------- host parameters ---
const std::vector<HostParam>& hostParams()
{
    static const std::vector<HostParam> list = []
    {
        std::vector<HostParam> v;
        v.push_back ({ "orbit_puck_x", "ORBIT Puck X", MFO_P_PUCK_X, Map::lin });
        v.push_back ({ "orbit_puck_y", "ORBIT Puck Y", MFO_P_PUCK_Y, Map::lin });
        v.push_back ({ "orbit_rate", "ORBIT Rate", spRate, Map::special });
        v.push_back ({ "orbit_wobble", "ORBIT Wobble", MFO_P_ORBIT_WOBBLE, Map::lin });
        v.push_back ({ "orbit_spread", "ORBIT Spread", spSpread, Map::special });
        v.push_back ({ "orbit_drift", "ORBIT Drift", MFO_P_DRIFT, Map::lin });
        v.push_back ({ "orbit_tape_age", "ORBIT Tape Age", spTapeAge, Map::special });
        v.push_back ({ "orbit_output", "ORBIT Output", MFO_P_MASTER, Map::sq2 });
        static const char* dims[5] = { "Body", "Tone", "Grain", "Motion", "Pitch" };
        for (int l = 0; l < layers; l++)
        {
            const juce::String p = "orbit_l" + juce::String (l + 1) + "_", n = "ORBIT L" + juce::String (l + 1) + " ";
            v.push_back ({ p + "level", n + "Level", MFO_LPARAM (l, MFO_L_LEVEL), Map::sq2 });
            for (int d = 0; d < 5; d++)
                v.push_back ({ p + juce::String (dims[d]).toLowerCase(), n + dims[d], MFO_LPARAM (l, MFO_L_BODY + d), Map::lin });
            v.push_back ({ p + "cutoff", n + "Cutoff", MFO_LPARAM (l, MFO_L_CUTOFF), Map::log });
            v.push_back ({ p + "res", n + "Resonance", MFO_LPARAM (l, MFO_L_RESO), Map::lin });
        }
        static const char* fx[numFx] = { "fuzz", "tape", "ensemble", "bbd", "oilcan", "reverb" };
        static const char* fxn[numFx] = { "Oct Fuzz", "Tape", "Ensemble", "BBD Echo", "Oil Can", "Reverb" };
        for (int i = 0; i < numFx; i++) v.push_back ({ juce::String ("orbit_fx_") + fx[i], juce::String ("ORBIT FX ") + fxn[i], spFx0 + i, Map::special });
        for (int i = 0; i < numMacros; i++)
            v.push_back ({ "orbit_macro" + juce::String (i + 1), "ORBIT Macro " + juce::String (i + 1), spMacro0 + i, Map::special });
        // 1.2.0: held orbit, live input (appended, so the 1.1.0 indices and ids stay)
        v.push_back ({ "orbit_motion", "ORBIT Motion", spMotion, Map::special, true });
        v.push_back ({ "orbit_puck_drift", "ORBIT Puck Drift", spPuckDrift, Map::special });
        v.push_back ({ "orbit_input_trim", "ORBIT Input Trim", spInTrim, Map::special });
        for (int l = 0; l < layers; l++)
            v.push_back ({ "orbit_l" + juce::String (l + 1) + "_src_input", "ORBIT L" + juce::String (l + 1) + " Source Input", spSrcIn0 + l, Map::special, true });
        for (int l = 0; l < layers; l++)
            v.push_back ({ "orbit_l" + juce::String (l + 1) + "_imp_input", "ORBIT L" + juce::String (l + 1) + " Impulse Input", spImpIn0 + l, Map::special, true });
        for (int l = 0; l < layers; l++)
            v.push_back ({ "orbit_l" + juce::String (l + 1) + "_input_gate", "ORBIT L" + juce::String (l + 1) + " Input Gate", spGate0 + l, Map::special, true });
        return v;
    }();
    return list;
}

int hostIndexForSpecial (int special)
{
    const auto& h = hostParams();
    for (int i = 0; i < (int) h.size(); i++)
        if (h[(size_t) i].map == Map::special && h[(size_t) i].target == special) return i;
    return -1;
}

int hostIndexForCore (int coreId)
{
    const auto& h = hostParams();
    for (int i = 0; i < (int) h.size(); i++)
        if (h[(size_t) i].map != Map::special && h[(size_t) i].target == coreId) return i;
    return -1;
}

float coreFromNorm (const HostParam& h, float v)
{
    const auto* in = mfo_param_info (h.target);
    v = juce::jlimit (0.0f, 1.0f, v);
    switch (h.map)
    {
        case Map::sq2: return in->max * v * v;
        case Map::log: return in->min * std::pow (in->max / in->min, v);
        default: return in->min + v * (in->max - in->min);
    }
}

float normFromCore (const HostParam& h, float c)
{
    const auto* in = mfo_param_info (h.target);
    switch (h.map)
    {
        case Map::sq2: return std::sqrt (juce::jlimit (0.0f, 1.0f, c / in->max));
        case Map::log: return juce::jlimit (0.0f, 1.0f, std::log (std::max (c, in->min) / in->min) / std::log (in->max / in->min));
        default: return juce::jlimit (0.0f, 1.0f, (c - in->min) / (in->max - in->min));
    }
}

float hostNormFromSettings (const HostParam& h, const Settings& s)
{
    if (h.map != Map::special) return normFromCore (h, s.core[(size_t) h.target]);
    if (h.target == spRate) return s.rate;
    if (h.target == spSpread) return s.spread;
    if (h.target == spTapeAge) return s.tapeAge;
    if (h.target >= spFx0 && h.target < spFx0 + numFx) return s.fxAmt[h.target - spFx0];
    if (h.target >= spMacro0 && h.target < spMacro0 + numMacros) return s.macro[h.target - spMacro0];
    if (h.target == spMotion) return s.path != pathHold ? 1.0f : 0.0f;
    if (h.target == spPuckDrift) return s.puckDrift;
    if (h.target == spInTrim) return juce::jlimit (0.0f, 1.0f, 0.5f + s.inTrimDb / (2.0f * inTrimRange));
    if (h.target >= spSrcIn0 && h.target < spSrcIn0 + layers) return s.srcInput[h.target - spSrcIn0] ? 1.0f : 0.0f;
    if (h.target >= spImpIn0 && h.target < spImpIn0 + layers) return s.impInput[h.target - spImpIn0] ? 1.0f : 0.0f;
    if (h.target >= spGate0 && h.target < spGate0 + layers) return s.inGate[h.target - spGate0] ? 1.0f : 0.0f;
    return 0.0f;
}

void hostNormToSettings (const HostParam& h, float v, Settings& s)
{
    v = juce::jlimit (0.0f, 1.0f, v);
    if (h.map != Map::special) s.core[(size_t) h.target] = coreFromNorm (h, v);
    else if (h.target == spRate) s.rate = v;
    else if (h.target == spSpread) s.spread = v;
    else if (h.target == spTapeAge) s.tapeAge = v;
    else if (h.target >= spFx0 && h.target < spFx0 + numFx) s.fxAmt[h.target - spFx0] = v;
    else if (h.target >= spMacro0 && h.target < spMacro0 + numMacros) s.macro[h.target - spMacro0] = v;
    else if (h.target == spMotion)
    {
        const bool on = v > 0.5f;
        if (s.path != pathHold) s.shape = s.path;
        s.path = on ? juce::jlimit ((int) pathCircle, (int) pathDrawn, s.shape) : (int) pathHold;
    }
    else if (h.target == spPuckDrift) s.puckDrift = v;
    else if (h.target == spInTrim) s.inTrimDb = inTrimDbFromNorm (v);
    else if (h.target >= spSrcIn0 && h.target < spSrcIn0 + layers) s.srcInput[h.target - spSrcIn0] = v > 0.5f;
    else if (h.target >= spImpIn0 && h.target < spImpIn0 + layers) s.impInput[h.target - spImpIn0] = v > 0.5f;
    else if (h.target >= spGate0 && h.target < spGate0 + layers) s.inGate[h.target - spGate0] = v > 0.5f ? 1 : 0;
}

float rateHz (float n) { return 0.01f * std::pow (2000.0f, juce::jlimit (0.0f, 1.0f, n)); }
int rateBeats (float n) { return 64 >> juce::jlimit (0, 6, (int) std::lround (juce::jlimit (0.0f, 1.0f, n) * 6.0f)); }

juce::String rateText (float n, bool sync)
{
    if (sync)
    {
        const int b = rateBeats (n);
        if (b >= 4) return juce::String (b / 4) + (b == 4 ? " BAR" : " BARS");
        return juce::String (b) + (b == 1 ? " BEAT" : " BEATS");
    }
    const float hz = rateHz (n);
    if (hz < 1.0f) return juce::String (1.0f / hz, 1) + " s";
    return juce::String (hz, 2) + " Hz";
}

juce::String hostText (const HostParam& h, float v, bool sync)
{
    if (h.target == spRate) return v <= 0.0f && ! sync ? juce::String ("STOP") : rateText (v, sync);
    if (h.target == spMotion) return v > 0.5f ? "ON" : "OFF";
    if (h.target >= spSrcIn0 && h.target < spImpIn0 + layers) return v > 0.5f ? "INPUT" : "OFF";
    if (h.target >= spGate0 && h.target < spGate0 + layers) return v > 0.5f ? "NOTE" : "THRU";
    if (h.target == spInTrim) { const float db = inTrimDbFromNorm (v); return (db > 0.05f ? "+" : "") + juce::String (db, 1) + " dB"; }
    if (h.map == Map::sq2)
    {
        const float g = coreFromNorm (h, v);
        return g < 1e-4f ? juce::String ("-inf dB") : juce::String (20.0f * std::log10 (g), 1) + " dB";
    }
    if (h.map == Map::log)
    {
        const float hz = coreFromNorm (h, v);
        return hz >= 1000.0f ? juce::String (hz / 1000.0f, 2) + " kHz" : juce::String ((int) std::lround (hz)) + " Hz";
    }
    if (h.target == MFO_P_PUCK_X || h.target == MFO_P_PUCK_Y) return juce::String (coreFromNorm (h, v), 2);
    return juce::String ((int) std::lround (v * 100.0f)) + " %";
}

// -------------------------------------------------------------------------------- serialising ---
static const char* srcKind (Src k) { return k == Src::sample ? "sample" : k == Src::synth ? "synth" : "off"; }
static const char* impKind (Imp k) { return k == Imp::sample ? "sample" : k == Imp::synth ? "synth" : k == Imp::live ? "live" : "off"; }
static const char* fromName (SampleFrom f) { return f == SampleFrom::print ? "print" : f == SampleFrom::generated ? "generated" : "file"; }
static SampleFrom fromOf (const juce::String& s) { return s == "print" ? SampleFrom::print : s == "generated" ? SampleFrom::generated : SampleFrom::file; }

static juce::var patchVar (const ChonkPatchRef& r)
{
    auto* o = new juce::DynamicObject();
    o->setProperty ("name", r.name);
    if (r.mfp.isNotEmpty()) o->setProperty ("mfp", r.mfp);
    return juce::var (o);
}
static void patchFrom (const juce::var& v, ChonkPatchRef& r)
{
    if (! v.isObject()) return;
    r.name = v["name"].toString();
    r.mfp = v["mfp"].toString();
}
static juce::var fileVar (const SampleRef& s, const juce::String& missing)
{
    auto* o = new juce::DynamicObject();
    if (s != nullptr)
    {
        o->setProperty ("name", s->name);
        o->setProperty ("path", s->path);
        o->setProperty ("seconds", s->seconds());
        o->setProperty ("embedded", s->flac.getSize() > 0);
        o->setProperty ("detected", s->detectedRoot);
    }
    else if (missing.isNotEmpty())
    {
        o->setProperty ("name", juce::File (missing).getFileName());
        o->setProperty ("path", missing);
        o->setProperty ("missing", true);
    }
    return juce::var (o);
}

juce::var specVar (const SourceSpec& s)
{
    auto* o = new juce::DynamicObject();
    o->setProperty ("kind", srcKind (s.kind));
    o->setProperty ("from", fromName (s.from));
    o->setProperty ("patch", patchVar (s.patch));
    o->setProperty ("file", fileVar (s.sample, s.missing));
    o->setProperty ("printNote", s.printNote);
    o->setProperty ("printSeconds", s.printSeconds);
    o->setProperty ("printHold", s.printHold);
    o->setProperty ("root", s.root);
    return juce::var (o);
}

juce::var specVar (const ImpulseSpec& s)
{
    auto* o = new juce::DynamicObject();
    o->setProperty ("kind", impKind (s.kind));
    o->setProperty ("from", fromName (s.from));
    o->setProperty ("gen", s.genType);
    o->setProperty ("genSeconds", s.genSeconds);
    o->setProperty ("seed", (int) s.seed);
    o->setProperty ("file", fileVar (s.sample, s.missing));
    o->setProperty ("patch", patchVar (s.patch));
    o->setProperty ("printNote", s.printNote);
    o->setProperty ("printSeconds", s.printSeconds);
    o->setProperty ("printHold", s.printHold);
    o->setProperty ("live", s.liveLayer);
    o->setProperty ("root", s.root);
    o->setProperty ("start", s.start);
    o->setProperty ("length", s.length);
    o->setProperty ("pitch", s.pitch);
    o->setProperty ("fadeIn", s.fadeIn);
    o->setProperty ("fadeOut", s.fadeOut);
    o->setProperty ("tilt", s.tilt);
    o->setProperty ("damp", s.damp);
    o->setProperty ("age", s.age);
    o->setProperty ("reverse", s.reverse);
    o->setProperty ("gain", s.gain);
    return juce::var (o);
}

static float num (const juce::var& v, const char* k, float d)
{
    const auto x = v[k];
    return x.isVoid() || x.isUndefined() ? d : (float) (double) x;
}

void specFromVar (const juce::var& v, SourceSpec& s)
{
    if (! v.isObject()) return;
    if (v.hasProperty ("kind"))
    {
        const auto k = v["kind"].toString();
        s.kind = k == "sample" ? Src::sample : k == "synth" ? Src::synth : Src::off;
    }
    if (v.hasProperty ("from")) s.from = fromOf (v["from"].toString());
    if (s.from == SampleFrom::generated) s.from = SampleFrom::file;
    patchFrom (v["patch"], s.patch);
    s.printNote = juce::jlimit (0, 127, (int) num (v, "printNote", (float) s.printNote));
    s.printSeconds = juce::jlimit (0.05f, 8.0f, num (v, "printSeconds", s.printSeconds));
    s.printHold = juce::jlimit (0.0f, 8.0f, num (v, "printHold", s.printHold));
    s.root = juce::jlimit (0.0f, 127.0f, num (v, "root", s.root));
}

void specFromVar (const juce::var& v, ImpulseSpec& s)
{
    if (! v.isObject()) return;
    if (v.hasProperty ("kind"))
    {
        const auto k = v["kind"].toString();
        s.kind = k == "sample" ? Imp::sample : k == "synth" ? Imp::synth : k == "live" ? Imp::live : Imp::off;
    }
    if (v.hasProperty ("from")) s.from = fromOf (v["from"].toString());
    if (s.from == SampleFrom::print) s.from = SampleFrom::generated;
    s.genType = juce::jlimit (0, MFO_GEN_COUNT - 1, (int) num (v, "gen", (float) s.genType));
    s.genSeconds = juce::jlimit (0.05f, 6.0f, num (v, "genSeconds", s.genSeconds));
    s.seed = (uint32_t) juce::jlimit (0, 1 << 30, (int) num (v, "seed", (float) s.seed));
    patchFrom (v["patch"], s.patch);
    s.printNote = juce::jlimit (0, 127, (int) num (v, "printNote", (float) s.printNote));
    s.printSeconds = juce::jlimit (0.05f, 6.0f, num (v, "printSeconds", s.printSeconds));
    s.printHold = juce::jlimit (0.0f, 6.0f, num (v, "printHold", s.printHold));
    s.liveLayer = juce::jlimit (0, layers - 1, (int) num (v, "live", (float) s.liveLayer));
    s.root = juce::jlimit (0.0f, 127.0f, num (v, "root", s.root));
    s.start = juce::jlimit (0.0f, 60.0f, num (v, "start", s.start));
    s.length = juce::jlimit (0.0f, 6.0f, num (v, "length", s.length));
    s.pitch = juce::jlimit (-24.0f, 24.0f, num (v, "pitch", s.pitch));
    s.fadeIn = juce::jlimit (0.0f, 2.0f, num (v, "fadeIn", s.fadeIn));
    s.fadeOut = juce::jlimit (0.0f, 3.0f, num (v, "fadeOut", s.fadeOut));
    s.tilt = juce::jlimit (-1.0f, 1.0f, num (v, "tilt", s.tilt));
    s.damp = juce::jlimit (0.0f, 1.0f, num (v, "damp", s.damp));
    s.age = juce::jlimit (0.0f, 1.0f, num (v, "age", s.age));
    if (v.hasProperty ("reverse")) s.reverse = (bool) v["reverse"];
    s.gain = juce::jlimit (0.0f, 4.0f, num (v, "gain", s.gain));
}

juce::var toVar (const Settings& s)
{
    auto* o = new juce::DynamicObject();
    o->setProperty ("v", 2);
    juce::Array<juce::var> core;
    for (float c : s.core) core.add (c);
    o->setProperty ("core", core);
    juce::Array<juce::var> ls;
    for (int l = 0; l < layers; l++)
    {
        auto* lo = new juce::DynamicObject();
        lo->setProperty ("name", s.layerName[l]);
        lo->setProperty ("src", specVar (s.src[l]));
        lo->setProperty ("imp", specVar (s.imp[l]));
        ls.add (juce::var (lo));
    }
    o->setProperty ("layers", ls);
    o->setProperty ("path", s.path);
    o->setProperty ("sync", s.sync);
    o->setProperty ("rate", s.rate);
    o->setProperty ("spread", s.spread);
    o->setProperty ("tapeAge", s.tapeAge);
    juce::Array<juce::var> dr;
    for (int i = 0; i < s.drawn.n; i++) dr.add (juce::Array<juce::var> { s.drawn.x[i], s.drawn.y[i] });
    o->setProperty ("drawn", dr);
    auto* fx = new juce::DynamicObject();
    juce::Array<juce::var> on, amt;
    for (int i = 0; i < numFx; i++) { on.add (s.fxOn[i]); amt.add (s.fxAmt[i]); }
    fx->setProperty ("on", on);
    fx->setProperty ("amt", amt);
    fx->setProperty ("ens", s.ensMode);
    fx->setProperty ("rev", s.revType);
    fx->setProperty ("bbdSync", s.bbdSync);
    fx->setProperty ("oilSync", s.oilSync);
    o->setProperty ("fx", juce::var (fx));
    juce::Array<juce::var> mac;
    for (int i = 0; i < numMacros; i++)
    {
        auto* m = new juce::DynamicObject();
        m->setProperty ("name", s.macroName[i]);
        m->setProperty ("v", s.macro[i]);
        mac.add (juce::var (m));
    }
    o->setProperty ("macros", mac);
    juce::Array<juce::var> mods;
    for (int i = 0; i < MFO_NMOD; i++)
    {
        const auto& m = s.mods[(size_t) i];
        if (m.src == 0 || m.amount == 0.0f) continue;
        mods.add (juce::Array<juce::var> { i, m.src, m.dst, m.amount });
    }
    o->setProperty ("mods", mods);
    auto* pr = new juce::DynamicObject();
    pr->setProperty ("name", s.presetName);
    pr->setProperty ("kind", s.presetKind);
    pr->setProperty ("dirty", s.dirty);
    o->setProperty ("preset", juce::var (pr));
    // 1.2.0
    o->setProperty ("shape", s.shape);
    o->setProperty ("puckDrift", s.puckDrift);
    o->setProperty ("legacyWobble", s.legacyWobble);
    auto* in = new juce::DynamicObject();
    in->setProperty ("trim", s.inTrimDb);
    juce::Array<juce::var> il;
    for (int l = 0; l < layers; l++)
    {
        auto* e = new juce::DynamicObject();
        e->setProperty ("src", s.srcInput[l]);
        e->setProperty ("imp", s.impInput[l]);
        e->setProperty ("gate", s.inGate[l]);
        e->setProperty ("window", s.inWindow[l]);
        il.add (juce::var (e));
    }
    in->setProperty ("layers", il);
    o->setProperty ("input", juce::var (in));
    return juce::var (o);
}

bool fromVar (const juce::var& v, Settings& s, juce::String& err)
{
    if (! v.isObject() || ! v["core"].isArray()) { err = "not an ORBIT preset"; return false; }
    Settings d;                                     // defaults for what is missing
    const auto* core = v["core"].getArray();
    for (int i = 0; i < numCore && i < core->size(); i++)
    {
        const auto* in = mfo_param_info (i);
        d.core[(size_t) i] = juce::jlimit (in->min, in->max, (float) (double) (*core)[i]);
    }
    if (const auto* ls = v["layers"].getArray())
        for (int l = 0; l < layers && l < ls->size(); l++)
        {
            const auto& lv = (*ls)[l];
            if (lv["name"].toString().isNotEmpty()) d.layerName[l] = lv["name"].toString().substring (0, 24);
            d.src[l] = SourceSpec();
            d.imp[l] = ImpulseSpec();
            specFromVar (lv["src"], d.src[l]);
            specFromVar (lv["imp"], d.imp[l]);
            // files: resolved by the caller (embedded audio or path)
            if (d.src[l].kind == Src::sample && d.src[l].from == SampleFrom::file) d.src[l].missing = lv["src"]["file"]["path"].toString();
            if (d.imp[l].kind == Imp::sample && d.imp[l].from == SampleFrom::file) d.imp[l].missing = lv["imp"]["file"]["path"].toString();
        }
    d.path = juce::jlimit (0, 3, (int) num (v, "path", 0));
    d.sync = (bool) v["sync"];
    d.rate = juce::jlimit (0.0f, 1.0f, num (v, "rate", d.rate));
    d.spread = juce::jlimit (0.0f, 1.0f, num (v, "spread", d.spread));
    d.tapeAge = juce::jlimit (0.0f, 1.0f, num (v, "tapeAge", 0.0f));
    if (const auto* dr = v["drawn"].getArray())
    {
        d.drawn.n = 0;
        for (int i = 0; i < dr->size() && d.drawn.n < maxDrawn; i++)
        {
            const auto& p = (*dr)[i];
            if (! p.isArray() || p.size() < 2) continue;
            d.drawn.x[d.drawn.n] = juce::jlimit (-1.0f, 1.0f, (float) (double) p[0]);
            d.drawn.y[d.drawn.n] = juce::jlimit (-1.0f, 1.0f, (float) (double) p[1]);
            d.drawn.n++;
        }
    }
    const auto fx = v["fx"];
    if (fx.isObject())
    {
        for (int i = 0; i < numFx; i++)
        {
            if (fx["on"].isArray() && i < fx["on"].size()) d.fxOn[i] = (bool) fx["on"][i];
            if (fx["amt"].isArray() && i < fx["amt"].size()) d.fxAmt[i] = juce::jlimit (0.0f, 1.0f, (float) (double) fx["amt"][i]);
        }
        d.ensMode = juce::jlimit (1, 4, (int) num (fx, "ens", 1));
        d.revType = juce::jlimit (0, 1, (int) num (fx, "rev", 0));
        d.bbdSync = juce::jlimit (0, MFE_SYNC_DIVS, (int) num (fx, "bbdSync", 0));
        d.oilSync = juce::jlimit (0, MFE_SYNC_DIVS, (int) num (fx, "oilSync", 0));
    }
    if (const auto* mac = v["macros"].getArray())
        for (int i = 0; i < numMacros && i < mac->size(); i++)
        {
            if ((*mac)[i]["name"].toString().isNotEmpty()) d.macroName[i] = (*mac)[i]["name"].toString().substring (0, 16);
            d.macro[i] = juce::jlimit (0.0f, 1.0f, num ((*mac)[i], "v", 0.5f));
        }
    if (const auto* mods = v["mods"].getArray())
        for (const auto& m : *mods)
        {
            if (! m.isArray() || m.size() < 4) continue;
            const int slot = (int) m[0];
            if (slot < 0 || slot >= MFO_NMOD) continue;
            d.mods[(size_t) slot] = { juce::jlimit (0, MFO_MS_COUNT - 1, (int) m[1]), juce::jlimit (0, numCore - 1, (int) m[2]),
                                      juce::jlimit (-1.0f, 1.0f, (float) (double) m[3]) };
        }
    const auto pr = v["preset"];
    if (pr.isObject())
    {
        d.presetName = pr["name"].toString().substring (0, 40);
        d.presetKind = pr["kind"].toString() == "U" ? "U" : "F";
        d.dirty = (bool) pr["dirty"];
    }
    // 1.2.0 (absent in 1.1.0 states: those keep the 1.1.0 behaviour, bit for bit)
    const int ver = (int) num (v, "v", 1);
    d.legacyWobble = ver < 2 ? true : (bool) v["legacyWobble"];
    d.shape = juce::jlimit ((int) pathCircle, (int) pathDrawn, (int) num (v, "shape", d.path != pathHold ? (float) d.path : (float) pathCircle));
    d.puckDrift = juce::jlimit (0.0f, 1.0f, num (v, "puckDrift", 0.0f));
    const auto in = v["input"];
    if (in.isObject())
    {
        d.inTrimDb = juce::jlimit (-inTrimRange, inTrimRange, num (in, "trim", 0.0f));
        if (const auto* il = in["layers"].getArray())
            for (int l = 0; l < layers && l < il->size(); l++)
            {
                const auto& e = (*il)[l];
                d.srcInput[l] = (bool) e["src"];
                d.impInput[l] = (bool) e["imp"];
                d.inGate[l] = (int) num (e, "gate", 0.0f) ? 1 : 0;
                d.inWindow[l] = windowFromChoice (num (e, "window", 0.25f));
            }
    }
    s = d;
    return true;
}

// ----------------------------------------------------------------------------------- samples ---
static juce::AudioFormatManager& formats()
{
    static juce::AudioFormatManager m;
    static std::once_flag once;
    std::call_once (once, [] { m.registerBasicFormats(); });
    return m;
}

static void embed (SampleData& d)
{
    // FLAC 24-bit when the rate is one FLAC writes, else WAV 24-bit; nothing past the cap
    juce::AudioBuffer<float> b (d.channels, d.length());
    for (int c = 0; c < d.channels; c++) b.copyFrom (c, 0, d.ch[c].data(), d.length());
    for (int pass = 0; pass < 2; pass++)
    {
        juce::MemoryBlock mb;
        std::unique_ptr<juce::OutputStream> os = std::make_unique<juce::MemoryOutputStream> (mb, false);
        std::unique_ptr<juce::AudioFormat> fmt;
        if (pass == 0) fmt = std::make_unique<juce::FlacAudioFormat>();
        else fmt = std::make_unique<juce::WavAudioFormat>();
        auto w = fmt->createWriterFor (os, juce::AudioFormatWriterOptions {}.withSampleRate (d.rate)
                                                .withNumChannels (d.channels).withBitsPerSample (24));
        if (w == nullptr) continue;
        if (! w->writeFromAudioSampleBuffer (b, 0, b.getNumSamples())) continue;
        w.reset();   // flushes into mb
        if (mb.getSize() <= embedCapBytes) d.flac = std::move (mb);
        return;
    }
}

static SampleRef fromReader (juce::AudioFormatReader& r, const juce::String& name, const juce::String& path, juce::String& err)
{
    if (r.sampleRate < 1000.0 || r.lengthInSamples < 2) { err = "not audio"; return nullptr; }
    auto d = std::make_shared<SampleData>();
    d->name = name;
    d->path = path;
    d->rate = r.sampleRate;
    d->channels = r.numChannels > 1 ? 2 : 1;
    const int64_t len = std::min<int64_t> (r.lengthInSamples, (int64_t) (maxSampleSeconds * r.sampleRate));
    juce::AudioBuffer<float> b ((int) r.numChannels, (int) len);
    if (! r.read (&b, 0, (int) len, 0, true, true)) { err = "could not read the audio"; return nullptr; }
    for (int c = 0; c < d->channels; c++) d->ch[c].assign (b.getReadPointer (c), b.getReadPointer (c) + len);
    d->detectedRoot = detectRoot (*d);
    embed (*d);
    return d;
}

SampleRef decodeFile (const juce::File& f, juce::String& err)
{
    std::unique_ptr<juce::AudioFormatReader> r (formats().createReaderFor (f));
    if (r == nullptr) { err = "not a WAV / AIFF / FLAC file: " + f.getFileName(); return nullptr; }
    return fromReader (*r, f.getFileName(), f.getFullPathName(), err);
}

SampleRef decodeMemory (const void* data, size_t size, const juce::String& name, const juce::String& path, juce::String& err)
{
    auto in = std::make_unique<juce::MemoryInputStream> (data, size, true);
    std::unique_ptr<juce::AudioFormatReader> r (formats().createReaderFor (std::move (in)));
    if (r == nullptr) { err = "unreadable audio: " + name; return nullptr; }
    auto s = fromReader (*r, name, path, err);
    return s;
}

float detectRoot (const SampleData& s)
{
    // YIN (de Cheveigne & Kawahara 2002) on three frames of the steady part (after the loudest moment)
    const int len = s.length();
    if (len < 4096) return -1.0f;
    std::vector<float> x ((size_t) len);
    for (int i = 0; i < len; i++) x[(size_t) i] = s.channels > 1 ? 0.5f * (s.ch[0][(size_t) i] + s.ch[1][(size_t) i]) : s.ch[0][(size_t) i];
    const int hop = 1024, frames = len / hop;
    std::vector<float> rms ((size_t) std::max (1, frames));
    int pk = 0;
    for (int f = 0; f < frames; f++)
    {
        double e = 0;
        for (int i = 0; i < hop; i++) e += (double) x[(size_t) (f * hop + i)] * x[(size_t) (f * hop + i)];
        rms[(size_t) f] = (float) std::sqrt (e / hop);
        if (rms[(size_t) f] > rms[(size_t) pk]) pk = f;
    }
    if (rms[(size_t) pk] < 1e-4f) return -1.0f;
    int end = pk;
    while (end + 1 < frames && rms[(size_t) end + 1] > 0.2f * rms[(size_t) pk]) end++;
    const int W = 2048, tauMin = std::max (2, (int) (s.rate / 2000.0)), tauMax = std::min (W, (int) (s.rate / 40.0));
    const int a0 = std::min ((pk + 1) * hop, len - W - tauMax - 1);
    const int a1 = std::max (a0, std::min (end * hop, len - W - tauMax - 1));
    if (a0 < 0) return -1.0f;
    std::vector<float> found;
    std::vector<double> d ((size_t) tauMax + 2);
    for (int k = 0; k < 3; k++)
    {
        const int st = a0 + (a1 - a0) * k / 3;
        d[0] = 1.0;
        double run = 0.0;
        int pick = -1;
        for (int tau = 1; tau <= tauMax; tau++)
        {
            double sum = 0;
            for (int j = 0; j < W; j++)
            {
                const double q = (double) x[(size_t) (st + j)] - x[(size_t) (st + j + tau)];
                sum += q * q;
            }
            run += sum;
            d[(size_t) tau] = run > 0 ? sum * tau / run : 1.0;
        }
        for (int tau = tauMin; tau < tauMax; tau++)
            if (d[(size_t) tau] < 0.15)
            {
                while (tau + 1 < tauMax && d[(size_t) tau + 1] < d[(size_t) tau]) tau++;
                pick = tau;
                break;
            }
        if (pick < 0) continue;
        const double y0 = d[(size_t) pick - 1], y1 = d[(size_t) pick], y2 = d[(size_t) pick + 1];
        const double den = y0 - 2 * y1 + y2;
        const double t = pick + (std::fabs (den) > 1e-12 ? 0.5 * (y0 - y2) / den : 0.0);
        found.push_back ((float) (69.0 + 12.0 * std::log2 (s.rate / t / 440.0)));
    }
    if (found.empty()) return -1.0f;
    std::sort (found.begin(), found.end());
    return juce::jlimit (0.0f, 127.0f, found[found.size() / 2]);
}

// ------------------------------------------------------------------------ Chonk preset refs ---
bool chonkPatch (const ChonkPatchRef& r, mfe_patch_t& p)
{
    if (r.mfp.isNotEmpty())
    {
        juce::MemoryOutputStream mo;
        if (r.mfp.length() < 8192 && juce::Base64::convertFromBase64 (mo, r.mfp)
            && mfe_preset_read (&p, static_cast<const uint8_t*> (mo.getData()), mo.getDataSize()) == 0)
            return true;
    }
    for (int i = 0; i < mfm1_factory::count; i++)
        if (r.name.equalsIgnoreCase (mfm1_factory::names[i]))
            return mfe_preset_read (&p, mfm1_factory::images[i], mfm1_factory::size) == 0;
    return false;
}

juce::var chonkPresetList()
{
    juce::Array<juce::var> a;
    for (int i = 0; i < mfm1_factory::count; i++)
    {
        auto* o = new juce::DynamicObject();
        o->setProperty ("name", juce::String (mfm1_factory::names[i]));
        o->setProperty ("kind", "F");
        a.add (juce::var (o));
    }
    if (const auto* u = mfm1::PresetStore::list().getArray())
        for (const auto& p : *u)
        {
            auto* o = new juce::DynamicObject();
            o->setProperty ("name", p["name"]);
            o->setProperty ("kind", "U");
            o->setProperty ("mfp", p["b64"]);
            a.add (juce::var (o));
        }
    return a;
}

// ----------------------------------------------------------------------------- plugin state ---
std::unique_ptr<juce::XmlElement> toXml (const Settings& s)
{
    auto x = std::make_unique<juce::XmlElement> ("ORBIT");
    x->setAttribute ("version", 1);
    x->setAttribute ("settings", juce::JSON::toString (toVar (s), true));
    auto addSample = [&] (const char* id, const SampleRef& d)
    {
        if (d == nullptr) return;
        auto* e = x->createNewChildElement ("SAMPLE");
        e->setAttribute ("id", id);
        e->setAttribute ("name", d->name);
        e->setAttribute ("path", d->path);
        e->setAttribute ("embedded", d->flac.getSize() > 0 ? 1 : 0);
        if (d->flac.getSize() > 0) e->setAttribute ("audio", juce::Base64::toBase64 (d->flac.getData(), d->flac.getSize()));
    };
    for (int l = 0; l < layers; l++)
    {
        if (s.src[l].kind == Src::sample && s.src[l].from == SampleFrom::file) addSample (l == 0 ? "S1" : l == 1 ? "S2" : "S3", s.src[l].sample);
        if (s.imp[l].kind == Imp::sample && s.imp[l].from == SampleFrom::file) addSample (l == 0 ? "I1" : l == 1 ? "I2" : "I3", s.imp[l].sample);
    }
    return x;
}

static SampleRef resolve (const juce::XmlElement& x, const char* id, const juce::String& pathHint, juce::StringArray& problems)
{
    juce::String name = juce::File (pathHint).getFileName(), path = pathHint;
    if (auto* e = x.getChildByAttribute ("id", id))
    {
        name = e->getStringAttribute ("name", name);
        path = e->getStringAttribute ("path", path);
        const auto audio = e->getStringAttribute ("audio");
        if (audio.isNotEmpty())
        {
            juce::MemoryOutputStream mo;
            if (juce::Base64::convertFromBase64 (mo, audio))
            {
                juce::String err;
                if (auto d = decodeMemory (mo.getData(), mo.getDataSize(), name, path, err)) return d;
                problems.add ("embedded " + name + ": " + err);
            }
        }
    }
    if (path.isNotEmpty())
    {
        const juce::File f (path);
        if (f.existsAsFile())
        {
            juce::String err;
            if (auto d = decodeFile (f, err)) return d;
            problems.add (err);
        }
        else
            problems.add (name + " is not embedded and not found at " + path);
    }
    return nullptr;
}

bool fromXml (const juce::XmlElement& x, Settings& s, juce::StringArray& problems)
{
    juce::String err;
    Settings d;
    if (! fromVar (juce::JSON::parse (x.getStringAttribute ("settings")), d, err)) { problems.add (err); return false; }
    static const char* sid[layers] = { "S1", "S2", "S3" };
    static const char* iid[layers] = { "I1", "I2", "I3" };
    for (int l = 0; l < layers; l++)
    {
        if (d.src[l].kind == Src::sample && d.src[l].from == SampleFrom::file)
        {
            d.src[l].sample = resolve (x, sid[l], d.src[l].missing, problems);
            if (d.src[l].sample != nullptr) d.src[l].missing = {};
        }
        if (d.imp[l].kind == Imp::sample && d.imp[l].from == SampleFrom::file)
        {
            d.imp[l].sample = resolve (x, iid[l], d.imp[l].missing, problems);
            if (d.imp[l].sample != nullptr) d.imp[l].missing = {};
        }
    }
    s = d;
    return true;
}

// ------------------------------------------------------------------------------ user presets ---
juce::File userFolder() { return mfm1::PresetStore::dataFolder ("orbit"); }
juce::File droppedFolder() { return mfm1::PresetStore::dataFolder ("samples"); }

juce::var userPresetList()
{
    juce::Array<juce::var> a;
    auto files = userFolder().findChildFiles (juce::File::findFiles, false, "*.orbit");
    files.sort();
    for (const auto& f : files)
    {
        auto* o = new juce::DynamicObject();
        o->setProperty ("name", f.getFileNameWithoutExtension());
        a.add (juce::var (o));
    }
    return a;
}

juce::String saveUserPreset (const juce::String& name, const Settings& s)
{
    const auto n = mfm1::PresetStore::safeName (name);
    auto dir = userFolder();
    if (! dir.createDirectory()) return "cannot create " + dir.getFullPathName();
    Settings c = s;
    c.presetName = n;
    c.presetKind = "U";
    c.dirty = false;
    // presets travel as XML (samples embedded within the cap, paths otherwise)
    auto x = toXml (c);
    return x->writeTo (dir.getChildFile (n + ".orbit")) ? juce::String ("ok") : juce::String ("write failed");
}

bool loadUserPreset (const juce::String& name, Settings& s, juce::String& err)
{
    const auto f = userFolder().getChildFile (mfm1::PresetStore::safeName (name) + ".orbit");
    auto x = juce::XmlDocument::parse (f);
    if (x == nullptr || ! x->hasTagName ("ORBIT")) { err = "cannot read " + f.getFileName(); return false; }
    juce::StringArray problems;
    if (! fromXml (*x, s, problems)) { err = problems.joinIntoString ("; "); return false; }
    s.presetKind = "U";
    s.presetName = f.getFileNameWithoutExtension();
    s.dirty = false;
    err = problems.joinIntoString ("; ");
    return true;
}

juce::String deleteUserPreset (const juce::String& name)
{
    const auto f = userFolder().getChildFile (mfm1::PresetStore::safeName (name) + ".orbit");
    return f.existsAsFile() && f.deleteFile() ? juce::String ("ok") : juce::String ("not found");
}

// --------------------------------------------------------------------------- factory presets ---
// Only material made here: Chonk factory presets (as synths or prints) and the generated impulses.
// Levels: each preset's output (MFO_P_MASTER) is set so that the test score of plugin/tools/orbit_tool.cpp
// (also the host test's preset renders) sits near -19 dBFS RMS and peaks at or below -1.5 dBFS, also on a plain C chord (SPRING RAIN,
// sparse drops whose chords stack up, is peak-limited at about -23.5 dBFS RMS).
namespace {
struct P
{
    Settings s;
    explicit P (const char* name, bool legacy = true)
    {
        s.presetName = name;
        s.presetKind = "F";
        s.legacyWobble = legacy;           // the 1.1.0 factory presets (17-24) sound exactly as they did
        for (int l = 0; l < layers; l++)
        {
            s.src[l].kind = Src::off;
            s.imp[l].kind = Imp::off;
            s.core[(size_t) MFO_LPARAM (l, MFO_L_BODY)] = 0.0f;
        }
        s.core[MFO_P_MASTER] = 0.8f;
    }
    float& L (int l, int p) { return s.core[(size_t) MFO_LPARAM (l, p)]; }
    float& G (int p) { return s.core[(size_t) p]; }
    void synth (int l, const char* preset, const char* name)
    {
        s.src[l].kind = Src::synth;
        s.src[l].patch.name = preset;
        s.layerName[l] = name;
    }
    void print (int l, const char* preset, int note, float secs, float hold, const char* name)
    {
        s.src[l].kind = Src::sample;
        s.src[l].from = SampleFrom::print;
        s.src[l].patch.name = preset;
        s.src[l].printNote = note;
        s.src[l].printSeconds = secs;
        s.src[l].printHold = hold;
        s.src[l].root = (float) note;
        s.layerName[l] = name;
    }
    ImpulseSpec& gen (int l, int type, float secs, uint32_t seed)
    {
        auto& i = s.imp[l];
        i = ImpulseSpec();
        i.kind = Imp::sample;
        i.from = SampleFrom::generated;
        i.genType = type;
        i.genSeconds = secs;
        i.seed = seed;
        return i;
    }
    ImpulseSpec& impPrint (int l, const char* preset, int note, float secs, float hold)
    {
        auto& i = s.imp[l];
        i = ImpulseSpec();
        i.kind = Imp::synth;
        i.patch.name = preset;
        i.printNote = note;
        i.printSeconds = secs;
        i.printHold = hold;
        i.root = (float) note;
        return i;
    }
    void live (int l, int from) { s.imp[l] = ImpulseSpec(); s.imp[l].kind = Imp::live; s.imp[l].liveLayer = from; }
    void matrix (int l, float body, float tone, float grain, float motion, float pitch)
    {
        L (l, MFO_L_BODY) = body; L (l, MFO_L_TONE) = tone; L (l, MFO_L_GRAIN) = grain;
        L (l, MFO_L_MOTION) = motion; L (l, MFO_L_PITCH) = pitch;
    }
    void filter (int l, int type, float hz, float res, float env = 0.0f)
    {
        L (l, MFO_L_FILTER_TYPE) = (float) type; L (l, MFO_L_CUTOFF) = hz; L (l, MFO_L_RESO) = res; L (l, MFO_L_FENV_AMT) = env;
    }
    void amp (int l, float a, float d, float su, float r)
    {
        L (l, MFO_L_ATTACK) = a; L (l, MFO_L_DECAY) = d; L (l, MFO_L_SUSTAIN) = su; L (l, MFO_L_RELEASE) = r;
    }
    void nodes (float x1, float y1, float x2, float y2, float x3, float y3, float dx, float dy)
    {
        G (MFO_P_NODE1_X) = x1; G (MFO_P_NODE1_Y) = y1; G (MFO_P_NODE2_X) = x2; G (MFO_P_NODE2_Y) = y2;
        G (MFO_P_NODE3_X) = x3; G (MFO_P_NODE3_Y) = y3; G (MFO_P_DRY_X) = dx; G (MFO_P_DRY_Y) = dy;
    }
    void puck (float x, float y) { G (MFO_P_PUCK_X) = x; G (MFO_P_PUCK_Y) = y; }
    void orbit (int path, bool sync, float rate, float spread, float wobble, int dir = 0)
    {
        s.path = path; s.sync = sync; s.rate = rate; s.spread = spread;
        if (path != pathHold) s.shape = path;
        G (MFO_P_ORBIT_WOBBLE) = wobble; G (MFO_P_ORBIT_DIR) = (float) dir;
    }
    void fx (int i, float amt) { s.fxOn[i] = true; s.fxAmt[i] = amt; }
    void macro (int k, const char* name, float v) { s.macroName[k] = name; s.macro[k] = v; }
    int nmod = 0;
    void mod (int src, int dst, float amt) { if (nmod < MFO_NMOD) s.mods[(size_t) nmod++] = { src, dst, amt }; }
    void drawLoop (float cx, float cy, float rx, float ry, float rot, float wob)
    {
        s.drawn.n = 48;
        for (int i = 0; i < 48; i++)
        {
            const float t = (float) i / 48.0f * 6.2831853f;
            const float k = 1.0f + wob * (0.55f * std::sin (3 * t + 0.7f) + 0.3f * std::sin (5 * t + 2.1f) + 0.15f * std::sin (2 * t));
            const float x0 = std::cos (t) * rx * k, y0 = std::sin (t) * ry * k;
            s.drawn.x[i] = juce::jlimit (-1.0f, 1.0f, cx + x0 * std::cos (rot) - y0 * std::sin (rot));
            s.drawn.y[i] = juce::jlimit (-1.0f, 1.0f, cy + x0 * std::sin (rot) + y0 * std::cos (rot));
        }
        puck (cx, cy);
    }
};
constexpr int LADDER = 1, SVF_LP = 2, SVF_BP = 3, SVF_HP = 4;
constexpr float rate4bars = 2.0f / 6.0f, rate2bars = 3.0f / 6.0f, rate8bars = 1.0f / 6.0f, rate16bars = 0.0f;

Settings bowedHalo()
{
    P p ("BOWED HALO");
    p.synth (0, "Velvet Pad", "HALO PAD");
    p.gen (0, MFO_GEN_BOWED_METAL, 4.0f, 21).gain = 0.75f;
    p.matrix (0, 0.85f, 0.2f, 0.0f, 0.0f, 0.0f);
    p.filter (0, LADDER, 3800.0f, 0.18f);
    p.amp (0, 0.6f, 1.0f, 1.0f, 2.4f);
    p.synth (1, "Silk Strings", "BOWED AIR");
    auto& i1 = p.gen (1, MFO_GEN_GLASS, 3.0f, 3);
    i1.damp = 0.3f;
    p.matrix (1, 0.35f, 0.25f, 0.45f, 0.0f, 0.4f);
    p.L (1, MFO_L_SCAN_RATE) = 0.4f;
    p.L (1, MFO_L_LEVEL) = 0.8f;
    p.amp (1, 0.9f, 1.0f, 1.0f, 2.8f);
    p.nodes (-0.7f, 0.6f, 0.7f, 0.6f, 0.0f, -0.75f, -0.6f, -0.6f);
    p.puck (-0.05f, 0.35f);
    p.orbit (pathCircle, true, rate8bars, 0.42f, 0.3f);
    p.G (MFO_P_DRIFT) = 0.35f;
    p.fx (1, 0.25f); p.fx (2, 0.35f); p.fx (5, 0.38f);
    p.macro (0, "SHIMMER", 0.4f); p.mod (MFO_MS_MACRO1, MFO_LPARAM (1, MFO_L_PITCH), 0.5f);
    p.macro (1, "METAL", 0.5f); p.mod (MFO_MS_MACRO2, MFO_LPARAM (0, MFO_L_BODY), 0.3f);
    p.macro (2, "DARK", 0.0f); p.mod (MFO_MS_MACRO3, MFO_LPARAM (0, MFO_L_CUTOFF), -0.4f);
    p.macro (3, "WOBBLE", 0.0f); p.mod (MFO_MS_MACRO4, MFO_P_ORBIT_WOBBLE, 0.7f);
    p.G (MFO_P_MASTER) = 0.34f;     // output level: about -19 dBFS RMS, peaks at or below -1.5 dBFS (orbit_tool)
    return p.s;
}

Settings tapeChoir()
{
    P p ("BREATHING TAPE CHOIR");
    p.synth (0, "Velvet Pad", "CHOIR LOW");
    p.gen (0, MFO_GEN_FORMANT_VOX, 3.0f, 5);
    p.matrix (0, 0.0f, 0.9f, 0.25f, 0.0f, 0.0f);
    p.L (0, MFO_L_SCAN_RATE) = 0.5f;
    p.L (0, MFO_L_TAPE) = 1; p.L (0, MFO_L_WOW) = 0.5f; p.L (0, MFO_L_HISS) = 0.25f;
    p.amp (0, 0.5f, 1.0f, 1.0f, 2.0f);
    p.synth (1, "Silk Strings", "CHOIR HIGH");
    p.gen (1, MFO_GEN_FORMANT_VOX, 3.0f, 9);
    p.matrix (1, 0.0f, 0.8f, 0.15f, 0.25f, 0.0f);
    p.L (1, MFO_L_SCAN_RATE) = 0.3f;
    p.L (1, MFO_L_TUNE) = 12.0f;
    p.L (1, MFO_L_LEVEL) = 0.7f;
    p.amp (1, 0.8f, 1.0f, 1.0f, 2.4f);
    // breath: generated tape hiss as a looping sample, shaped by the formant voice
    p.s.src[2].kind = Src::sample; p.s.src[2].from = SampleFrom::print; p.s.src[2].patch.name = "Night Wind";
    p.s.src[2].printNote = 60; p.s.src[2].printSeconds = 2.0f; p.s.src[2].printHold = 1.8f; p.s.src[2].root = 60;
    p.s.layerName[2] = "BREATH";
    p.gen (2, MFO_GEN_FORMANT_VOX, 3.0f, 17);
    p.matrix (2, 0.0f, 1.0f, 0.0f, 0.5f, 0.0f);
    p.L (2, MFO_L_LOOP) = 1; p.L (2, MFO_L_LOOP_START) = 0.2f; p.L (2, MFO_L_LOOP_END) = 0.85f; p.L (2, MFO_L_XFADE) = 0.3f;
    p.L (2, MFO_L_SCAN_RATE) = 0.7f;
    p.filter (2, SVF_BP, 1400.0f, 0.4f);
    p.amp (2, 0.4f, 1.0f, 1.0f, 1.5f);
    p.L (2, MFO_L_LEVEL) = 0.9f;
    p.nodes (-0.7f, 0.5f, 0.7f, 0.5f, 0.0f, -0.7f, 0.0f, 0.8f);
    p.puck (0.0f, 0.15f);
    p.orbit (pathFig8, true, rate8bars, 0.6f, 0.25f);
    p.G (MFO_P_DRIFT) = 0.3f;
    p.s.tapeAge = 0.3f;
    p.fx (1, 0.4f); p.fx (2, 0.45f); p.fx (5, 0.42f);
    p.macro (0, "BREATH", 0.5f); p.mod (MFO_MS_MACRO1, MFO_LPARAM (2, MFO_L_LEVEL), 0.4f);
    p.macro (1, "VOWEL", 0.5f); p.mod (MFO_MS_MACRO2, MFO_LPARAM (0, MFO_L_SCAN_POS), 0.6f);
    p.mod (MFO_MS_MODWHEEL, MFO_LPARAM (1, MFO_L_GRAIN), 0.5f);
    p.G (MFO_P_MASTER) = 0.165f;     // output level: about -19 dBFS RMS, peaks at or below -1.5 dBFS (orbit_tool)
    return p.s;
}

Settings springRain()
{
    P p ("SPRING RAIN");
    p.print (0, "Iron Bell", 72, 1.2f, 0.15f, "DROPS");
    p.gen (0, MFO_GEN_SPRING, 3.5f, 9);
    p.matrix (0, 0.9f, 0.0f, 0.3f, 0.0f, 0.0f);
    p.amp (0, 0.0f, 0.9f, 0.0f, 0.9f);
    p.L (0, MFO_L_VEL_START) = 0.5f;
    p.L (0, MFO_L_LEVEL) = 2.0f;
    p.print (1, "Night Wind", 60, 3.0f, 2.8f, "RAIN");
    p.gen (1, MFO_GEN_METAL_HIT, 0.4f, 31);
    p.matrix (1, 0.0f, 0.2f, 0.6f, 0.85f, 0.0f);
    p.L (1, MFO_L_SCAN_RATE) = 1.7f;
    p.L (1, MFO_L_LOOP) = 1; p.L (1, MFO_L_LOOP_START) = 0.1f; p.L (1, MFO_L_XFADE) = 0.3f;
    p.filter (1, SVF_HP, 1800.0f, 0.2f);
    p.amp (1, 0.3f, 1.0f, 1.0f, 1.2f);
    p.L (1, MFO_L_LEVEL) = 1.6f;
    p.synth (2, "Velvet Pad", "UNDER");
    p.gen (2, MFO_GEN_SPRING, 3.0f, 12).damp = 0.5f;
    p.matrix (2, 0.6f, 0.3f, 0.0f, 0.0f, 0.0f);
    p.filter (2, LADDER, 900.0f, 0.2f);
    p.L (2, MFO_L_LEVEL) = 1.2f;
    p.nodes (-0.7f, 0.6f, 0.7f, 0.6f, 0.0f, -0.7f, -0.7f, -0.7f);
    p.puck (-0.2f, 0.35f);
    p.orbit (pathHold, false, 0.3f, 0.5f, 0.35f);
    p.G (MFO_P_DRIFT) = 0.3f;
    p.s.revType = MFE_REV_SPRING;
    p.fx (4, 0.28f); p.fx (5, 0.4f);
    p.macro (0, "STORM", 0.3f); p.mod (MFO_MS_MACRO1, MFO_LPARAM (1, MFO_L_LEVEL), 0.6f);
    p.mod (MFO_MS_MACRO1, MFO_LPARAM (1, MFO_L_SCAN_RATE), 0.3f);
    p.macro (1, "SPRING", 0.5f); p.mod (MFO_MS_MACRO2, MFO_LPARAM (0, MFO_L_GRAIN), 0.5f);
    p.G (MFO_P_MASTER) = 1.3f;      // output level: peak-limited (four drops at once on a chord peak at about -2 dBFS)
    return p.s;
}

Settings woodMarimba()
{
    P p ("WOOD BOX MARIMBA");
    p.print (0, "Glass Pluck", 60, 0.3f, 0.05f, "MALLET");
    auto& w = p.gen (0, MFO_GEN_WOOD_BOX, 0.6f, 4);
    w.gain = 1.6f;
    p.matrix (0, 1.0f, 0.0f, 0.0f, 0.0f, 0.0f);
    p.L (0, MFO_L_KEYTRACK) = 1;
    p.amp (0, 0.0f, 0.3f, 0.0f, 0.3f);
    p.L (0, MFO_L_VEL_SENS) = 0.8f;
    p.print (1, "Parlour Keys", 60, 1.2f, 0.4f, "TINE");
    p.gen (1, MFO_GEN_WOOD_BOX, 0.5f, 7);
    p.matrix (1, 0.5f, 0.35f, 0.0f, 0.0f, 0.0f);
    p.amp (1, 0.0f, 0.8f, 0.0f, 0.5f);
    p.L (1, MFO_L_LEVEL) = 0.8f;
    p.nodes (-0.7f, 0.6f, 0.7f, 0.6f, 0.0f, -0.7f, -0.7f, -0.7f);
    p.puck (-0.45f, 0.5f);
    p.orbit (pathHold, false, 0.3f, 0.4f, 0.1f);
    p.G (MFO_P_DRIFT) = 0.15f;
    p.fx (1, 0.25f); p.fx (5, 0.22f);
    p.macro (0, "HOLLOW", 0.0f); p.mod (MFO_MS_MACRO1, MFO_LPARAM (1, MFO_L_TONE), 0.6f);
    p.mod (MFO_MS_VELOCITY, MFO_LPARAM (1, MFO_L_LEVEL), 0.2f);
    p.G (MFO_P_MASTER) = 0.27f;     // output level: about -19 dBFS RMS, peaks at or below -1.5 dBFS (orbit_tool)
    return p.s;
}

Settings liveDrone()
{
    P p ("LIVE CROSS DRONE");
    p.synth (0, "Walnut Bass", "ROOT");
    p.matrix (0, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f);
    p.filter (0, LADDER, 700.0f, 0.3f);
    p.amp (0, 0.3f, 1.0f, 1.0f, 2.0f);
    p.synth (1, "Silk Strings", "CROSS");
    p.live (1, 0);
    p.matrix (1, 0.0f, 0.55f, 0.7f, 0.2f, 0.0f);
    p.amp (1, 1.2f, 1.0f, 1.0f, 3.0f);
    p.synth (2, "Night Wind", "IRON AIR");
    p.gen (2, MFO_GEN_BOWED_METAL, 4.0f, 44).tilt = -0.3f;
    p.matrix (2, 0.9f, 0.0f, 0.2f, 0.0f, 0.0f);
    p.L (2, MFO_L_LEVEL) = 1.4f;
    p.amp (2, 1.5f, 1.0f, 1.0f, 3.0f);
    p.nodes (-0.7f, 0.6f, 0.7f, 0.6f, 0.0f, -0.75f, -0.7f, -0.7f);
    p.drawLoop (0.15f, 0.1f, 0.6f, 0.45f, -0.3f, 0.2f);
    p.orbit (pathDrawn, true, rate16bars, 0.7f, 0.2f);
    p.G (MFO_P_DRIFT) = 0.4f;
    p.fx (2, 0.3f); p.fx (3, 0.22f); p.fx (5, 0.38f);
    p.s.bbdSync = 9;   // 1 beat
    p.macro (0, "CROSS", 0.5f); p.mod (MFO_MS_MACRO1, MFO_LPARAM (1, MFO_L_GRAIN), 0.3f);
    p.mod (MFO_MS_MACRO1, MFO_LPARAM (1, MFO_L_TONE), 0.3f);
    p.mod (MFO_MS_EXT8, MFO_LPARAM (0, MFO_L_CUTOFF), 0.15f);
    p.G (MFO_P_MASTER) = 0.59f;     // output level: about -19 dBFS RMS, peaks at or below -1.5 dBFS (orbit_tool)
    return p.s;
}

Settings motionChop()
{
    P p ("MOTION CHOP");
    p.synth (0, "Velvet Pad", "CHOPPED PAD");
    p.gen (0, MFO_GEN_METAL_HIT, 0.5f, 77);
    p.matrix (0, 0.0f, 0.15f, 0.2f, 0.75f, 0.0f);
    p.L (0, MFO_L_SCAN_RATE) = 1.0f;
    p.filter (0, LADDER, 1800.0f, 0.45f, 0.35f);
    p.L (0, MFO_L_LEVEL) = 0.45f;
    p.mod (MFO_MS_EXT2, MFO_LPARAM (0, MFO_L_LEVEL), 0.5f);         // the stepper: 1/16 accents
    p.mod (MFO_MS_EXT2, MFO_LPARAM (0, MFO_L_CUTOFF), 0.25f);
    p.synth (1, "Hollow Stab", "METAL STAB");
    p.gen (1, MFO_GEN_METAL_HIT, 0.25f, 5);
    p.matrix (1, 0.4f, 0.0f, 0.5f, 0.6f, 0.0f);
    p.L (1, MFO_L_LEVEL) = 1.2f;
    p.mod (MFO_MS_EXT7, MFO_LPARAM (1, MFO_L_SCAN_POS), 0.8f);       // the beat clock scans the hit
    p.nodes (-0.7f, 0.6f, 0.7f, 0.6f, 0.0f, -0.75f, -0.7f, -0.7f);
    p.puck (-0.2f, 0.45f);
    p.orbit (pathHold, true, rate2bars, 0.5f, 0.1f);
    p.G (MFO_P_DRIFT) = 0.15f;
    p.s.bbdSync = 8;    // 3/4 beat = a dotted 1/8
    p.fx (1, 0.45f); p.fx (3, 0.3f); p.fx (5, 0.2f);
    p.macro (0, "CHOP", 0.6f); p.mod (MFO_MS_MACRO1, MFO_LPARAM (0, MFO_L_MOTION), 0.25f);
    p.macro (1, "GRIT", 0.3f); p.mod (MFO_MS_MACRO2, MFO_LPARAM (1, MFO_L_GRAIN), 0.5f);
    p.G (MFO_P_MASTER) = 0.49f;     // output level: about -19 dBFS RMS, peaks at or below -1.5 dBFS (orbit_tool)
    return p.s;
}

Settings threeBody()
{
    P p ("THREE-BODY ORBIT");
    p.synth (0, "Velvet Pad", "PLATE PAD");
    p.gen (0, MFO_GEN_PLATE, 3.0f, 8);
    p.matrix (0, 0.9f, 0.1f, 0.0f, 0.0f, 0.0f);
    p.amp (0, 0.8f, 1.0f, 1.0f, 2.5f);
    p.synth (1, "Silk Strings", "GLASS STRINGS");
    p.gen (1, MFO_GEN_GLASS, 3.0f, 3);
    p.matrix (1, 0.2f, 0.3f, 0.0f, 0.0f, 0.7f);
    p.L (1, MFO_L_TUNE) = 12.0f;
    p.L (1, MFO_L_SCAN_RATE) = 0.0f; p.L (1, MFO_L_SCAN_POS) = 0.05f;
    p.L (1, MFO_L_LEVEL) = 0.7f;
    p.amp (1, 1.2f, 1.0f, 1.0f, 3.0f);
    p.synth (2, "Night Wind", "VOICE WIND");
    p.gen (2, MFO_GEN_FORMANT_VOX, 3.0f, 5);
    p.matrix (2, 0.0f, 1.0f, 0.35f, 0.0f, 0.0f);
    p.L (2, MFO_L_SCAN_RATE) = 0.5f;
    p.amp (2, 1.0f, 1.0f, 1.0f, 3.0f);
    p.nodes (-0.7f, 0.6f, 0.7f, 0.6f, 0.0f, -0.75f, 0.0f, 0.05f);
    p.puck (0.0f, 0.1f);
    p.orbit (pathCircle, true, rate4bars, 0.7f, 0.35f);
    p.G (MFO_P_ORBIT_ROTATE) = 0.375f;
    p.G (MFO_P_DRIFT) = 0.4f;
    p.G (MFO_P_BLEND_SHARP) = 2.5f;
    p.fx (1, 0.2f); p.fx (2, 0.3f); p.fx (5, 0.4f);
    p.macro (0, "GRAVITY", 0.5f); p.mod (MFO_MS_MACRO1, MFO_P_BLEND_SHARP, 0.4f);
    p.macro (1, "SPEED", 0.0f); p.mod (MFO_MS_MACRO2, MFO_P_ORBIT_WOBBLE, 0.6f);
    p.G (MFO_P_MASTER) = 0.185f;     // output level: about -19 dBFS RMS, peaks at or below -1.5 dBFS (orbit_tool)
    return p.s;
}

Settings rustedSatellite()
{
    P p ("RUSTED SATELLITE");
    p.synth (0, "Walnut Bass", "SPRING BASS");
    p.gen (0, MFO_GEN_SPRING, 2.4f, 11);
    p.matrix (0, 0.72f, 0.4f, 0.18f, 0.0f, 0.35f);
    p.filter (0, LADDER, 1200.0f, 0.34f);
    p.amp (0, 0.004f, 0.82f, 0.6f, 1.4f);
    p.print (1, "Iron Bell", 55, 3.0f, 2.0f, "GLASS SAW");
    p.impPrint (1, "Glass Pluck", 72, 1.1f, 0.3f);
    p.matrix (1, 0.2f, 0.85f, 0.55f, 0.3f, 0.1f);
    p.L (1, MFO_L_TAPE) = 1;
    p.L (1, MFO_L_LOOP) = 1; p.L (1, MFO_L_LOOP_START) = 0.3f; p.L (1, MFO_L_XFADE) = 0.3f;
    p.filter (1, SVF_BP, 2800.0f, 0.52f);
    p.amp (1, 0.38f, 1.1f, 0.72f, 2.6f);
    p.L (1, MFO_L_LEVEL) = 0.75f;
    p.print (2, "Night Wind", 60, 3.0f, 2.8f, "STATIC CHOIR");
    p.live (2, 0);
    p.matrix (2, 0.0f, 0.3f, 0.9f, 0.75f, 0.6f);
    p.L (2, MFO_L_STRETCH) = 0.6f;
    p.L (2, MFO_L_LOOP) = 1;
    p.filter (2, LADDER, 640.0f, 0.71f);
    p.amp (2, 0.002f, 0.31f, 0.2f, 0.54f);
    p.L (2, MFO_L_LEVEL) = 0.9f;
    p.nodes (-0.6f, 0.45f, 0.65f, 0.45f, 0.1f, -0.65f, 0.0f, 0.0f);
    p.drawLoop (0.0f, -0.05f, 0.56f, 0.5f, -0.25f, 0.14f);
    p.orbit (pathDrawn, true, rate2bars, 0.64f, 0.22f);
    p.G (MFO_P_DRIFT) = 0.18f;
    p.s.tapeAge = 0.35f;
    p.s.bbdSync = 8;
    p.fx (1, 0.45f); p.fx (2, 0.3f); p.fx (3, 0.38f); p.fx (5, 0.32f);
    p.s.fxAmt[0] = 0.3f; p.s.fxAmt[4] = 0.25f;
    p.macro (0, "BLOOM", 0.62f); p.mod (MFO_MS_MACRO1, MFO_LPARAM (1, MFO_L_TONE), 0.2f); p.mod (MFO_MS_MACRO1, MFO_LPARAM (0, MFO_L_BODY), 0.2f);
    p.macro (1, "RUST", 0.35f); p.mod (MFO_MS_MACRO2, MFO_LPARAM (2, MFO_L_GRAIN), 0.3f); p.mod (MFO_MS_MACRO2, MFO_P_DRIVE, 0.4f);
    p.macro (2, "ORBIT SPEED", 0.5f); p.mod (MFO_MS_MACRO3, MFO_P_ORBIT_WOBBLE, 0.4f);
    p.macro (3, "GRAIN", 0.74f); p.mod (MFO_MS_MACRO4, MFO_LPARAM (1, MFO_L_GRAIN), 0.3f);
    p.G (MFO_P_MASTER) = 0.19f;     // output level: about -19 dBFS RMS, peaks at or below -1.5 dBFS (orbit_tool)
    return p.s;
}

// ---- 1.2.0 presets (programs 25+): held puck (a fixed blend, macros sweep it by hand), live INPUT
// the input presets need audio on the sidechain bus; without it they are quiet by design (silence in, silence out)
void handPuck (P& p, int kx, int ky)
{
    // macro kx sweeps the puck X by +-0.75 around the held point, macro ky the Y (macro at 0.5 = the held point)
    // (a mod adds amount x macro x range: 0.375 x 0..1 x 2 = 0..0.75, so the set point sits 0.375 lower)
    if (kx >= 0) { p.macro (kx, "PUCK X", 0.5f); p.mod (MFO_MS_MACRO1 + kx, MFO_P_PUCK_X, 0.375f); p.G (MFO_P_PUCK_X) -= 0.375f; }
    if (ky >= 0) { p.macro (ky, "PUCK Y", 0.5f); p.mod (MFO_MS_MACRO1 + ky, MFO_P_PUCK_Y, 0.375f); p.G (MFO_P_PUCK_Y) -= 0.375f; }
}

Settings inputSpringThroat()
{
    // the input is the impulse: a REGOLITH pad convolved with what arrives on the sidechain (a voice, drums,
    // a guitar), and the input itself through a spring on layer 2
    P p ("INPUT - SPRING THROAT", false);
    p.synth (0, "Velvet Pad", "THROAT PAD");
    p.s.impInput[0] = true;
    p.s.inWindow[0] = 0.25f;
    p.matrix (0, 0.9f, 0.25f, 0.0f, 0.35f, 0.0f);
    p.filter (0, LADDER, 4200.0f, 0.2f);
    p.amp (0, 0.02f, 1.0f, 1.0f, 1.2f);
    p.L (0, MFO_L_LEVEL) = 1.2f;
    p.s.srcInput[1] = true;
    p.s.layerName[1] = "SPRING IN";
    p.gen (1, MFO_GEN_SPRING, 2.5f, 13).damp = 0.35f;
    p.matrix (1, 0.7f, 0.0f, 0.0f, 0.0f, 0.0f);
    p.L (1, MFO_L_LEVEL) = 0.7f;
    p.nodes (-0.7f, 0.6f, 0.7f, 0.6f, 0.0f, -0.75f, -0.7f, -0.7f);
    p.puck (-0.35f, 0.55f);                  // mostly the pad, some spring
    p.orbit (pathHold, false, 0.3f, 0.5f, 0.0f);
    p.s.shape = pathCircle;
    p.G (MFO_P_DRIFT) = 0.1f;
    p.fx (5, 0.25f);                         // TWIN TANK (the spring is the layer's BODY)
    handPuck (p, 0, 1);
    p.macro (2, "THROAT", 0.5f); p.mod (MFO_MS_MACRO3, MFO_LPARAM (0, MFO_L_MOTION), 0.5f);
    p.G (MFO_P_MASTER) = 0.3f;               // about -19 dBFS RMS on the host test's demo sidechain
    return p.s;
}

Settings inputHaloFollower()
{
    // the input plays through a bowed-metal halo (THRU), and a pad follows its spectrum (TONE) and loudness
    // (MOTION) with the input as the modulator
    P p ("INPUT - HALO FOLLOWER", false);
    p.s.srcInput[0] = true;
    p.s.layerName[0] = "HALO";
    p.gen (0, MFO_GEN_BOWED_METAL, 4.0f, 21).gain = 0.75f;
    p.matrix (0, 0.75f, 0.0f, 0.0f, 0.0f, 0.0f);
    p.filter (0, SVF_HP, 160.0f, 0.1f);
    p.synth (1, "Silk Strings", "FOLLOWER");
    p.s.impInput[1] = true;
    p.matrix (1, 0.0f, 0.8f, 0.2f, 0.6f, 0.0f);
    p.amp (1, 0.3f, 1.0f, 1.0f, 2.0f);
    p.L (1, MFO_L_LEVEL) = 1.2f;
    p.nodes (-0.7f, 0.6f, 0.7f, 0.6f, 0.0f, -0.75f, -0.7f, -0.7f);
    p.puck (0.0f, 0.6f);                     // between the halo and the follower
    p.orbit (pathHold, false, 0.3f, 0.5f, 0.0f);
    p.s.shape = pathCircle;
    p.G (MFO_P_DRIFT) = 0.15f;
    p.fx (2, 0.3f); p.fx (5, 0.35f);
    handPuck (p, 0, 1);
    p.macro (2, "FOLLOW", 0.5f); p.mod (MFO_MS_MACRO3, MFO_LPARAM (1, MFO_L_TONE), 0.4f);
    p.G (MFO_P_MASTER) = 1.2f;                // about -19 dBFS RMS on the host test's demo sidechain
    return p.s;
}

Settings parkedTwinField()
{
    // a held blend of a plate pad and pitched glass strings: no motion; MACRO 1 / 2 move the puck by hand
    P p ("PARKED - TWIN FIELD", false);
    p.synth (0, "Velvet Pad", "PLATE PAD");
    p.gen (0, MFO_GEN_PLATE, 3.0f, 8);
    p.matrix (0, 0.85f, 0.1f, 0.0f, 0.0f, 0.0f);
    p.amp (0, 0.6f, 1.0f, 1.0f, 2.4f);
    p.synth (1, "Silk Strings", "GLASS STRINGS");
    p.gen (1, MFO_GEN_GLASS, 3.0f, 3);
    p.matrix (1, 0.3f, 0.3f, 0.0f, 0.0f, 0.6f);
    p.L (1, MFO_L_TUNE) = 12.0f;
    p.L (1, MFO_L_LEVEL) = 0.7f;
    p.amp (1, 0.9f, 1.0f, 1.0f, 2.8f);
    p.synth (2, "Night Wind", "WIND");
    p.gen (2, MFO_GEN_FORMANT_VOX, 3.0f, 5);
    p.matrix (2, 0.0f, 0.9f, 0.3f, 0.0f, 0.0f);
    p.L (2, MFO_L_SCAN_RATE) = 0.4f;
    p.amp (2, 1.0f, 1.0f, 1.0f, 3.0f);
    p.nodes (-0.7f, 0.6f, 0.7f, 0.6f, 0.0f, -0.75f, 0.0f, 0.05f);
    p.puck (0.0f, 0.3f);
    p.orbit (pathHold, false, 0.3f, 0.6f, 0.0f);
    p.s.shape = pathCircle;
    p.G (MFO_P_DRIFT) = 0.3f;
    p.fx (1, 0.2f); p.fx (2, 0.3f); p.fx (5, 0.4f);
    handPuck (p, 0, 1);
    p.macro (2, "GRAVITY", 0.5f); p.mod (MFO_MS_MACRO3, MFO_P_BLEND_SHARP, 0.4f);
    p.G (MFO_P_MASTER) = 0.112f;              // about -19 dBFS RMS (host test score)
    return p.s;
}

using Maker = Settings (*)();
// 0-7: the 1.1.0 presets (programs 17-24, unchanged); 8+: 1.2.0 (programs 25+)
const Maker makers[] = { rustedSatellite, bowedHalo, tapeChoir, springRain, woodMarimba, liveDrone, motionChop, threeBody,
                         inputSpringThroat, inputHaloFollower, parkedTwinField };
}

int factoryCount() { return (int) (sizeof (makers) / sizeof (makers[0])); }
Settings factoryPreset (int i) { return makers[juce::jlimit (0, factoryCount() - 1, i)](); }
juce::String factoryName (int i) { return factoryPreset (i).presetName; }

}
