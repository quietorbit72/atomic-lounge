// SPDX-License-Identifier: AGPL-3.0-only
// Vintage Chonk ORBIT: a development tool for the factory presets (sound design). Renders each preset with
// the engine host the plugin uses (orbit::Runner at 48 kHz / 256) and prints its level, the level of every
// layer on its own and the blend weights, optionally writing WAVs.
//   VintageChonkOrbitTool [outdir|-] [preset index] [output level to try] [chord: the host test's C chord instead]
#include <juce_audio_formats/juce_audio_formats.h>
#include <cstdio>
#include "OrbitRunner.h"

static std::vector<mfm1::MidiEvent> eventsFor (int i, double& seconds)
{
    struct E { double t; int note, vel; bool on; };
    std::vector<E> e;
    auto chords = [&] (std::initializer_list<std::vector<int>> cs, double each, double gap)
    {
        double t = 0.05;
        for (const auto& c : cs) { for (int n : c) { e.push_back ({ t, n, 100, true }); e.push_back ({ t + each - gap, n, 0, false }); } t += each; }
    };
    auto line = [&] (std::initializer_list<int> ns, double step, double len)
    {
        double t = 0.05;
        int k = 0;
        for (int n : ns) { e.push_back ({ t, n, k % 4 ? 80 : 100, true }); e.push_back ({ t + len, n, 0, false }); t += step; k++; }
    };
    switch (i)
    {
        case 99: chords ({ { 48, 55, 60, 64 } }, 1.4, 0.0); break;    // the host test's program check
        case 0: chords ({ { 36, 48, 55, 63 }, { 32, 44, 51, 60 }, { 34, 46, 53, 62 }, { 31, 43, 50, 58 } }, 2.4, 0.15); break;
        case 3: line ({ 72, 79, 84, 76, 81, 74, 79, 86, 77, 72, 84, 79, 76, 83, 74, 88, 81, 76, 72, 79 }, 0.4, 0.9); break;
        case 4: line ({ 60, 64, 67, 72, 71, 67, 64, 62, 65, 69, 72, 74, 72, 69, 65, 64, 60, 55, 60, 67, 72, 76, 74, 72 }, 0.25, 0.1); break;
        case 5: chords ({ { 36, 43, 60, 67 }, { 36, 43, 58, 65 } }, 5.0, 0.3); break;
        case 6: chords ({ { 41, 48, 53, 60 }, { 39, 46, 51, 58 } }, 4.3, 0.05); break;
        case 7: chords ({ { 50, 57, 62, 69 }, { 46, 53, 62, 65 }, { 48, 55, 60, 67 }, { 45, 52, 60, 64 } }, 2.6, 0.2); break;
        default: chords ({ { 50, 57, 62, 66 }, { 47, 54, 59, 62 }, { 43, 50, 59, 62 } }, 3.2, 0.25); break;
    }
    std::stable_sort (e.begin(), e.end(), [] (const E& a, const E& b) { return a.t < b.t; });
    seconds = e.back().t + 3.0;
    std::vector<mfm1::MidiEvent> out;
    for (const auto& x : e) out.push_back ({ (int) std::llround (x.t * 48000.0), { (uint8_t) (x.on ? 0x90 : 0x80), (uint8_t) x.note, (uint8_t) x.vel }, 3 });
    return out;
}

static juce::AudioBuffer<float> render (const orbit::Settings& s0, int idx, int solo, float* w)
{
    auto s = s0;                       // solo: the other layers' sources off (their dry share goes with them)
    for (int l = 0; l < orbit::layers; l++) if (solo >= 0 && l != solo) s.src[l].kind = orbit::Src::off;
    orbit::Runner r;
    r.prepare (48000.0, 256);
    const auto& hp = orbit::hostParams();
    for (int i = 0; i < (int) hp.size(); i++)
    {
        float v = orbit::hostNormFromSettings (hp[(size_t) i], s);
        r.setHostNorm (i, v);
    }
    for (int i = 0; i < orbit::numCore; i++) r.setCore (i, s.core[(size_t) i]);
    r.setPath (s.path, s.sync, s.drawn);
    r.setFx (s.fxOn, s.ensMode, s.revType, s.bbdSync, s.oilSync);
    r.setMods (s.mods);
    for (int l = 0; l < orbit::layers; l++) { r.requestSource (l, s.src[l]); r.requestImpulse (l, s.imp[l]); }
    r.createEngine();
    r.waitIdle (30000);
    r.process (nullptr, nullptr, 0, nullptr, 0);
    r.reset();
    double secs = 0;
    const auto ev = eventsFor (idx, secs);
    r.setTransport (112.0, false, 0.0);
    const int total = (int) (secs * 48000.0);
    juce::AudioBuffer<float> out (2, total);
    std::vector<mfm1::MidiEvent> blk;
    for (int pos = 0; pos < total; pos += 256)
    {
        const int n = std::min (256, total - pos);
        blk.clear();
        for (const auto& e : ev) if (e.offset >= pos && e.offset < pos + n) { auto c = e; c.offset -= pos; blk.push_back (c); }
        r.process (out.getWritePointer (0, pos), out.getWritePointer (1, pos), n, blk.data(), (int) blk.size());
        if (w != nullptr && pos == 48128) { const auto m = r.meter(); for (int k = 0; k < 4; k++) w[k] = m.w[k]; }
    }
    return out;
}

static double dbOf (double x) { return 20.0 * std::log10 (std::max (x, 1e-9)); }
static double peakOf (const juce::AudioBuffer<float>& b) { return std::max (b.getMagnitude (0, 0, b.getNumSamples()), b.getMagnitude (1, 0, b.getNumSamples())); }
static double rmsOf (const juce::AudioBuffer<float>& b)
{
    const double l = b.getRMSLevel (0, 0, b.getNumSamples()), r = b.getRMSLevel (1, 0, b.getNumSamples());
    return std::sqrt (0.5 * (l * l + r * r));
}

int main (int argc, char** argv)
{
    const juce::File outDir = argc > 1 && juce::String (argv[1]) != "-" ? juce::File (juce::String (argv[1])) : juce::File();
    const int only = argc > 2 ? juce::String (argv[2]).getIntValue() : -1;
    for (int i = 0; i < orbit::factoryCount(); i++)
    {
        if (only >= 0 && i != only) continue;
        auto s = orbit::factoryPreset (i);
        if (argc > 3) s.core[MFO_P_MASTER] = (float) juce::String (argv[3]).getDoubleValue();   // try another output level
        float w[4] {};
        const auto out = render (s, argc > 4 ? 99 : i, -1, w);
        std::printf ("%d %-24s rms %6.1f dBFS  peak %6.1f dBFS  blend L1 %.2f L2 %.2f L3 %.2f DRY %.2f\n", i, s.presetName.toRawUTF8(),
                     dbOf (rmsOf (out)), dbOf (peakOf (out)), w[0], w[1], w[2], w[3]);
        for (int l = 0; l < orbit::layers; l++)
        {
            if (s.src[l].kind == orbit::Src::off) continue;
            const auto so = render (s, i, l, nullptr);
            std::printf ("     layer %d %-16s rms %6.1f dBFS  peak %6.1f dBFS\n", l + 1, s.layerName[l].toRawUTF8(),
                         dbOf (rmsOf (so)), dbOf (peakOf (so)));
        }
        if (outDir != juce::File())
        {
            outDir.createDirectory();
            auto f = outDir.getChildFile (juce::String (i + 1) + "_" + s.presetName.replaceCharacters (" -", "__") + ".wav");
            f.deleteFile();
            std::unique_ptr<juce::OutputStream> os = std::make_unique<juce::FileOutputStream> (f);
            juce::WavAudioFormat wav;
            auto wr = wav.createWriterFor (os, juce::AudioFormatWriterOptions {}.withSampleRate (48000.0).withNumChannels (2).withBitsPerSample (24));
            if (wr != nullptr) wr->writeFromAudioSampleBuffer (out, 0, out.getNumSamples());
        }
    }
    return 0;
}
