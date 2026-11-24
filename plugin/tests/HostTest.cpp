// SPDX-License-Identifier: AGPL-3.0-only
// Atomic Lounge (formerly Vintage Chonk) VST3 host test: loads the built plugin the way a DAW does (JUCE's VST3 hosting), plays
// MIDI into it at 44.1 / 48 / 96 kHz with block sizes 32 / 64 / 441 / 512 / 1024 and checks:
//   * sound: non-silent, finite, no overload;
//   * block boundaries: every block size gives the same output (bit for bit), and at 44.1 kHz the
//     plugin equals the engine rendered offline (dsp/, events at the nearest 128-frame block, 64 later);
//   * resampler: the kernel's response (pass band, image rejection), the reported latency, and the
//     resampled output against an exact offline resample of the offline engine render;
//   * state save / restore round trip, host programs, parameter automation moves the sound, host tempo;
//   * EngineRunner directly: no heap allocation while processing;
//   * the editor (WebView2 panel) opens, the panel talks to the plugin both ways, and a screenshot;
//   * the VST3 parameter list a DAW shows: only ours (+ program / bypass) without kIsHidden (JUCE patch);
//   * mod wheel, pitch bend and sustain reach the engine through the VST3 path (IMidiMapping);
//   * user presets of the pre-rename folder are migrated once.
//   * 1.2.0: the Sidechain input bus (layout, arrangements, absent / silent / noise input), ORBIT INPUT in every
//     dimension, the held / stopped orbit and puck automation, 1.1.0 states bit-identical against the released
//     1.1.0 bundle (plugin/dist, loaded read-only), the WebView2 folder per host and the editor's fallback.
//   VintageChonkHostTest "<path>\Atomic Lounge.vst3" <out dir> [--no-screenshot] [--ref "<1.1.0 .vst3>"]
//   VintageChonkHostTest --editor-child "<.vst3>" <status file> <seconds>      (a second "host" for the editor test)
#define NOMINMAX
#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_audio_utils/juce_audio_utils.h>
#include <juce_gui_extra/juce_gui_extra.h>
#include <windows.h>
#include <atomic>
#include <chrono>
#include <complex>
#include <cstdio>
#include <cstdlib>
#include <new>
#include <set>
#include <map>
#include <pluginterfaces/base/ipluginbase.h>
#include <pluginterfaces/vst/ivstcomponent.h>
#include <pluginterfaces/vst/ivstaudioprocessor.h>
#include <pluginterfaces/vst/ivsteditcontroller.h>
#include <pluginterfaces/vst/ivstmessage.h>
#include "EngineRunner.h"
#include "PresetStore.h"
#include "Params.h"
#include "factory_presets.h"
#include "FxChain.h"
#include "OrbitModel.h"
#include "OrbitRunner.h"

// ------------------------------------------------------------ allocation counter (this thread) ---
static thread_local bool countAllocs = false;
static std::atomic<long> allocs { 0 };
void* operator new (size_t n)
{
    if (countAllocs) allocs++;
    if (void* p = std::malloc (n ? n : 1)) return p;
    throw std::bad_alloc();
}
void* operator new[] (size_t n) { return operator new (n); }
void operator delete (void* p) noexcept { std::free (p); }
void operator delete[] (void* p) noexcept { std::free (p); }
void operator delete (void* p, size_t) noexcept { std::free (p); }
void operator delete[] (void* p, size_t) noexcept { std::free (p); }

static int passes = 0, fails = 0;
static void check (bool ok, const juce::String& what)
{
    if (ok) passes++;
    else fails++;
    std::printf ("%s %s\n", ok ? "  ok  " : "  FAIL", what.toRawUTF8());
    std::fflush (stdout);
}
static void info (const juce::String& s) { std::printf ("        %s\n", s.toRawUTF8()); std::fflush (stdout); }

// ------------------------------------------------------------------------------- MIDI script ---
struct Ev { double t; uint8_t b[3]; };
static std::vector<Ev> script()
{
    return {
        { 0.050, { 0x90, 48, 100 } }, { 0.100, { 0x90, 55, 90 } }, { 0.150, { 0x90, 60, 110 } },
        { 0.300, { 0xB0, 1, 80 } },   { 0.500, { 0xE0, 0x20, 0x5D } }, { 0.600, { 0xD0, 90, 0 } },
        { 0.800, { 0xB0, 64, 127 } }, { 1.000, { 0x80, 48, 0 } }, { 1.000, { 0x80, 55, 0 } },
        { 1.010, { 0x80, 60, 0 } },   { 1.200, { 0xE0, 0x00, 0x40 } }, { 1.400, { 0xB0, 64, 0 } },
        { 1.500, { 0x90, 67, 120 } }, { 1.733, { 0x90, 70, 64 } }, { 2.000, { 0x80, 67, 0 } },
        { 2.050, { 0x90, 72, 100 } }, { 2.300, { 0x80, 72, 0 } },  { 2.310, { 0x80, 70, 0 } },
    };
}
static int evSize (const Ev& e) { return (e.b[0] & 0xF0) == 0xD0 || (e.b[0] & 0xF0) == 0xC0 ? 2 : 3; }

struct TempoHead final : juce::AudioPlayHead
{
    double bpm = 120.0;
    juce::Optional<PositionInfo> getPosition() const override
    {
        PositionInfo p;
        p.setBpm (bpm);
        p.setIsPlaying (true);
        return p;
    }
};

static juce::AudioBuffer<float> renderPlugin (juce::AudioPluginInstance& p, double fs, int bs, double seconds,
                                              const std::vector<Ev>& evs, std::function<void (int64_t)> perBlock = {})
{
    p.setRateAndBufferSizeDetails (fs, bs);
    p.prepareToPlay (fs, bs);
    const int total = (int) std::lround (seconds * fs);
    juce::AudioBuffer<float> out (2, total), buf (2, bs);
    juce::MidiBuffer midi;
    for (int pos = 0; pos < total; pos += bs)
    {
        const int n = std::min (bs, total - pos);
        if (perBlock) perBlock (pos);
        midi.clear();
        for (const auto& e : evs)
        {
            const int64_t m = std::llround (e.t * fs);
            if (m >= pos && m < pos + n) midi.addEvent (e.b, evSize (e), (int) (m - pos));
        }
        buf.setSize (2, n, false, false, true);
        buf.clear();
        p.processBlock (buf, midi);
        for (int c = 0; c < 2; c++) out.copyFrom (c, pos, buf, c, 0, n);
    }
    p.releaseResources();
    return out;
}

// a render with something on the sidechain: in (ch, sample) gives the input; inCh 0 = the input bus switched off
static juce::AudioBuffer<float> renderIn (juce::AudioPluginInstance& p, double fs, int bs, double seconds, const std::vector<Ev>& evs,
                                      std::function<float (int, int64_t)> in, int inCh)
{
    auto lay = p.getBusesLayout();
    lay.inputBuses.getReference (0) = inCh == 0 ? juce::AudioChannelSet::disabled() : inCh == 1 ? juce::AudioChannelSet::mono() : juce::AudioChannelSet::stereo();
    p.releaseResources();
    p.setBusesLayout (lay);
    p.setRateAndBufferSizeDetails (fs, bs);
    p.prepareToPlay (fs, bs);
    const int total = (int) std::lround (seconds * fs);
    juce::AudioBuffer<float> out (2, total), buf (2, bs);
    juce::MidiBuffer midi;
    for (int pos = 0; pos < total; pos += bs)
    {
        const int n = std::min (bs, total - pos);
        midi.clear();
        for (const auto& e : evs)
        {
            const int64_t m = std::llround (e.t * fs);
            if (m >= pos && m < pos + n) midi.addEvent (e.b, evSize (e), (int) (m - pos));
        }
        buf.setSize (2, n, false, false, true);
        buf.clear();
        if (in && inCh > 0)
            for (int c = 0; c < inCh; c++)
                for (int i = 0; i < n; i++) buf.setSample (c, i, in (c, pos + i));
        p.processBlock (buf, midi);
        for (int c = 0; c < 2; c++) out.copyFrom (c, pos, buf, c, 0, n);
    }
    p.releaseResources();
    lay.inputBuses.getReference (0) = juce::AudioChannelSet::stereo();
    p.setBusesLayout (lay);
    return out;
}

// a demo sidechain (own material, generated): a soft kick every half beat at 120 BPM, a noise tick between, a held
// 110 Hz tone of a few harmonics; about -14 dBFS peak
static float demoInput (int c, int64_t i)
{
    const double t = (double) i / 48000.0, beat = std::fmod (t, 0.5), half = std::fmod (t + 0.25, 0.5);
    const double kick = 0.35 * std::exp (-beat * 18.0) * std::sin (2.0 * 3.14159265358979 * (55.0 + 60.0 * std::exp (-beat * 30.0)) * beat);
    uint32_t x = (uint32_t) (i * 2654435761u) ^ (uint32_t) (c * 0x9e3779b9u);
    x ^= x >> 13; x *= 0x5bd1e995u; x ^= x >> 15;
    const double tick = 0.12 * std::exp (-half * 60.0) * ((double) (x >> 8) / 8388608.0 - 1.0);
    double tone = 0;
    for (int h = 1; h <= 6; h++) tone += std::sin (2.0 * 3.14159265358979 * 110.0 * h * t + 0.3 * h) / h;
    return (float) (kick + tick + 0.05 * tone);
}

struct Stats { double rms = 0, peak = 0; bool finite = true; };
static Stats stats (const juce::AudioBuffer<float>& b, int from = 0, int to = -1)
{
    Stats s;
    double sum = 0;
    int64_t cnt = 0;
    if (to < 0) to = b.getNumSamples();
    for (int c = 0; c < b.getNumChannels(); c++)
        for (int i = from; i < to; i++)
        {
            const float v = b.getSample (c, i);
            if (! std::isfinite (v)) { s.finite = false; continue; }
            sum += (double) v * v;
            s.peak = std::max (s.peak, (double) std::fabs (v));
            cnt++;
        }
    s.rms = cnt ? std::sqrt (sum / (double) cnt) : 0;
    return s;
}
static double db (double x) { return 20.0 * std::log10 (std::max (x, 1e-12)); }
static double maxDiff (const juce::AudioBuffer<float>& a, const juce::AudioBuffer<float>& b)
{
    double d = 0;
    const int n = std::min (a.getNumSamples(), b.getNumSamples());
    for (int c = 0; c < 2; c++)
        for (int i = 0; i < n; i++) d = std::max (d, (double) std::fabs (a.getSample (c, i) - b.getSample (c, i)));
    return d;
}
// brightness: RMS of the first difference over RMS
static double brightness (const juce::AudioBuffer<float>& b, int from, int to)
{
    double e = 0, d = 0;
    for (int i = from + 1; i < to; i++)
    {
        const double x = b.getSample (0, i), px = b.getSample (0, i - 1);
        e += x * x;
        d += (x - px) * (x - px);
    }
    return e > 0 ? std::sqrt (d / e) : 0;
}

// the engine alone at 44.1 kHz, events applied at the 128-frame block nearest to their time
static juce::AudioBuffer<float> renderReference (const mfe_patch_t& patch, int frames, const std::vector<Ev>& evs)
{
    mfm1::initTablesOnce();
    auto* e = static_cast<mfe_engine_t*> (std::calloc (1, mfe_engine_size()));
    mfe_engine_init (e);
    mfe_patch_t tmp = patch;
    if (tmp.max_voices == 8) tmp.max_voices = MFE_MAX_VOICES;
    mfe_engine_set_patch (e, &tmp);
    mfe_set_tempo (e, 120u << 8);
    juce::AudioBuffer<float> out (2, frames);
    out.clear();
    size_t next = 0;
    int32_t blk[MFE_BLOCK * 2];
    for (int b = 0; b < frames; b += MFE_BLOCK)
    {
        while (next < evs.size() && std::llround (evs[next].t * 44100.0) < b + MFE_BLOCK / 2)
        {
            mfe_midi (e, evs[next].b, (size_t) evSize (evs[next]));
            next++;
        }
        mfe_render (e, blk);
        for (int i = 0; i < MFE_BLOCK && b + i < frames; i++)
        {
            out.setSample (0, b + i, (float) blk[2 * i] * (1.0f / 32768.0f));
            out.setSample (1, b + i, (float) blk[2 * i + 1] * (1.0f / 32768.0f));
        }
    }
    std::free (e);
    return out;
}

static juce::Image captureClient (HWND h)
{
    RECT r;
    GetClientRect (h, &r);
    const int w = r.right - r.left, hh = r.bottom - r.top;
    juce::Image img (juce::Image::RGB, std::max (1, w), std::max (1, hh), true);
    if (w <= 0 || hh <= 0) return img;
    HDC wdc = GetDC (h);
    HDC mdc = CreateCompatibleDC (wdc);
    HBITMAP bmp = CreateCompatibleBitmap (wdc, w, hh);
    auto old = SelectObject (mdc, bmp);
    PrintWindow (h, mdc, PW_CLIENTONLY | 2 /* PW_RENDERFULLCONTENT */);
    BITMAPINFO bi {};
    bi.bmiHeader.biSize = sizeof (BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = w;
    bi.bmiHeader.biHeight = -hh;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    std::vector<uint8_t> px ((size_t) w * (size_t) hh * 4);
    GetDIBits (mdc, bmp, 0, (UINT) hh, px.data(), &bi, DIB_RGB_COLORS);
    {
        juce::Image::BitmapData bd (img, juce::Image::BitmapData::writeOnly);
        for (int y = 0; y < hh; y++)
            for (int x = 0; x < w; x++)
            {
                const uint8_t* p = &px[((size_t) y * (size_t) w + (size_t) x) * 4];
                bd.setPixelColour (x, y, juce::Colour (p[2], p[1], p[0]));
            }
    }
    SelectObject (mdc, old);
    DeleteObject (bmp);
    DeleteDC (mdc);
    ReleaseDC (h, wdc);
    return img;
}

// the plugin's own state record inside the VST3 host's wrapper (VST3PluginState / IComponent, base64)
static std::unique_ptr<juce::XmlElement> pluginState (juce::AudioPluginInstance& p)
{
    juce::MemoryBlock mb;
    p.getStateInformation (mb);
    auto host = juce::AudioProcessor::getXmlFromBinary (mb.getData(), (int) mb.getSize());
    auto* comp = host != nullptr ? host->getChildByName ("IComponent") : nullptr;
    if (comp == nullptr)
    {
        info ("host state without IComponent: " + (host != nullptr ? host->toString().substring (0, 300) : juce::String ("not XML, ") + juce::String ((int) mb.getSize()) + " bytes"));
        return {};
    }
    juce::MemoryBlock raw;
    raw.fromBase64Encoding (comp->getAllSubText());
    const std::string bytes (static_cast<const char*> (raw.getData()), raw.getSize());   // binary header first
    // the record ends with "/>" (1.0.0, no children) or "</VINTAGECHONK>" (1.1.0: the <ORBIT> child)
    const auto a0 = bytes.find ("<VINTAGECHONK");
    auto b0 = a0 == std::string::npos ? a0 : bytes.find ("</VINTAGECHONK>", a0);
    size_t endLen = 15;
    if (a0 != std::string::npos && b0 == std::string::npos) { b0 = bytes.find ("/>", a0); endLen = 2; }
    const auto text = juce::String::fromUTF8 (bytes.c_str(), (int) bytes.size());
    const int a = a0 == std::string::npos ? -1 : (int) a0, b = b0 == std::string::npos ? -1 : (int) b0;
    if (a < 0 || b < 0)
    {
        info ("no plugin record in the host state (" + juce::String ((int) raw.getSize()) + " bytes): " + text.substring (0, 120).removeCharacters ("\r\n"));
        return {};
    }
    return juce::parseXML (juce::String::fromUTF8 (bytes.c_str() + a, b + (int) endLen - a));
}
// give the plugin one of its own state records, wrapped the way JUCE's VST3 host stores it
static void setPluginState (juce::AudioPluginInstance& p, const juce::MemoryBlock& own)
{
    juce::XmlElement host ("VST3PluginState");
    host.createNewChildElement ("IComponent")->addTextElement (own.toBase64Encoding());
    juce::MemoryBlock mb;
    juce::AudioProcessor::copyXmlToBinary (host, mb);
    p.setStateInformation (mb.getData(), (int) mb.getSize());
}
static juce::String stateUi (juce::AudioPluginInstance& p)
{
    auto x = pluginState (p);
    return x != nullptr ? x->getStringAttribute ("ui") : juce::String();
}

// a hosted VST3 parameter's ID is the VST3 number; find ours by the name the plugin gives each id
static juce::AudioProcessorParameter* paramById (juce::AudioPluginInstance& p, const juce::String& id)
{
    juce::String name;
    for (const auto& d : mfm1::paramDefs) if (id == d.id) name = d.name;
    for (auto* prm : p.getParameters())
        if (prm->getName (64) == name) return prm;
    return nullptr;
}

static bool pumpUntil (std::function<bool()> done, int ms)
{
    const auto end = juce::Time::getMillisecondCounter() + (juce::uint32) ms;
    while (juce::Time::getMillisecondCounter() < end)
    {
        juce::MessageManager::getInstance()->runDispatchLoopUntil (50);
        if (done && done()) return true;
    }
    return ! done ? true : done();
}

static void savePng (const juce::Image& img, const juce::File& f)
{
    f.deleteFile();
    juce::FileOutputStream os (f);
    juce::PNGImageFormat png;
    png.writeImageToStream (img, os);
}

// The parameter list a DAW builds: the plugin's own edit controller (a second instance from the bundle's
// factory, connected to the hosted component like a host does) and each parameter's VST3 flags.
struct Vst3ParamList { int total = -1, visible = 0, hiddenCc = 0; juce::StringArray visibleNames; };
static Vst3ParamList vst3ParamList (juce::AudioPluginInstance& p, const juce::File& bundle)
{
    using namespace Steinberg;
    Vst3ParamList r;
    struct { Vst::IComponent* comp = nullptr; } v;
    if (auto* c = p.getVST3Client()) v.comp = c->getIComponentPtr();
    const auto dll = bundle.getChildFile ("Contents").getChildFile ("x86_64-win").getChildFile (bundle.getFileName());
    HMODULE mod = LoadLibraryW (dll.getFullPathName().toWideCharPointer());
    if (v.comp == nullptr || mod == nullptr) return r;
    using GetFactory = IPluginFactory* (PLUGIN_API*)();
    auto gf = reinterpret_cast<GetFactory> (reinterpret_cast<void*> (GetProcAddress (mod, "GetPluginFactory")));
    IPluginFactory* factory = gf != nullptr ? gf() : nullptr;
    TUID cid {};
    Vst::IEditController* ec = nullptr;
    if (factory != nullptr && v.comp->getControllerClassId (cid) == kResultOk
        && factory->createInstance (cid, Vst::IEditController::iid, reinterpret_cast<void**> (&ec)) == kResultOk && ec != nullptr)
    {
        ec->initialize (nullptr);
        Vst::IConnectionPoint *ecp = nullptr, *ccp = nullptr;
        ec->queryInterface (Vst::IConnectionPoint::iid, reinterpret_cast<void**> (&ecp));
        v.comp->queryInterface (Vst::IConnectionPoint::iid, reinterpret_cast<void**> (&ccp));
        if (ecp != nullptr && ccp != nullptr) ecp->connect (ccp);
        r.total = ec->getParameterCount();
        for (int32 i = 0; i < r.total; i++)
        {
            Vst::ParameterInfo pi {};
            if (ec->getParameterInfo (i, pi) != kResultOk) continue;
            const juce::String title (juce::CharPointer_UTF16 (reinterpret_cast<const juce::CharPointer_UTF16::CharType*> (pi.title)));
            if ((pi.flags & Vst::ParameterInfo::kIsHidden) == 0) { r.visible++; r.visibleNames.add (title); }
            else if (title.startsWith ("MIDI CC ")) r.hiddenCc++;
        }
        if (ecp != nullptr && ccp != nullptr) ecp->disconnect (ccp);
        if (ccp != nullptr) ccp->release();
        if (ecp != nullptr) ecp->release();
        ec->terminate();
        ec->release();
    }
    if (factory != nullptr) factory->release();
    FreeLibrary (mod);
    return r;
}

// zero crossings of the left channel (a pitch measure for one held note)
static int zeroCrossings (const juce::AudioBuffer<float>& b, int from, int to)
{
    int n = 0;
    for (int i = from + 1; i < to; i++)
        if ((b.getSample (0, i - 1) < 0.0f) != (b.getSample (0, i) < 0.0f)) n++;
    return n;
}

// ------------------------------------------------------------------------------- ORBIT helpers ---
// the settings into a runner the way the processor does it (PluginProcessor::orbitApplyAll)
static void orbitApply (orbit::Runner& r, const orbit::Settings& s)
{
    const auto& hp = orbit::hostParams();
    for (int i = 0; i < (int) hp.size(); i++) r.setHostNorm (i, orbit::hostNormFromSettings (hp[(size_t) i], s));
    for (int i = 0; i < orbit::numCore; i++) r.setCore (i, s.core[(size_t) i]);
    r.setPath (s.path != orbit::pathHold ? s.path : s.shape, s.sync, s.drawn);
    r.setExtras (s.legacyWobble, s.inWindow);
    r.setFx (s.fxOn, s.ensMode, s.revType, s.bbdSync, s.oilSync);
    r.setMods (s.mods);
    for (int l = 0; l < orbit::layers; l++) { r.requestSource (l, s.src[l]); r.requestImpulse (l, s.imp[l]); }
}
// a plugin state record (the plugin's own XML) with an ORBIT settings block and a mode
static juce::MemoryBlock orbitBlob (juce::AudioPluginInstance& p, const orbit::Settings* s, const char* mode)
{
    auto x = pluginState (p);
    juce::MemoryBlock mb;
    if (x == nullptr) return mb;
    x->setAttribute ("mode", mode);
    if (s != nullptr)
    {
        if (auto* old = x->getChildByName ("ORBIT")) x->removeChildElement (old, true);
        x->addChildElement (orbit::toXml (*s).release());
    }
    juce::AudioProcessor::copyXmlToBinary (*x, mb);
    return mb;
}
static juce::AudioProcessorParameter* paramByName (juce::AudioPluginInstance& p, const juce::String& name)
{
    for (auto* prm : p.getParameters()) if (prm->getName (64) == name) return prm;
    return nullptr;
}
// musical scores for the preset renders (seconds, MIDI)
static std::vector<Ev> chordScore (std::initializer_list<std::vector<int>> chords, double each, double gap, int vel = 100)
{
    std::vector<Ev> e;
    double t = 0.05;
    for (const auto& c : chords)
    {
        for (int n : c) e.push_back ({ t, { 0x90, (uint8_t) n, (uint8_t) vel } });
        for (int n : c) e.push_back ({ t + each - gap, { 0x80, (uint8_t) n, 0 } });
        t += each;
    }
    return e;
}
static std::vector<Ev> lineScore (std::initializer_list<int> notes, double step, double len, int vel = 100)
{
    std::vector<Ev> e;
    double t = 0.05;
    int k = 0;
    for (int n : notes)
    {
        e.push_back ({ t, { 0x90, (uint8_t) n, (uint8_t) (vel - (k % 4 ? 20 : 0)) } });
        e.push_back ({ t + len, { 0x80, (uint8_t) n, 0 } });
        t += step;
        k++;
    }
    return e;
}
static void sortScore (std::vector<Ev>& e) { std::stable_sort (e.begin(), e.end(), [] (const Ev& a, const Ev& b) { return a.t < b.t; }); }
static bool writeWav (const juce::File& f, const juce::AudioBuffer<float>& b, double fs, float& gainOut)
{
    float pk = 0.0f;
    for (int c = 0; c < b.getNumChannels(); c++) pk = std::max (pk, b.getMagnitude (c, 0, b.getNumSamples()));
    gainOut = pk > 0.891f ? 0.891f / pk : 1.0f;      // never above -1 dBFS
    juce::AudioBuffer<float> o (b);
    o.applyGain (gainOut);
    f.getParentDirectory().createDirectory();
    f.deleteFile();
    std::unique_ptr<juce::OutputStream> os = std::make_unique<juce::FileOutputStream> (f);
    juce::WavAudioFormat wav;
    auto w = wav.createWriterFor (os, juce::AudioFormatWriterOptions {}.withSampleRate (fs).withNumChannels (2).withBitsPerSample (24));
    return w != nullptr && w->writeFromAudioSampleBuffer (o, 0, o.getNumSamples());
}
static double maxJump (const juce::AudioBuffer<float>& b, int from, int to)
{
    double m = 0;
    for (int c = 0; c < 2; c++)
        for (int i = std::max (1, from); i < to; i++) m = std::max (m, (double) std::fabs (b.getSample (c, i) - b.getSample (c, i - 1)));
    return m;
}

// what the plugin pushes to the ORBIT panel (PluginProcessor::orbitState), built from the same model and
// runner for the factory preset 1: the fixture of plugin/tests/test_panel_desktop.mjs
static juce::var orbitPanelState()
{
    const auto s = orbit::factoryPreset (0);
    orbit::Runner r;
    r.prepare (48000.0, 256);
    orbitApply (r, s);
    r.waitIdle (20000);
    auto* o = new juce::DynamicObject();
    o->setProperty ("settings", orbit::toVar (s));
    juce::Array<juce::var> host, hv;
    for (const auto& h : orbit::hostParams())
    {
        auto* e = new juce::DynamicObject();
        e->setProperty ("id", h.id);
        e->setProperty ("name", h.name);
        e->setProperty ("target", h.target);
        e->setProperty ("map", h.map == orbit::Map::lin ? "lin" : h.map == orbit::Map::sq2 ? "sq2" : h.map == orbit::Map::log ? "log" : "special");
        host.add (juce::var (e));
        hv.add (orbit::hostNormFromSettings (h, s));
    }
    o->setProperty ("host", host);
    o->setProperty ("hostValues", hv);
    juce::Array<juce::var> core;
    for (int i = 0; i < orbit::numCore; i++)
    {
        const auto* in = mfo_param_info (i);
        auto* e = new juce::DynamicObject();
        e->setProperty ("name", juce::String (in->name));
        e->setProperty ("min", in->min);
        e->setProperty ("max", in->max);
        e->setProperty ("def", in->def);
        e->setProperty ("int", (in->flags & MFO_PF_INT) != 0);
        e->setProperty ("log", (in->flags & MFO_PF_LOG) != 0);
        core.add (juce::var (e));
    }
    o->setProperty ("core", core);
    juce::Array<juce::var> chonk;
    for (int i = 0; i < mfm1_factory::count; i++)
    {
        auto* e = new juce::DynamicObject();
        e->setProperty ("name", juce::String (mfm1_factory::names[i]));
        e->setProperty ("kind", "F");
        chonk.add (juce::var (e));
    }
    o->setProperty ("chonk", chonk);
    juce::Array<juce::var> fac, gens, views;
    for (int i = 0; i < orbit::factoryCount(); i++) fac.add (orbit::factoryName (i));
    o->setProperty ("factory", fac);
    o->setProperty ("user", juce::Array<juce::var>());
    for (int i = 0; i < MFO_GEN_COUNT; i++) gens.add (juce::String (mfo_generate_name (i)));
    o->setProperty ("gens", gens);
    auto peaks = [] (const std::vector<float>& p) { juce::Array<juce::var> a; for (float x : p) a.add (std::round (x * 1000.0f) / 1000.0f); return a; };
    for (int l = 0; l < orbit::layers; l++)
    {
        const auto v = r.view (l);
        auto* e = new juce::DynamicObject();
        e->setProperty ("layer", l);
        e->setProperty ("srcName", v.srcName); e->setProperty ("srcSub", v.srcSub); e->setProperty ("srcPeaks", peaks (v.srcPeaks));
        e->setProperty ("srcTrace", v.srcTrace); e->setProperty ("srcError", v.srcError);
        e->setProperty ("impName", v.impName); e->setProperty ("impSub", v.impSub); e->setProperty ("impError", v.impError);
        if (s.imp[l].kind == orbit::Imp::live)
        {
            const auto ov = r.view (s.imp[l].liveLayer);
            e->setProperty ("impPeaks", peaks (ov.srcPeaks));
            e->setProperty ("impTrace", ov.srcTrace);
        }
        else { e->setProperty ("impPeaks", peaks (v.impPeaks)); e->setProperty ("impTrace", false); }
        views.add (juce::var (e));
    }
    o->setProperty ("views", views);
    o->setProperty ("problems", juce::Array<juce::var>());
    auto* m = new juce::DynamicObject();
    m->setProperty ("cpu", 0.11); m->setProperty ("voices", 6); m->setProperty ("rate", 48000.0); m->setProperty ("latencyMs", 7.23);
    m->setProperty ("puck", juce::Array<juce::var> { 0.38, 0.61 }); m->setProperty ("w", juce::Array<juce::var> { 0.42, 0.18, 0.31, 0.09 });
    m->setProperty ("vu", juce::Array<juce::var> { 0.1, 0.1 }); m->setProperty ("pk", juce::Array<juce::var> { 0.4, 0.4 }); m->setProperty ("midi", 0);
    m->setProperty ("mode", "orbit");
    o->setProperty ("meter", juce::var (m));
    return juce::var (o);
}

// a second "host" for the editor test (this program copied under another name): opens the plugin's editor and
// waits until its status file says the panel loaded or fell back. Exit 0 loaded, 2 fallback, 1 neither.
static int editorChild (const juce::String& path, const juce::File& status, int seconds)
{
    SetEnvironmentVariableW (L"ATOMIC_LOUNGE_TEST_STATUS", status.getFullPathName().toWideCharPointer());
    juce::AudioPluginFormatManager fm;
    juce::addDefaultFormatsToManager (fm);
    juce::OwnedArray<juce::PluginDescription> types;
    for (auto* f : fm.getFormats())
        if (auto* v = dynamic_cast<juce::VST3PluginFormat*> (f)) v->findAllTypesForFile (types, path);
    if (types.isEmpty()) return 1;
    juce::String err;
    auto p = fm.createPluginInstance (*types[0], 48000.0, 256, err);
    if (p == nullptr) return 1;
    std::unique_ptr<juce::AudioProcessorEditor> ed (p->createEditorIfNeeded());
    if (ed == nullptr) return 1;
    juce::DocumentWindow win ("Atomic Lounge (second host)", juce::Colours::black, 0, true);
    win.setUsingNativeTitleBar (true);
    win.setContentNonOwned (ed.get(), true);
    win.setTopLeftPosition (120, 120);
    win.setVisible (true);
    pumpUntil ([&] { const auto t = status.loadFileAsString().removeCharacters ("\r"); return t.contains ("\nloaded") || t.contains ("\nfallback"); }, seconds * 1000);
    pumpUntil ({}, 500);
    const auto t = status.loadFileAsString().removeCharacters ("\r");
    win.setVisible (false);
    win.clearContentComponent();
    ed.reset();
    pumpUntil ({}, 300);
    p.reset();
    return t.contains ("\nloaded") ? 0 : t.contains ("\nfallback") ? 2 : 1;
}

int main (int argc, char** argv)
{
    juce::ScopedJuceInitialiser_GUI gui;
    if (argc > 4 && juce::String (argv[1]) == "--editor-child") return editorChild (juce::String (argv[2]), juce::File (juce::String (argv[3])), std::atoi (argv[4]));
    const juce::String path = argc > 1 ? juce::String (argv[1]) : juce::String (MFM1_VST3_PATH);
    const juce::File outDir (argc > 2 ? juce::String (argv[2]) : juce::File::getCurrentWorkingDirectory().getFullPathName());
    bool shots = true;
    for (int i = 1; i < argc; i++) if (juce::String (argv[i]) == "--no-screenshot") shots = false;
    // the released 1.1.0 bundle (read only) for the bit-identity checks: --ref <bundle>, else plugin/dist next to the
    // dev bundle's folder, else release/v1.1.0-bundle (where the 1.2.0 release moved it). With --ref the reference is
    // required; without one anywhere (a fresh clone) the 1.1.0 states are checked on the saved fixtures alone.
    const auto pluginDir = juce::File (path).getParentDirectory().getParentDirectory();
    juce::String refPath = pluginDir.getChildFile ("dist").getChildFile ("Vintage Chonk.vst3").getFullPathName();
    if (! juce::File (refPath).exists())
        refPath = pluginDir.getParentDirectory().getChildFile ("release/v1.1.0-bundle/Vintage Chonk.vst3").getFullPathName();
    bool refRequired = false;
    for (int i = 1; i + 1 < argc; i++) if (juce::String (argv[i]) == "--ref") { refPath = juce::String (argv[i + 1]); refRequired = true; }
    outDir.createDirectory();
    std::printf ("Atomic Lounge host test\n  plugin: %s\n", path.toRawUTF8());

    // ---------------------------------------------------------------- 1. EngineRunner directly ---
    std::printf ("[engine runner]\n");
    mfe_patch_t f0;
    {
        check (mfe_preset_size() == mfm1_factory::size, "C preset size " + juce::String ((int) mfe_preset_size()) + " == the panel's " + juce::String ((int) mfm1_factory::size));
        check (mfe_preset_read (&f0, mfm1_factory::images[0], mfm1_factory::size) == 0, "factory image 0 reads in C");
        int rt = 0;
        for (int i = 0; i < mfm1_factory::count; i++)
        {
            mfe_patch_t q;
            uint8_t back[1024];
            if (mfe_preset_read (&q, mfm1_factory::images[i], mfm1_factory::size) == 0
                && mfe_preset_write (&q, back, sizeof back) == mfm1_factory::size
                && std::memcmp (back, mfm1_factory::images[i], mfm1_factory::size) == 0) rt++;
        }
        check (rt == mfm1_factory::count, juce::String (rt) + " of 16 factory images round-trip byte for byte through the C reader/writer");

        for (double fs : { 44100.0, 48000.0, 96000.0 })
        {
            mfm1::EngineRunner r;
            r.prepare (fs, 512);
            r.postPatch (f0);
            std::vector<float> l (1024), rr (1024);
            r.process (l.data(), rr.data(), 512, nullptr, 0);   // first engine block takes the patch
            mfm1::MidiEvent evs[3] = { { 10, { 0x90, 60, 100 }, 3 }, { 200, { 0x90, 64, 100 }, 3 }, { 300, { 0xB0, 1, 64 }, 3 } };
            uint8_t on[3] = { 0x90, 67, 90 };
            allocs = 0;
            countAllocs = true;
            for (int k = 0; k < 400; k++)
            {
                if (k == 50) r.postPatch (f0);          // patch hand-over
                if (k == 60) r.postMidi (on, 3);        // a panel note
                if (k == 70) r.setTempo (97.0);
                r.process (l.data(), rr.data(), (k % 7) * 61 + 17, k == 3 ? evs : nullptr, k == 3 ? 3 : 0);
            }
            countAllocs = false;
            check (allocs.load() == 0, juce::String ("no heap allocation in process(), postPatch, postMidi at ") + juce::String (fs / 1000.0, 1) + " kHz (" + juce::String ((int) allocs.load()) + " allocations)");
            if (fs != 44100.0)
            {
                // response of the continuous kernel the phase table samples (all phases interleaved =
                // the impulse response at 512x the engine rate): pass band, and the rejection of the
                // images of 0-20 kHz content that upsampling leaves above 24.1 kHz
                const auto& t = r.kernel();
                const int taps = r.kernelTaps(), H = r.kernelHalfWidth(), P = mfm1::EngineRunner::kernelPhases();
                auto resp = [&] (double f)
                {
                    std::complex<double> s = 0;
                    for (int ph = 0; ph < P; ph++)
                        for (int k = 0; k < taps; k++)
                        {
                            const double tt = (double) (k - H + 1) - (double) ph / P;
                            s += (double) t[(size_t) ph * (size_t) taps + (size_t) k] * std::polar (1.0, -2.0 * juce::MathConstants<double>::pi * f / 44100.0 * tt);
                        }
                    return db (std::abs (s) / P);
                };
                double passWorst = 0, stopWorst = -300;
                for (double f : { 100.0, 1000.0, 5000.0, 10000.0, 15000.0, 18000.0 }) passWorst = std::min (passWorst, resp (f));
                for (double f = 24100.0; f <= 44100.0; f += 400.0) stopWorst = std::max (stopWorst, resp (f));
                info ("resampler " + juce::String (fs / 1000.0, 1) + " kHz: " + juce::String (taps) + " taps, at 20 kHz " + juce::String (resp (20000.0), 2)
                      + " dB, 0-18 kHz worst " + juce::String (passWorst, 3) + " dB, images 24.1-44.1 kHz <= " + juce::String (stopWorst, 1) + " dB");
                check (passWorst > -0.1, "resampler pass band 0-18 kHz within 0.1 dB");
                check (stopWorst < -70.0, "resampler image rejection >= 70 dB from 24.1 kHz");
            }
            info ("latency at " + juce::String (fs / 1000.0, 1) + " kHz: " + juce::String (r.latencySamples()) + " samples = "
                  + juce::String (r.latencySamples() * 1000.0 / fs, 2) + " ms");
        }
    }

    // ------------------------------------------------------------------------- 2. load the VST3 ---
    std::printf ("[vst3]\n");
    juce::AudioPluginFormatManager fm;
    juce::addDefaultFormatsToManager (fm);
    juce::VST3PluginFormat* vst3 = nullptr;
    for (auto* f : fm.getFormats()) if (auto* v = dynamic_cast<juce::VST3PluginFormat*> (f)) vst3 = v;
    check (vst3 != nullptr, "VST3 hosting available");
    if (vst3 == nullptr) return 1;
    juce::OwnedArray<juce::PluginDescription> types;
    vst3->findAllTypesForFile (types, path);
    check (types.size() == 1, "one plugin in the bundle (" + juce::String (types.size()) + ")");
    if (types.isEmpty()) return 1;
    const auto desc = *types[0];
    info (desc.name + " by " + desc.manufacturerName + ", " + desc.category + ", version " + desc.version + ", id " + desc.createIdentifierString());
    check (desc.name == "Atomic Lounge" && desc.manufacturerName == "Atomic Lounge", "name Atomic Lounge, company Atomic Lounge");
    check (desc.version == "1.2.1", "version 1.2.1 (" + desc.version + ")");
    check (desc.isInstrument, "registered as an instrument");
    check (desc.numInputChannels == 2 && desc.numOutputChannels == 2, "stereo sidechain in, stereo out (" + juce::String (desc.numInputChannels) + " in / " + juce::String (desc.numOutputChannels) + " out)");

    auto make = [&]
    {
        juce::String err;
        auto p = fm.createPluginInstance (desc, 44100.0, 512, err);
        if (p == nullptr) info ("create failed: " + err);
        return p;
    };
    auto plug = make();
    check (plug != nullptr, "instantiate");
    if (plug == nullptr) return 1;
    check (plug->acceptsMidi() && ! plug->producesMidi(), "MIDI in, no MIDI out");
    const int nChonk = mfm1_factory::count, nOrbit = orbit::factoryCount();
    check (plug->getNumPrograms() == nChonk + nOrbit, juce::String (plug->getNumPrograms()) + " host programs (16 CHONK + " + juce::String (nOrbit) + " ORBIT)");
    check (plug->getProgramName (0) == "Walnut Bass" && plug->getProgramName (9) == "Parlour Keys", "program names from the factory bank");
    {
        juce::StringArray on;
        int named = 0;
        for (int i = 0; i < nOrbit; i++)
        {
            on.add (plug->getProgramName (nChonk + i));
            if (plug->getProgramName (nChonk + i) == "ORBIT - " + orbit::factoryName (i)) named++;
        }
        check (named == nOrbit && nOrbit == 11, "ORBIT programs after the REGOLITH ones (17-24 as in 1.1.0, 25-27 new): " + on.joinIntoString (", "));
        static const char* v110[8] = { "RUSTED SATELLITE", "BOWED HALO", "BREATHING TAPE CHOIR", "SPRING RAIN", "WOOD BOX MARIMBA",
                                       "LIVE CROSS DRONE", "MOTION CHOP", "THREE-BODY ORBIT" };
        int same = 0;
        for (int i = 0; i < 8; i++) same += plug->getProgramName (nChonk + i) == juce::String ("ORBIT - ") + v110[i] ? 1 : 0;
        check (same == 8 && plug->getProgramName (24).startsWith ("ORBIT - INPUT"), "programs 17-24 keep their 1.1.0 names and numbers; 25 = " + plug->getProgramName (24));
    }
    int named = 0;
    for (int i = 0; i < mfm1::numParams; i++)
        if (auto* prm = paramById (*plug, mfm1::paramDefs[i].id); prm != nullptr && prm->getName (64) == mfm1::paramDefs[i].name) named++;
    check (named == mfm1::numParams, juce::String (named) + " of " + juce::String (mfm1::numParams) + " host parameters found by id and name");
    if (auto* prm = paramById (*plug, "f1_cutoff"))
        info ("f1_cutoff shows " + prm->getCurrentValueAsText() + ", master " + paramById (*plug, "master")->getCurrentValueAsText()
              + ", glide " + paramById (*plug, "glide")->getCurrentValueAsText());
    {
        auto layout = plug->getBusesLayout();
        layout.getChannelSet (false, 0) = juce::AudioChannelSet::mono();
        info (juce::String ("mono output layout ") + (plug->checkBusesLayoutSupported (layout) ? "supported" : "not offered by the host wrapper"));
    }

    // -------------------------------------------- 3. render at 3 rates x 5 block sizes ---
    std::printf ("[render: 3 s of chords, bend, mod wheel, aftertouch, sustain]\n");
    const auto evs = script();
    const double seconds = 3.0;
    for (double fs : { 44100.0, 48000.0, 96000.0 })
    {
        juce::AudioBuffer<float> refOut;
        for (int bs : { 1024, 32, 64, 441, 512 })
        {
            const auto t0 = std::chrono::steady_clock::now();
            auto out = renderPlugin (*plug, fs, bs, seconds, evs);
            const double secs = std::chrono::duration<double> (std::chrono::steady_clock::now() - t0).count();
            const auto s = stats (out);
            const juce::String tag = juce::String (fs / 1000.0, 1) + " kHz / " + juce::String (bs);
            check (s.finite && s.rms > 0.003 && s.peak < 2.0, tag + ": finite, sounding, no overload (rms " + juce::String (db (s.rms), 1)
                                                         + " dBFS, peak " + juce::String (db (s.peak), 1) + " dBFS, " + juce::String (seconds / secs, 0) + "x real time)");
            if (bs == 1024)
            {
                refOut = out;
                const int want = (int) std::lround ((64 + (fs == 44100.0 ? 0 : 32)) * fs / 44100.0);
                check (plug->getLatencySamples() == want, tag + ": latency reported to the host " + juce::String (plug->getLatencySamples()) + " samples ("
                                                            + juce::String (plug->getLatencySamples() * 1000.0 / fs, 2) + " ms)");
            }
            else
            {
                const double d = maxDiff (out, refOut);
                check (d < 1e-6, tag + ": identical to block size 1024 (max diff " + juce::String (d, 9) + ")");
            }
        }
        if (fs == 44100.0)
        {
            const int frames = (int) std::lround (seconds * fs);
            auto ref = renderReference (f0, frames, evs);
            double d = 0;
            for (int c = 0; c < 2; c++)
                for (int i = 64; i < frames; i++) d = std::max (d, (double) std::fabs (refOut.getSample (c, i) - ref.getSample (c, i - 64)));
            check (d < 1e-7, "44.1 kHz: plugin == offline engine render, 64 samples later (max diff " + juce::String (d, 9) + ")");
            juce::WavAudioFormat wav;
            auto f = outDir.getChildFile ("render_44k1.wav");
            f.deleteFile();
            std::unique_ptr<juce::OutputStream> os (f.createOutputStream().release());
            if (os != nullptr)
                if (std::unique_ptr<juce::AudioFormatWriter> w (wav.createWriterFor (os.get(), 44100.0, 2, 24, {}, 0)); w != nullptr)
                {
                    os.release();
                    w->writeFromAudioSampleBuffer (refOut, 0, refOut.getNumSamples());
                }
        }
        else
        {
            // resampled output vs the offline engine render resampled exactly (double precision, the same
            // windowed sinc computed per sample instead of from the phase table)
            const int frames = (int) std::lround (seconds * 44100.0) + 256;
            auto ref = renderReference (f0, frames, evs);
            const int H = 32;
            const double c = 0.92, beta = 9.0;
            auto i0 = [] (double x) { double s = 1, t = 1, q = x * x / 4; for (int k = 1; k < 64; k++) { t *= q / ((double) k * k); s += t; } return s; };
            const double i0b = i0 (beta);
            double sig = 0, err = 0;
            const int L = 64 + H;
            for (int m = (int) fs / 10; m < refOut.getNumSamples() - 64; m += 3)
            {
                const double x = (double) m * 44100.0 / fs - L;
                const int64_t idx = (int64_t) std::floor (x);
                const double frac = x - (double) idx;
                for (int ch = 0; ch < 2; ch++)
                {
                    double y = 0, wsum = 0;
                    for (int k = 0; k < 2 * H; k++)
                    {
                        const double t = (double) (k - H + 1) - frac, u = t / H;
                        const double w = std::fabs (u) < 1 ? i0 (beta * std::sqrt (1 - u * u)) / i0b : 0;
                        const double a = juce::MathConstants<double>::pi * c * t;
                        const double h = c * (std::fabs (a) < 1e-9 ? 1.0 : std::sin (a) / a) * w;
                        const int64_t j = idx - H + 1 + k;
                        y += h * (j >= 0 && j < frames ? ref.getSample (ch, (int) j) : 0.0);
                        wsum += h;
                    }
                    y /= wsum;
                    const double e = refOut.getSample (ch, m) - y;
                    sig += y * y;
                    err += e * e;
                }
            }
            const double snr = 10.0 * std::log10 (sig / std::max (err, 1e-30));
            check (snr > 90.0, juce::String (fs / 1000.0, 1) + " kHz: plugin vs exact offline resample of the offline engine render: " + juce::String (snr, 1) + " dB SNR");
        }
    }

    // --------------------------------------------------------------- 4. state, programs, ... ---
    std::printf ("[state, programs, automation, tempo]\n");
    {
        auto a = make();
        a->setCurrentProgram (5);
        paramById (*a, "f1_cutoff")->setValueNotifyingHost (0.31f);
        paramById (*a, "macro3")->setValueNotifyingHost (0.77f);
        juce::MemoryBlock mb;
        a->getStateInformation (mb);
        auto b = make();
        b->setStateInformation (mb.getData(), (int) mb.getSize());
        check (b->getCurrentProgram() == 5, "restored program number (" + juce::String (b->getCurrentProgram()) + ")");
        const float ca = paramById (*a, "f1_cutoff")->getValue(), cb = paramById (*b, "f1_cutoff")->getValue();
        const float ma = paramById (*a, "macro3")->getValue(), mbv = paramById (*b, "macro3")->getValue();
        check (std::fabs (ca - cb) < 1e-4f && std::fabs (ma - mbv) < 1e-4f, "restored parameter values (cutoff " + juce::String (cb, 4) + ", macro 3 " + juce::String (mbv, 4) + ")");
        auto oa = renderPlugin (*a, 48000.0, 512, 2.5, evs), ob = renderPlugin (*b, 48000.0, 512, 2.5, evs);
        check (stats (oa).rms > 0.003 && maxDiff (oa, ob) < 1e-6, "restored instance sounds identical (max diff " + juce::String (maxDiff (oa, ob), 9) + ")");
        juce::MemoryBlock mb2;
        b->getStateInformation (mb2);
        check (mb2 == mb, "state saved again is byte-identical (" + juce::String ((int) mb.getSize()) + " bytes)");
        info ("ui state: " + stateUi (*b));
    }
    {
        auto p = make();
        int sounding = 0;
        juce::StringArray quiet;
        for (int i = 0; i < mfm1_factory::count; i++)
        {
            p->setCurrentProgram (i);
            const std::vector<Ev> one = { { 0.02, { 0x90, 57, 110 } }, { 0.7, { 0x80, 57, 0 } } };
            const auto s = stats (renderPlugin (*p, 48000.0, 256, 1.2, one));
            if (s.finite && s.rms > 0.0005 && s.peak < 2.0) sounding++;
            else quiet.add (p->getProgramName (i) + " " + juce::String (db (s.rms), 1) + " dBFS");
        }
        check (sounding == mfm1_factory::count, juce::String (sounding) + " of 16 CHONK programs play (finite, sounding, no overload) " + quiet.joinIntoString (", "));
    }
    {
        auto p = make();
        const std::vector<Ev> chord = { { 0.02, { 0x90, 45, 110 } }, { 0.02, { 0x90, 57, 110 } }, { 1.9, { 0x80, 45, 0 } }, { 1.9, { 0x80, 57, 0 } } };
        auto* cut = paramById (*p, "f1_cutoff");
        cut->setValueNotifyingHost (0.25f);
        auto lo = renderPlugin (*p, 48000.0, 512, 1.5, chord);
        cut->setValueNotifyingHost (0.85f);
        auto hi = renderPlugin (*p, 48000.0, 512, 1.5, chord);
        const double bl = brightness (lo, 4800, 60000), bh = brightness (hi, 4800, 60000);
        check (bh > 2.0 * bl, "f1_cutoff automation changes the sound (brightness " + juce::String (bl, 4) + " -> " + juce::String (bh, 4) + ")");
        auto moving = renderPlugin (*p, 48000.0, 480, 2.0, chord,
                                    [cut] (int64_t pos) { cut->setValueNotifyingHost (juce::jlimit (0.0f, 1.0f, 0.2f + 0.7f * (float) pos / 96000.0f)); });
        const double b1 = brightness (moving, 4800, 30000), b2 = brightness (moving, 66000, 90000);
        check (stats (moving).finite && b2 > 1.5 * b1, "a cutoff sweep automated during the chord opens the filter (" + juce::String (b1, 4) + " -> " + juce::String (b2, 4) + ")");
        auto* vol = paramById (*p, "master");
        vol->setValueNotifyingHost (0.25f);
        const double q = stats (renderPlugin (*p, 48000.0, 512, 1.5, chord)).rms;
        vol->setValueNotifyingHost (0.5f);
        const double l = stats (renderPlugin (*p, 48000.0, 512, 1.5, chord)).rms;
        check (l > 3.0 * q, "master volume automation (" + juce::String (db (q), 1) + " -> " + juce::String (db (l), 1) + " dBFS)");
        auto* m1 = paramById (*p, "macro1");
        check (m1 != nullptr && m1->getCurrentValueAsText().endsWith ("%"), "macro 1 shows " + (m1 ? m1->getCurrentValueAsText() : juce::String()));
    }
    {
        // host tempo -> a BBD echo synced to 1/2 beat; short dry note: the echo lands at 0.05 + 30/BPM s
        mfe_patch_t t = f0;
        mfe_fx_defaults (&t.fx);                    // only the echo
        t.fx.bbd_mix = 30000;
        t.fx.bbd_feedback = 0;
        t.fx.fx_sync = (uint8_t) ((5 + 1) << 4);    // mfe_sync_beats(5) = 1/2 beat
        t.env[3].release = 2000;
        t.env[3].sust_time = 0;
        uint8_t img[1024];
        const size_t n = mfe_preset_write (&t, img, sizeof img);
        juce::XmlElement x ("VINTAGECHONK");
        x.setAttribute ("version", 1);
        x.setAttribute ("mfp", juce::Base64::toBase64 (img, n));
        x.setAttribute ("ui", "{}");
        x.setAttribute ("program", 0);
        juce::MemoryBlock mb;
        juce::AudioProcessor::copyXmlToBinary (x, mb);
        double found[2] = { 0, 0 };
        const double bpms[2] = { 60.0, 150.0 };
        for (int k = 0; k < 2; k++)
        {
            auto p = make();
            setPluginState (*p, mb);
            TempoHead head;
            head.bpm = bpms[k];
            p->setPlayHead (&head);
            const std::vector<Ev> blip = { { 0.05, { 0x90, 60, 120 } }, { 0.09, { 0x80, 60, 0 } } };
            auto out = renderPlugin (*p, 48000.0, 512, 1.2, blip);
            p->setPlayHead (nullptr);
            juce::String env;
            for (int w = 0; w < (int) (1.1 * 48000); w += 2400) env << juce::String (db (stats (out, w, w + 2400).rms), 0) << " ";
            info (juce::String (bpms[k], 0) + " BPM, dBFS per 50 ms: " + env);
            double best = 0;
            for (int w = (int) (0.17 * 48000); w < (int) (1.1 * 48000); w += 240)
            {
                const double e = stats (out, w, w + 480).rms;
                if (e > best) { best = e; found[k] = (w + 240) / 48000.0; }
            }
        }
        // the loudest 10 ms of the echo sits a little after its start (attack); the spacing is exact
        check (std::fabs ((found[0] - found[1]) - 0.30) < 0.02 && std::fabs (found[0] - 0.55) < 0.06 && std::fabs (found[1] - 0.25) < 0.06,
               "host tempo reaches the synced echo: 60 BPM echo at " + juce::String (found[0], 3) + " s (0.55 + attack), 150 BPM at "
                   + juce::String (found[1], 3) + " s (0.25 + attack)");
    }

    // --------------------------------------- 4b. the parameter list a DAW shows; MIDI via the VST3 path ---
    std::printf ("[vst3 parameter list, MIDI controllers]\n");
    {
        const auto pl = vst3ParamList (*plug, juce::File (path));
        info ("edit controller: " + juce::String (pl.total) + " parameters, " + juce::String (pl.visible) + " without kIsHidden ("
              + pl.visibleNames.joinIntoString (", ") + "), " + juce::String (pl.hiddenCc) + " hidden MIDI CC parameters");
        int ours = 0, orbitOurs = 0;
        juce::StringArray extra;
        for (const auto& n : pl.visibleNames)
        {
            bool mine = false, orb = false;
            for (const auto& d : mfm1::paramDefs) mine = mine || n == juce::String (d.name);
            for (const auto& h : orbit::hostParams()) orb = orb || n == h.name;
            if (mine) ours++;
            else if (orb) orbitOurs++;
            else extra.add (n);
        }
        const int nOrbit = (int) orbit::hostParams().size();
        check (ours == mfm1::numParams && orbitOurs == nOrbit && pl.visible <= mfm1::numParams + nOrbit + 2,
               juce::String (pl.visible) + " visible VST3 parameters: CHONK " + juce::String (ours) + " + ORBIT " + juce::String (orbitOurs)
                   + " + [" + extra.joinIntoString (", ") + "]");
        check (pl.hiddenCc == 16 * 130, juce::String (pl.hiddenCc) + " MIDI CC emulation parameters, all flagged kIsHidden (16 x 130)");
        info ("JUCE's hosted view of the plugin lists " + juce::String (plug->getParameters().size()) + " parameters (host side, flags not applied)");
    }
    {
        // one held note through a patch with mod wheel -> F1 cutoff; amp envelope with a short release
        mfe_patch_t t = f0;
        t.mod[MFE_NMOD - 1].src = (uint8_t) MFE_S_MODWHEEL;
        t.mod[MFE_NMOD - 1].dst = (uint8_t) MFE_D_F1_CUTOFF;
        t.mod[MFE_NMOD - 1].amount = 24000;
        t.env[3].release = 2000;
        uint8_t img[1024];
        const size_t n = mfe_preset_write (&t, img, sizeof img);
        juce::XmlElement x ("VINTAGECHONK");
        x.setAttribute ("version", 1);
        x.setAttribute ("mfp", juce::Base64::toBase64 (img, n));
        x.setAttribute ("ui", "{}");
        x.setAttribute ("program", 0);
        juce::MemoryBlock mb;
        juce::AudioProcessor::copyXmlToBinary (x, mb);
        auto p = make();
        setPluginState (*p, mb);
        const double fs = 48000.0;
        auto run = [&] (std::vector<Ev> e) { return renderPlugin (*p, fs, 512, 1.6, e); };
        const auto plain = run ({ { 0.02, { 0x90, 45, 110 } }, { 0.9, { 0x80, 45, 0 } } });
        const auto wheel = run ({ { 0.01, { 0xB0, 1, 127 } }, { 0.02, { 0x90, 45, 110 } }, { 0.9, { 0x80, 45, 0 } }, { 1.5, { 0xB0, 1, 0 } } });
        const double b0 = brightness (plain, 9600, 40000), b1 = brightness (wheel, 9600, 40000);
        check (b1 > 1.3 * b0, "mod wheel (CC 1) reaches the engine through the VST3 path: brightness " + juce::String (b0, 4) + " -> " + juce::String (b1, 4));
        const auto bent = run ({ { 0.01, { 0xE0, 0x7F, 0x7F } }, { 0.02, { 0x90, 45, 110 } }, { 0.9, { 0x80, 45, 0 } }, { 1.5, { 0xE0, 0x00, 0x40 } } });
        const int z0 = zeroCrossings (plain, 9600, 40000), z1 = zeroCrossings (bent, 9600, 40000);
        check (z1 > z0 + z0 / 20, "pitch bend reaches the engine through the VST3 path: zero crossings " + juce::String (z0) + " -> " + juce::String (z1));
        const auto held = run ({ { 0.01, { 0xB0, 64, 127 } }, { 0.02, { 0x90, 45, 110 } }, { 0.4, { 0x80, 45, 0 } }, { 1.5, { 0xB0, 64, 0 } } });
        const auto dry = run ({ { 0.02, { 0x90, 45, 110 } }, { 0.4, { 0x80, 45, 0 } } });
        const double sh = stats (held, 48000, 62400).rms, sd = stats (dry, 48000, 62400).rms;
        check (sh > 0.003 && sh > 10.0 * sd, "sustain (CC 64) holds the note after its note-off through the VST3 path: "
                                             + juce::String (db (sd), 1) + " -> " + juce::String (db (sh), 1) + " dBFS at 1.0-1.3 s");
    }
    {
        // user presets: the pre-rename folder is copied once into the new one, never overwriting
        auto tmp = juce::File::getSpecialLocation (juce::File::tempDirectory).getChildFile ("vc_preset_migration_test");
        tmp.deleteRecursively();
        auto from = tmp.getChildFile ("old"), to = tmp.getChildFile ("new");
        from.createDirectory();
        to.createDirectory();
        from.getChildFile ("A.mfp").replaceWithText ("old a");
        from.getChildFile ("B.mfp").replaceWithText ("old b");
        to.getChildFile ("B.mfp").replaceWithText ("new b");
        const int c1 = mfm1::PresetStore::migrate (from, to);
        to.getChildFile ("A.mfp").deleteFile();
        const int c2 = mfm1::PresetStore::migrate (from, to);
        check (c1 == 1 && to.getChildFile ("B.mfp").loadFileAsString() == "new b" && c2 == 0 && ! to.getChildFile ("A.mfp").exists(),
               "old preset folder migrated once (copied " + juce::String (c1) + ", existing kept, deleted stays deleted)");
        tmp.deleteRecursively();
    }

    {
        // a project saved by release 1.0.0 (a real state from the 1.0.0 bundle: program 7, macro 2 at 0.73, no mode,
        // no ORBIT child) loads as CHONK, unchanged: also into an instance that is in ORBIT mode
        const auto repoDir = juce::File (path).getParentDirectory().getParentDirectory().getParentDirectory();
        const auto fx = repoDir.getChildFile ("plugin/tests/fixtures/state_1.0.0_program7_macro2.bin");
        juce::MemoryBlock v100;
        const bool have = fx.loadFileAsData (v100);
        check (have && v100.getSize() > 500, "1.0.0 state fixture " + fx.getFileName() + " (" + juce::String ((int) v100.getSize()) + " bytes)");
        if (have)
        {
            auto a = make();
            a->setStateInformation (v100.getData(), (int) v100.getSize());
            auto b = make();
            b->setCurrentProgram (mfm1_factory::count + 2);
            auto xb0 = pluginState (*b);
            const bool bWasOrbit = xb0 != nullptr && xb0->getStringAttribute ("mode") == "orbit";
            b->setStateInformation (v100.getData(), (int) v100.getSize());
            auto xa = pluginState (*a), xb = pluginState (*b);
            auto* m2 = paramById (*a, "macro2");
            check (xa != nullptr && xa->getStringAttribute ("mode") == "chonk" && a->getCurrentProgram() == 7 && m2 != nullptr
                       && std::fabs (m2->getValue() - 0.73f) < 0.01f,
                   "the 1.0.0 state loads as CHONK, program " + juce::String (a->getCurrentProgram()) + " (" + a->getProgramName (a->getCurrentProgram())
                       + "), macro 2 " + juce::String (m2 ? m2->getValue() : -1.0f, 3));
            check (bWasOrbit && xb != nullptr && xb->getStringAttribute ("mode") == "chonk" && b->getCurrentProgram() == 7,
                   "the 1.0.0 state loaded into an instance playing an ORBIT program switches it to CHONK, program " + juce::String (b->getCurrentProgram()));
            check (xa != nullptr && xb != nullptr && xa->getStringAttribute ("mfp") == xb->getStringAttribute ("mfp")
                       && xa->getStringAttribute ("ui") == xb->getStringAttribute ("ui"),
                   "same CHONK sound and panel entry either way: " + (xa ? xa->getStringAttribute ("ui") : juce::String()));
            auto c = make();
            c->setCurrentProgram (7);
            if (auto* cm2 = paramById (*c, "macro2")) cm2->setValueNotifyingHost (m2 != nullptr ? m2->getValue() : 0.73f);
            const auto ra = renderPlugin (*a, 48000.0, 256, 1.5, evs), rb = renderPlugin (*b, 48000.0, 256, 1.5, evs),
                       rc = renderPlugin (*c, 48000.0, 256, 1.5, evs);
            check (maxDiff (ra, rb) < 1e-6 && maxDiff (ra, rc) < 1e-6 && stats (ra).rms > 0.001,
                   "the 1.0.0 project sounds the same in a fresh instance, after ORBIT, and as program 7 + macro 2 chosen by hand (max diff "
                       + juce::String (std::max (maxDiff (ra, rb), maxDiff (ra, rc)), 9) + ", rms " + juce::String (db (stats (ra).rms), 1) + " dBFS)");
        }
    }
    {
        // the ORBIT host programs: selecting one switches the mode and loads the preset; a CHONK program switches back
        auto p = make();
        int good = 0;
        juce::StringArray bad;
        for (int i = 0; i < orbit::factoryCount(); i++)
        {
            p->setCurrentProgram (mfm1_factory::count + i);
            auto x = pluginState (*p);
            auto* oe = x != nullptr ? x->getChildByName ("ORBIT") : nullptr;
            const auto o = i >= 8 && i <= 9 ? renderIn (*p, 48000.0, 256, 2.0, chordScore ({ { 48, 55, 60, 64 } }, 1.4, 0.0), demoInput, 2)
                                            : renderPlugin (*p, 48000.0, 256, 2.0, chordScore ({ { 48, 55, 60, 64 } }, 1.4, 0.0));
            const auto so = stats (o);
            const bool ok = x != nullptr && x->getStringAttribute ("mode") == "orbit" && p->getCurrentProgram() == mfm1_factory::count + i
                            && x->getIntAttribute ("orbitProgram", -1) == i && oe != nullptr && oe->toString().contains (orbit::factoryName (i))
                            && so.finite && so.rms > 0.001 && so.peak <= 0.8913;    // at or below -1 dBFS
            if (ok) good++;
            else bad.add (p->getProgramName (mfm1_factory::count + i) + " (rms " + juce::String (db (so.rms), 1) + ")");
        }
        check (good == orbit::factoryCount(), juce::String (good) + " of " + juce::String (orbit::factoryCount())
                                                  + " ORBIT programs switch to ORBIT, load the preset, report the program and play " + bad.joinIntoString (", "));
        // the program survives the state
        juce::MemoryBlock mb;
        p->getStateInformation (mb);
        auto q = make();
        q->setStateInformation (mb.getData(), (int) mb.getSize());
        check (q->getCurrentProgram() == p->getCurrentProgram(), "an ORBIT program comes back from the state (" + q->getProgramName (q->getCurrentProgram()) + ")");
        p->setCurrentProgram (4);
        auto x = pluginState (*p);
        check (x != nullptr && x->getStringAttribute ("mode") == "chonk" && p->getCurrentProgram() == 4 && x->getIntAttribute ("program") == 4,
               "a CHONK program after an ORBIT one switches back to CHONK (" + p->getProgramName (p->getCurrentProgram()) + ")");
    }
    {
        // a DAW project saved by a pre-release build (its state tag has the old name) still loads
        uint8_t img[1024];
        const size_t n = mfe_preset_write (&f0, img, sizeof img);
        juce::XmlElement x ("MOOSIVEFM1");   // legacy name: the pre-release state tag
        x.setAttribute ("version", 1);
        x.setAttribute ("mfp", juce::Base64::toBase64 (img, n));
        x.setAttribute ("ui", "{}");
        x.setAttribute ("program", 7);
        juce::MemoryBlock mb;
        juce::AudioProcessor::copyXmlToBinary (x, mb);
        auto p = make();
        setPluginState (*p, mb);
        check (p->getCurrentProgram() == 7, "a pre-release project state still loads (program " + juce::String (p->getCurrentProgram()) + ")");
    }

    // ----------------------------------------------------------------------------- 6. ORBIT mode ---
    std::printf ("[ORBIT: effects wrapper]\n");
    for (double fs : { 44100.0, 48000.0, 96000.0 })
    {
        mfm1::FxChain fx;
        fx.prepare (fs);
        mfe_fx_t off;
        mfe_fx_defaults (&off);
        fx.setParams (off);
        // a band-limited test signal (Hann bursts at 1 and 9 kHz): the output must be the input delayed by L
        const int n = (int) fs;
        std::vector<float> l ((size_t) n, 0.0f), r ((size_t) n, 0.0f), in ((size_t) n, 0.0f);
        for (int i = 0; i < 4800; i++)
        {
            const double w = 0.5 - 0.5 * std::cos (2.0 * juce::MathConstants<double>::pi * i / 4800.0), t = i / fs;
            in[(size_t) (1000 + i)] = (float) (w * (0.5 * std::sin (2.0 * juce::MathConstants<double>::pi * 1000.0 * t) + 0.3 * std::sin (2.0 * juce::MathConstants<double>::pi * 9000.0 * t)));
        }
        l = r = in;
        for (int pos = 0; pos < n; pos += 333) fx.process (l.data() + pos, r.data() + pos, std::min (333, n - pos));
        const int L = fx.latencySamples();
        double sig = 0, err = 0;
        for (int i = 0; i + L < n; i++) { sig += (double) in[(size_t) i] * in[(size_t) i]; err += std::pow ((double) l[(size_t) (i + L)] - in[(size_t) i], 2.0); }
        const double snr = 10.0 * std::log10 (sig / std::max (err, 1e-30));
        check (snr > 60.0, juce::String (fs / 1000.0, 1) + " kHz: every effect off = the input delayed by exactly the reported " + juce::String (L)
                               + " samples (" + juce::String (L * 1000.0 / fs, 2) + " ms; " + juce::String (snr, 1) + " dB SNR against the delayed input)");
        // all six on: finite, it rings (reverb / echo tail), and it sounds at all rates
        mfe_fx_t on = off;
        on.fuzz_amount = 6000; on.tape_drive = 12000; on.ens_mode = MFE_ENS_I; on.ens_mix = 16000; on.bbd_mix = 12000; on.oil_mix = 10000; on.rev_amount = 14000;
        mfm1::FxChain fx2;
        fx2.prepare (fs);
        fx2.setParams (on);
        std::vector<float> a ((size_t) n), b ((size_t) n);
        for (int i = 0; i < n; i++) a[(size_t) i] = b[(size_t) i] = i < n / 4 ? 0.3f * std::sin (2.0f * 3.14159f * 220.0f * (float) i / (float) fs) : 0.0f;
        for (int pos = 0; pos < n; pos += 256) fx2.process (a.data() + pos, b.data() + pos, std::min (256, n - pos));
        double tail = 0;
        bool fin = true;
        for (int i = 0; i < n; i++) { fin = fin && std::isfinite (a[(size_t) i]); if (i > n / 2) tail += (double) a[(size_t) i] * a[(size_t) i]; }
        check (fin && tail > 1e-4, juce::String (fs / 1000.0, 1) + " kHz: the whole chain on (OCT FUZZ, TAPE, ENSEMBLE, BBD, OIL CAN, TWIN TANK): finite, tail "
                                        + juce::String (db (std::sqrt (tail / (n / 2))), 1) + " dBFS");
    }

    std::printf ("[ORBIT: engine host]\n");
    const auto threeChords = [] { auto e = chordScore ({ { 50, 57, 62, 66, 69 }, { 46, 53, 62, 65, 70 }, { 48, 55, 60, 64, 67 }, { 45, 52, 60, 64, 69 } }, 3.0, 0.2); sortScore (e); return e; }();
    {
        orbit::Runner r;
        r.prepare (48000.0, 256);
        orbitApply (r, orbit::factoryPreset (0));
        r.createEngine();
        r.waitIdle (20000);
        const int core = mfo_latency_samples (r.engineForTests());
        mfm1::FxChain ref;
        ref.prepare (48000.0);
        check (core == 256 && r.latencySamples() == core + ref.latencySamples(),
               "ORBIT latency at 48 kHz = core STFT " + juce::String (core) + " + effects " + juce::String (ref.latencySamples()) + " = "
                   + juce::String (r.latencySamples()) + " samples (" + juce::String (r.latencySamples() / 48.0, 2) + " ms)");
        // no allocation on the audio thread: renders, notes split mid-block, a panel note, a new impulse arriving
        std::vector<float> l (2048), rr (2048);
        mfm1::MidiEvent mev[4] = { { 10, { 0x90, 48, 100 }, 3 }, { 77, { 0x90, 55, 100 }, 3 }, { 200, { 0xB0, 1, 90 }, 3 }, { 250, { 0xE0, 0, 80 }, 3 } };
        uint8_t on[3] = { 0x90, 67, 90 };
        orbit::ImpulseSpec spec = orbit::factoryPreset (0).imp[0];
        spec.genType = MFO_GEN_GLASS;
        allocs = 0;
        countAllocs = true;
        bool fin = true;
        for (int k = 0; k < 600; k++)
        {
            if (k == 40) r.postMidi (on, 3);
            if (k == 60) { countAllocs = false; r.requestImpulse (0, spec); countAllocs = true; }   // the request itself is the message thread's
            const int n = (k % 5) * 97 + 31;
            r.process (l.data(), rr.data(), n, k == 3 ? mev : nullptr, k == 3 ? 4 : 0);
            for (int i = 0; i < n; i++) fin = fin && std::isfinite (l[(size_t) i]) && std::isfinite (rr[(size_t) i]);
            if (k == 100) { countAllocs = false; r.waitIdle (5000); countAllocs = true; }
        }
        countAllocs = false;
        check (allocs.load() == 0 && fin, "ORBIT audio thread: no heap allocation over 600 blocks with MIDI, a panel note and an impulse swap ("
                                              + juce::String ((int) allocs.load()) + " allocations), all finite");
    }
    {
        // render calls split at the MIDI offsets: block sizes on the 64-sample grid give the same output
        auto run = [&] (int bs)
        {
            orbit::Runner r;
            r.prepare (48000.0, bs);
            auto s = orbit::factoryPreset (7);
            s.core[MFO_P_DRIFT] = 0.0f;              // the random walks (drift, wobble) step once per render chunk
            s.core[MFO_P_ORBIT_WOBBLE] = 0.0f;
            s.mods = {};
            orbitApply (r, s);
            r.createEngine();
            r.waitIdle (20000);
            r.process (nullptr, nullptr, 0, nullptr, 0);   // assets in
            r.reset();
            const int total = 48000 * 3;
            juce::AudioBuffer<float> out (2, total);
            std::vector<mfm1::MidiEvent> ev;
            for (int pos = 0; pos < total; pos += bs)
            {
                const int n = std::min (bs, total - pos);
                ev.clear();
                for (const auto& e : threeChords)
                {
                    const int64_t m = std::llround (e.t * 48000.0);
                    if (m >= pos && m < pos + n) ev.push_back ({ (int) (m - pos), { e.b[0], e.b[1], e.b[2] }, 3 });
                }
                r.process (out.getWritePointer (0, pos), out.getWritePointer (1, pos), n, ev.data(), (int) ev.size());
            }
            return out;
        };
        const auto a = run (64), b = run (256), c = run (1000);
        double sig = 0, err = 0;
        for (int i = 0; i < a.getNumSamples(); i++) { sig += (double) a.getSample (0, i) * a.getSample (0, i); err += std::pow ((double) a.getSample (0, i) - c.getSample (0, i), 2.0); }
        check (stats (a).rms > 0.003 && maxDiff (a, b) < 1e-6, "ORBIT: block sizes 64 and 256 render the same (events at their exact samples; max diff " + juce::String (maxDiff (a, b), 9) + ")");
        check (10.0 * std::log10 (sig / std::max (err, 1e-20)) > 40.0, "ORBIT: block size 1000 (off the 64 grid) within " + juce::String (10.0 * std::log10 (sig / std::max (err, 1e-20)), 1) + " dB SNR");
    }
    {
        // CPU at 48 kHz / 256: the heaviest factory presets with five-note chords (best of three runs)
        for (int pi : { 7, 0, 1 })
        {
            orbit::Runner r;
            r.prepare (48000.0, 256);
            orbitApply (r, orbit::factoryPreset (pi));
            r.createEngine();
            r.waitIdle (20000);
            const int total = 48000 * 12;
            std::vector<float> l (256), rr (256);
            std::vector<mfm1::MidiEvent> ev;
            double best = 1e9;
            for (int run = 0; run < 3; run++)
            {
                r.reset();
                const auto t0 = std::chrono::steady_clock::now();
                for (int pos = 0; pos < total; pos += 256)
                {
                    ev.clear();
                    for (const auto& e : threeChords)
                    {
                        const int64_t m = std::llround (e.t * 48000.0);
                        if (m >= pos && m < pos + 256) ev.push_back ({ (int) (m - pos), { e.b[0], e.b[1], e.b[2] }, 3 });
                    }
                    r.process (l.data(), rr.data(), 256, ev.data(), (int) ev.size());
                }
                best = std::min (best, std::chrono::duration<double> (std::chrono::steady_clock::now() - t0).count() / 12.0);
            }
            check (best < 0.6, "ORBIT CPU at 48 kHz / 256, " + orbit::factoryName (pi) + ": " + juce::String (best * 100.0, 1) + " % of one core (3 layers, 5-note chords, effects on; limit 60 %)");
        }
    }
    {
        // root detection: a 220 Hz tone with harmonics and a pluck envelope -> A3 (57)
        auto d = std::make_shared<orbit::SampleData>();
        d->rate = 44100.0;
        d->channels = 1;
        d->ch[0].resize (44100);
        const double tau = 2.0 * juce::MathConstants<double>::pi;
        for (int i = 0; i < 44100; i++)
        {
            const double t = i / 44100.0, env = std::min (1.0, t * 200.0) * std::exp (-t * 1.5);
            d->ch[0][(size_t) i] = (float) (env * (0.6 * std::sin (tau * 220.0 * t) + 0.3 * std::sin (tau * 440.0 * t) + 0.15 * std::sin (tau * 660.0 * t)));
        }
        const float root = orbit::detectRoot (*d);
        check (std::fabs (root - 57.0f) < 0.15f, "root-key detection (YIN): a 220 Hz tone reads " + juce::String (root, 2) + " (A3 = 57)");
        for (int i = 0; i < 44100; i++) d->ch[0][(size_t) i] = (float) (std::sin (tau * 65.41 * i / 44100.0) * 0.5);
        const float r2 = orbit::detectRoot (*d);
        check (std::fabs (r2 - 36.0f) < 0.15f, "root-key detection: 65.4 Hz reads " + juce::String (r2, 2) + " (C2 = 36)");
    }

    {
        const auto f = outDir.getChildFile ("orbit_panel_state.json");
        check (f.replaceWithText (juce::JSON::toString (orbitPanelState(), true)), "ORBIT panel state fixture written: " + f.getFullPathName());
    }

    std::printf ("[ORBIT: through the VST3]\n");
    const int nOrbitParams = (int) orbit::hostParams().size();
    {
        int found = 0;
        for (const auto& h : orbit::hostParams()) if (paramByName (*plug, h.name) != nullptr) found++;
        check (found == nOrbitParams, juce::String (found) + " of " + juce::String (nOrbitParams) + " ORBIT host parameters found by name");
        if (auto* p = paramByName (*plug, "ORBIT L1 Cutoff"))
            info ("ORBIT L1 Cutoff shows " + p->getCurrentValueAsText() + ", ORBIT Rate " + paramByName (*plug, "ORBIT Rate")->getCurrentValueAsText()
                  + ", ORBIT Output " + paramByName (*plug, "ORBIT Output")->getCurrentValueAsText());
    }
    {
        // mode switch through the state, latency reported to the host
        auto p = make();
        const std::vector<Ev> note = { { 0.05, { 0x90, 57, 110 } }, { 1.5, { 0x80, 57, 0 } } };
        renderPlugin (*p, 48000.0, 256, 0.3, note);
        const int chonkLat = p->getLatencySamples();
        const auto s0 = orbit::factoryPreset (0);
        setPluginState (*p, orbitBlob (*p, &s0, "orbit"));
        mfm1::FxChain ref;
        ref.prepare (48000.0);
        const int want = 256 + ref.latencySamples();
        pumpUntil ([&] { return p->getLatencySamples() == want; }, 3000);
        const int orbitLat = p->getLatencySamples();
        auto x = pluginState (*p);
        check (chonkLat == 104 && orbitLat == want && x != nullptr && x->getStringAttribute ("mode") == "orbit",
               "mode CHONK -> ORBIT in the state: latency reported to the host " + juce::String (chonkLat) + " -> " + juce::String (orbitLat) + " samples (want " + juce::String (want) + "), mode saved");
        const auto o = renderPlugin (*p, 48000.0, 256, 2.0, chordScore ({ { 48, 55, 60, 64 } }, 1.6, 0.0));
        const auto so = stats (o);
        check (so.finite && so.rms > 0.003 && so.peak < 1.5, "ORBIT plays through the VST3 (rms " + juce::String (db (so.rms), 1) + " dBFS, peak " + juce::String (db (so.peak), 1) + " dBFS)");
        setPluginState (*p, orbitBlob (*p, nullptr, "chonk"));
        pumpUntil ([&] { return p->getLatencySamples() == chonkLat; }, 3000);
        check (p->getLatencySamples() == chonkLat, "back to CHONK: latency " + juce::String (p->getLatencySamples()) + " samples");
        // CHONK after a round trip through ORBIT is still the 1.0.0 engine: equal to a fresh instance
        auto fresh = make();
        const auto a = renderPlugin (*p, 48000.0, 512, 1.5, evs), b = renderPlugin (*fresh, 48000.0, 512, 1.5, evs);
        check (maxDiff (a, b) < 1e-6, "CHONK after ORBIT renders exactly as a fresh instance (max diff " + juce::String (maxDiff (a, b), 9) + ")");
    }
    {
        // switching while CHONK sounds: an equal-power cross-fade, no click
        auto p = make();
        const auto s0 = orbit::factoryPreset (1);
        setPluginState (*p, orbitBlob (*p, &s0, "chonk"));
        const auto toOrbit = orbitBlob (*p, &s0, "orbit");
        const std::vector<Ev> held = { { 0.05, { 0x90, 45, 110 } }, { 0.05, { 0x90, 57, 110 } } };
        int sw = -1;                                // switch at the first block from 1.0 s
        const auto out = renderPlugin (*p, 48000.0, 256, 1.6, held, [&] (int64_t pos) { if (pos >= 48000 && sw < 0) { sw = (int) pos; setPluginState (*p, toOrbit); } });
        const double before = maxJump (out, 24000, sw), across = maxJump (out, sw, sw + 4800);
        const double after = stats (out, sw + 4800, sw + 9600).rms;
        check (across < 1.2 * before + 1e-4 && after < 0.25 * stats (out, 24000, sw).rms,
               "CHONK -> ORBIT while a chord sounds: largest step across the switch " + juce::String (across, 4) + " vs " + juce::String (before, 4)
                   + " before (no click), the old engine fades out (" + juce::String (db (after), 1) + " dBFS after)");
    }
    {
        // state round trip with an embedded sample; the file deleted; then a missing one
        auto tmp = juce::File::getSpecialLocation (juce::File::tempDirectory).getChildFile ("vc_orbit_state_test");
        tmp.deleteRecursively();
        tmp.createDirectory();
        auto wavF = tmp.getChildFile ("Cello Swell.wav");
        {
            const double tau = 2.0 * juce::MathConstants<double>::pi;
            juce::AudioBuffer<float> b (2, 66150);
            for (int i = 0; i < 66150; i++)
            {
                const double t = i / 44100.0, env = std::min (1.0, t * 4.0) * (1.0 - t / 1.6);
                for (int c = 0; c < 2; c++)
                    b.setSample (c, i, (float) (0.4 * env * (std::sin (tau * 130.81 * t) + 0.4 * std::sin (tau * 261.63 * t + c))));
            }
            float g = 1.0f;
            writeWav (wavF, b, 44100.0, g);
        }
        juce::String err;
        auto smp = orbit::decodeFile (wavF, err);
        check (smp != nullptr && smp->flac.getSize() > 1000 && std::fabs (smp->detectedRoot - 48.0f) < 0.3f,
               "a WAV decodes (JUCE AudioFormatManager), FLAC copy " + juce::String ((int) (smp ? smp->flac.getSize() : 0)) + " bytes, root "
                   + juce::String (smp ? smp->detectedRoot : -1.0f, 2) + " (C3 = 48) " + err);
        auto s = orbit::factoryPreset (1);
        s.src[2].kind = orbit::Src::sample;
        s.src[2].from = orbit::SampleFrom::file;
        s.src[2].sample = smp;
        s.src[2].root = 48.0f;
        s.imp[2] = orbit::ImpulseSpec();
        s.imp[2].kind = orbit::Imp::sample;
        s.imp[2].from = orbit::SampleFrom::file;
        s.imp[2].sample = smp;
        s.core[(size_t) MFO_LPARAM (2, MFO_L_BODY)] = 0.6f;
        s.core[MFO_P_PUCK_X] = 0.0f; s.core[MFO_P_PUCK_Y] = -0.6f;
        s.core[MFO_P_DRIFT] = 0.0f;
        s.path = orbit::pathHold;
        auto a = make();
        setPluginState (*a, orbitBlob (*a, &s, "orbit"));
        juce::MemoryBlock mb;
        a->getStateInformation (mb);
        auto x = pluginState (*a);
        auto* oe = x != nullptr ? x->getChildByName ("ORBIT") : nullptr;
        auto* se = oe != nullptr ? oe->getChildByAttribute ("id", "S3") : nullptr;
        check (se != nullptr && se->getIntAttribute ("embedded") == 1 && se->getStringAttribute ("audio").length() > 1000 && se->getStringAttribute ("path") == wavF.getFullPathName(),
               "the sample is in the state by path and embedded (" + juce::String (se ? se->getStringAttribute ("audio").length() : 0) + " base64 characters), state "
                   + juce::String ((int) mb.getSize()) + " bytes");
        wavF.deleteFile();
        auto b = make();
        b->setStateInformation (mb.getData(), (int) mb.getSize());
        const auto sc = chordScore ({ { 48, 55, 60 } }, 1.8, 0.2);
        const auto oa = renderPlugin (*a, 48000.0, 256, 2.0, sc), ob = renderPlugin (*b, 48000.0, 256, 2.0, sc);
        const double sig = stats (oa).rms;
        double err2 = 0;
        for (int c = 0; c < 2; c++) for (int i = 0; i < oa.getNumSamples(); i++) err2 += std::pow ((double) oa.getSample (c, i) - ob.getSample (c, i), 2.0);
        const double snr = 10.0 * std::log10 ((sig * sig * oa.getNumSamples() * 2) / std::max (err2, 1e-20));
        check (sig > 0.003 && snr > 30.0, "with the file deleted, the reopened project still has the sample (embedded) and sounds the same (" + juce::String (snr, 1) + " dB SNR)");
        auto x2 = pluginState (*b);
        auto* oe2 = x2 != nullptr ? x2->getChildByName ("ORBIT") : nullptr;
        check (oe2 != nullptr && se != nullptr && oe2->getChildByAttribute ("id", "S3") != nullptr
                   && oe2->getChildByAttribute ("id", "S3")->getStringAttribute ("audio") == se->getStringAttribute ("audio")
                   && oe2->getStringAttribute ("settings") == oe->getStringAttribute ("settings"),
               "ORBIT state saved again: same settings (150 core parameters, specs, mod matrix) and the same embedded audio");
        // not embedded and the file gone: loads, the layer is marked missing, the rest plays
        if (se != nullptr)
        {
            se->removeAttribute ("audio");
            se->setAttribute ("embedded", 0);
            if (auto* ie = oe->getChildByAttribute ("id", "I3")) ie->removeAttribute ("audio");
            juce::MemoryBlock mb3;
            juce::AudioProcessor::copyXmlToBinary (*x, mb3);
            auto c = make();
            setPluginState (*c, mb3);
            const auto oc = renderPlugin (*c, 48000.0, 256, 2.0, sc);
            auto x3 = pluginState (*c);
            const auto st3 = x3 != nullptr && x3->getChildByName ("ORBIT") != nullptr ? juce::JSON::parse (x3->getChildByName ("ORBIT")->getStringAttribute ("settings")) : juce::var();
            const bool missing = (bool) st3["layers"][2]["src"]["file"]["missing"];
            check (stats (oc).finite && stats (oc).rms > 0.001 && missing, "a sample neither embedded nor found: the project loads, layer 3 is marked missing ("
                                                                             + st3["layers"][2]["src"]["file"]["path"].toString() + "), the other layers play");
        }
        tmp.deleteRecursively();
    }
    {
        // automation of ORBIT host parameters
        auto p = make();
        auto s = orbit::factoryPreset (1);
        s.path = orbit::pathHold;
        s.core[MFO_P_PUCK_X] = -0.7f; s.core[MFO_P_PUCK_Y] = 0.6f;     // on layer 1
        setPluginState (*p, orbitBlob (*p, &s, "orbit"));
        const auto sc = chordScore ({ { 45, 57, 64 } }, 1.9, 0.0);
        auto* cut = paramByName (*p, "ORBIT L1 Cutoff");
        cut->setValueNotifyingHost (0.25f);
        const auto lo = renderPlugin (*p, 48000.0, 256, 1.8, sc);
        cut->setValueNotifyingHost (0.95f);
        const auto hi = renderPlugin (*p, 48000.0, 256, 1.8, sc);
        const double bl = brightness (lo, 24000, 80000), bh = brightness (hi, 24000, 80000);
        check (bh > 1.5 * bl, "ORBIT L1 Cutoff automation changes the sound (brightness " + juce::String (bl, 4) + " -> " + juce::String (bh, 4) + ")");
        auto* outp = paramByName (*p, "ORBIT Output");
        outp->setValueNotifyingHost (0.3f);
        const double q = stats (renderPlugin (*p, 48000.0, 256, 1.8, sc), 24000, 80000).rms;
        outp->setValueNotifyingHost (0.7f);
        const double l = stats (renderPlugin (*p, 48000.0, 256, 1.8, sc), 24000, 80000).rms;
        check (l > 3.0 * q, "ORBIT Output automation (" + juce::String (db (q), 1) + " -> " + juce::String (db (l), 1) + " dBFS)");
        // the puck automated across the field during the chord: layer 1 -> the dry node changes the sound
        auto* px = paramByName (*p, "ORBIT Puck X");
        auto* py = paramByName (*p, "ORBIT Puck Y");
        px->setValueNotifyingHost (0.15f);
        py->setValueNotifyingHost (0.8f);
        const auto moving = renderPlugin (*p, 48000.0, 256, 1.8, sc, [&] (int64_t pos)
        {
            const float t = juce::jlimit (0.0f, 1.0f, (float) pos / 86400.0f);
            px->setValueNotifyingHost (0.15f + 0.7f * t);
            py->setValueNotifyingHost (0.8f - 0.65f * t);
        });
        const double m0 = brightness (moving, 20000, 30000), m1 = brightness (moving, 76000, 86000);
        check (stats (moving).finite && std::fabs (m0 - m1) > 0.1 * std::max (m0, m1),
               "ORBIT Puck X / Y automated across the field moves the blend (brightness " + juce::String (m0, 4) + " -> " + juce::String (m1, 4) + ")");
        info ("ORBIT Rate shows " + paramByName (*p, "ORBIT Rate")->getCurrentValueAsText() + "; L1 Body " + paramByName (*p, "ORBIT L1 Body")->getCurrentValueAsText());
    }
    {
        // every factory preset through the VST3 at 48 kHz / 256: finite, sounding; a WAV of each in examples/orbit/presets
        const auto repo = juce::File (path).getParentDirectory().getParentDirectory().getParentDirectory();
        const auto wavDir = repo.getChildFile ("examples").getChildFile ("orbit").getChildFile ("presets");
        int ok = 0;
        juce::StringArray bad, written;
        for (int i = 0; i < orbit::factoryCount(); i++)
        {
            auto p = make();
            const auto s = orbit::factoryPreset (i);
            TempoHead head;
            head.bpm = 112.0;
            p->setPlayHead (&head);
            setPluginState (*p, orbitBlob (*p, &s, "orbit"));
            std::vector<Ev> sc;
            switch (i)
            {
                case 0: sc = chordScore ({ { 36, 48, 55, 63 }, { 32, 44, 51, 60 }, { 34, 46, 53, 62 }, { 31, 43, 50, 58 } }, 2.4, 0.15); break;
                case 3: sc = lineScore ({ 72, 79, 84, 76, 81, 74, 79, 86, 77, 72, 84, 79, 76, 83, 74, 88, 81, 76, 72, 79 }, 0.4, 0.9); break;
                case 4: sc = lineScore ({ 60, 64, 67, 72, 71, 67, 64, 62, 65, 69, 72, 74, 72, 69, 65, 64, 60, 55, 60, 67, 72, 76, 74, 72 }, 0.25, 0.1); break;
                case 5: sc = chordScore ({ { 36, 43, 60, 67 }, { 36, 43, 58, 65 } }, 5.0, 0.3); break;
                case 6: sc = chordScore ({ { 41, 48, 53, 60 }, { 39, 46, 51, 58 } }, 4.3, 0.05); break;
                case 7: sc = chordScore ({ { 50, 57, 62, 69 }, { 46, 53, 62, 65 }, { 48, 55, 60, 67 }, { 45, 52, 60, 64 } }, 2.6, 0.2); break;
                default: sc = chordScore ({ { 50, 57, 62, 66 }, { 47, 54, 59, 62 }, { 43, 50, 59, 62 } }, 3.2, 0.25); break;
            }
            sortScore (sc);
            const double secs = sc.back().t + 3.0;
            const auto out = i >= 8 && i <= 9 ? renderIn (*p, 48000.0, 256, secs, sc, demoInput, 2)   // INPUT presets: the demo sidechain
                                              : renderPlugin (*p, 48000.0, 256, secs, sc);
            p->setPlayHead (nullptr);
            const auto st = stats (out);
            if (st.finite && st.rms > 0.002 && st.peak <= 0.8913) ok++;      // every preset peaks at or below -1 dBFS
            else bad.add (s.presetName + " rms " + juce::String (db (st.rms), 1) + " peak " + juce::String (db (st.peak), 1));
            float g = 1.0f;
            const auto f = wavDir.getChildFile (juce::String (i + 1).paddedLeft ('0', 2) + "_" + s.presetName.toLowerCase().replaceCharacters (" -", "__") + ".wav");
            if (writeWav (f, out, 48000.0, g))
                written.add (f.getFileName() + " (" + juce::String (secs, 1) + " s, rms " + juce::String (db (st.rms), 1) + " dBFS, peak " + juce::String (db (st.peak), 1) + " dBFS)");
        }
        check (ok == orbit::factoryCount(), juce::String (ok) + " of " + juce::String (orbit::factoryCount()) + " ORBIT factory presets play through the VST3 (finite, sounding, peak at or below -1 dBFS) "
                                                + bad.joinIntoString (", "));
        info ("preset renders in " + wavDir.getFullPathName() + ":");
        for (const auto& w : written) info ("  " + w);
        check (written.size() == orbit::factoryCount(), juce::String (written.size()) + " preset WAVs written");
        // CPU through the VST3, the three-layer preset, 48 kHz / 256
        auto p = make();
        const auto s = orbit::factoryPreset (7);
        setPluginState (*p, orbitBlob (*p, &s, "orbit"));
        const auto t0 = std::chrono::steady_clock::now();
        renderPlugin (*p, 48000.0, 256, 12.0, threeChords);
        const double cpu = std::chrono::duration<double> (std::chrono::steady_clock::now() - t0).count() / 12.0;
        check (cpu < 0.6, "ORBIT CPU through the VST3 (THREE-BODY ORBIT, 5-note chords, 48 kHz / 256): " + juce::String (cpu * 100.0, 1) + " % of one core");
    }

    // ------------------------------------------------------------------- 7. 1.2.0: live input ---
    std::printf ("[1.2.0: the Sidechain input bus]\n");
    {
        using namespace Steinberg;
        auto p = make();
        auto* b = p->getBusCount (true) == 1 ? p->getBus (true, 0) : nullptr;
        check (b != nullptr && b->getName() == "Sidechain", "one audio input bus, \"Sidechain\" (" + juce::String (p->getBusCount (true)) + " input buses)");
        if (b != nullptr)
            check (b->getDefaultLayout() == juce::AudioChannelSet::stereo() && b->isEnabledByDefault() && b->isEnabled(),
                   "sidechain: stereo, active by default");
        Vst::IComponent* comp = p->getVST3Client() != nullptr ? p->getVST3Client()->getIComponentPtr() : nullptr;
        if (comp != nullptr)
        {
            Vst::BusInfo bi {};
            const bool got = comp->getBusCount (Vst::kAudio, Vst::kInput) == 1 && comp->getBusInfo (Vst::kAudio, Vst::kInput, 0, bi) == kResultOk;
            const juce::String nm (juce::CharPointer_UTF16 (reinterpret_cast<const juce::CharPointer_UTF16::CharType*> (bi.name)));
            check (got && bi.busType == Vst::kAux && bi.channelCount == 2 && (bi.flags & Vst::BusInfo::kDefaultActive) != 0 && nm == "Sidechain",
                   "VST3 input bus 0: type " + juce::String (bi.busType == Vst::kAux ? "kAux" : "kMain") + ", " + juce::String (bi.channelCount)
                       + " channels, " + ((bi.flags & Vst::BusInfo::kDefaultActive) ? "kDefaultActive" : "not default-active") + ", \"" + nm + "\"");
            Vst::BusInfo bo {};
            check (comp->getBusCount (Vst::kAudio, Vst::kOutput) == 1 && comp->getBusInfo (Vst::kAudio, Vst::kOutput, 0, bo) == kResultOk
                       && bo.busType == Vst::kMain && bo.channelCount == 2, "VST3 output bus 0: main, stereo");
            Vst::IAudioProcessor* ap = nullptr;
            comp->queryInterface (Vst::IAudioProcessor::iid, reinterpret_cast<void**> (&ap));
            if (ap != nullptr)
            {
                // what some hosts ask (inactive component): stereo, then mono; a DAW may also switch the input off
                p->releaseResources();
                comp->setActive (false);
                Vst::SpeakerArrangement st = Vst::SpeakerArr::kStereo, mo = Vst::SpeakerArr::kMono, none = 0, s51 = Vst::SpeakerArr::k51;
                const auto rs = ap->setBusArrangements (&st, 1, &st, 1);
                const auto rm = ap->setBusArrangements (&mo, 1, &st, 1);
                Vst::SpeakerArrangement got1 = 0;
                ap->getBusArrangement (Vst::kInput, 0, got1);
                const auto rn = ap->setBusArrangements (&none, 1, &st, 1);
                const auto r51 = ap->setBusArrangements (&s51, 1, &st, 1);
                const auto rmo = ap->setBusArrangements (&st, 1, &mo, 1);
                ap->setBusArrangements (&st, 1, &st, 1);
                check (rs == kResultTrue && rm == kResultTrue && got1 == Vst::SpeakerArr::kMono && rn == kResultTrue && r51 != kResultTrue && rmo == kResultTrue,
                       "setBusArrangements: stereo in " + juce::String (rs == kResultTrue ? "ok" : "refused") + ", mono in " + (rm == kResultTrue ? "ok" : "refused")
                           + ", no channels " + (rn == kResultTrue ? "ok" : "refused") + ", 5.1 in " + (r51 == kResultTrue ? "accepted (wrong)" : "refused")
                           + ", mono out " + (rmo == kResultTrue ? "ok" : "refused"));
                const auto ra = comp->activateBus (Vst::kAudio, Vst::kInput, 0, true);
                check (ra == kResultTrue, "activateBus (audio input 0) accepted");
                ap->release();
            }
        }
        // JUCE's own layout checks (what JUCE-based hosts ask)
        auto lay = p->getBusesLayout();
        lay.inputBuses.getReference (0) = juce::AudioChannelSet::mono();
        const bool monoOk = p->checkBusesLayoutSupported (lay);
        lay.inputBuses.getReference (0) = juce::AudioChannelSet::disabled();
        const bool offOk = p->checkBusesLayoutSupported (lay);
        lay.inputBuses.getReference (0) = juce::AudioChannelSet::create5point1();
        const bool s51Ok = p->checkBusesLayoutSupported (lay);
        check (monoOk && offOk && ! s51Ok, "layouts: mono and disabled sidechain accepted, 5.1 refused");
    }

    auto sine220 = [] (int, int64_t i) { return 0.5f * (float) std::sin (2.0 * 3.14159265358979 * 220.0 * (double) i / 48000.0); };
    auto noiseIn = [] (int c, int64_t i) { uint32_t x = (uint32_t) (i * 2654435761u) ^ (uint32_t) (c * 0x9e3779b9u); x ^= x >> 13; x *= 0x5bd1e995u; x ^= x >> 15;
                                           return (float) (x >> 8) / 8388608.0f - 1.0f; };   // full scale
    auto relDiff = [] (const juce::AudioBuffer<float>& a, const juce::AudioBuffer<float>& b, int from)
    {
        double d = 0, s = 0;
        for (int c = 0; c < 2; c++)
            for (int i = from; i < a.getNumSamples(); i++)
            {
                const double x = a.getSample (c, i), y = b.getSample (c, i);
                d += (x - y) * (x - y);
                s += std::max (x * x, y * y);
            }
        return std::sqrt (d / std::max (s, 1e-30));
    };
    const std::vector<Ev> oneNote = { { 0.1, { 0x90, 57, 100 } }, { 2.6, { 0x80, 57, 0 } } };

    std::printf ("[1.2.0: ORBIT live input through the VST3]\n");
    {
        auto p = make();
        const int prog25 = mfm1_factory::count + 8, prog26 = prog25 + 1;
        // absent / disabled, silent, full-scale noise: every INPUT preset stays finite and quiet when nothing arrives
        for (int pr : { prog25, prog26 })
        {
            const auto nm = p->getProgramName (pr);
            auto fresh = [&] { auto q = make(); q->setCurrentProgram (pr); return q; };   // the random walks run on across renders of one instance
            const auto off = renderIn (*fresh(), 48000.0, 256, 3.0, oneNote, {}, 0);
            const auto sil = renderIn (*fresh(), 48000.0, 256, 3.0, oneNote, [] (int, int64_t) { return 0.0f; }, 2);
            const auto nz = renderIn (*fresh(), 48000.0, 256, 3.0, oneNote, noiseIn, 2);
            const auto mono = renderIn (*fresh(), 48000.0, 256, 3.0, oneNote, sine220, 1);
            const auto so = stats (off), ss = stats (sil), sn = stats (nz), sm = stats (mono);
            check (so.finite && ss.finite && sn.finite && sm.finite && sn.peak < 1.5, nm + ": input off / silent / full-scale noise / mono sine: finite, noise peak "
                       + juce::String (db (sn.peak), 1) + " dBFS (rms off " + juce::String (db (so.rms), 1) + ", silent " + juce::String (db (ss.rms), 1)
                       + ", noise " + juce::String (db (sn.rms), 1) + ", mono sine " + juce::String (db (sm.rms), 1) + " dBFS)");
            check (maxDiff (off, sil) < 1e-6, nm + ": a switched-off input sounds the same as a silent one (max diff " + juce::String (maxDiff (off, sil), 9) + ")");
            check (sm.rms > 2.0 * ss.rms + 1e-5, nm + ": a mono sidechain is used ((L+R)/2 path)");
            // no stuck tail: 3 s of noise, then 6 s of silence
            auto burst = [&] (int c, int64_t i) { return i < 3 * 48000 ? noiseIn (c, i) : 0.0f; };
            const auto t = renderIn (*fresh(), 48000.0, 256, 12.0, {}, burst, 2);
            const double tail = stats (t, 11 * 48000, 12 * 48000).rms;
            check (stats (t).finite && tail < 1e-4, nm + ": after the input stops there is no stuck tail (last second " + juce::String (db (tail), 1) + " dBFS)");
        }
        // HALO FOLLOWER: layer 1 = INPUT through a halo, THRU: sound with the sine alone, no note
        {
            p->setCurrentProgram (prog26);
            const auto a = renderIn (*p, 48000.0, 256, 2.0, {}, sine220, 2);
            const auto z = renderIn (*p, 48000.0, 256, 2.0, {}, [] (int, int64_t) { return 0.0f; }, 2);
            check (stats (a, 48000).rms > 0.003 && stats (z).rms < 1e-6, "INPUT source (THRU) plays the sidechain without notes (rms " + juce::String (db (stats (a, 48000).rms), 1)
                       + " dBFS; silent input " + juce::String (db (stats (z).rms), 1) + " dBFS)");
        }
        // every dimension with INPUT: a REGOLITH pad (layer 1) with the INPUT as its impulse, one dimension at a time;
        // the sidechain (sine / noise / silent) must change the output measurably
        static const int dims[5] = { MFO_L_BODY, MFO_L_TONE, MFO_L_GRAIN, MFO_L_MOTION, MFO_L_PITCH };
        static const char* dn[5] = { "BODY", "TONE", "GRAIN", "MOTION", "PITCH" };
        for (int d = 0; d < 5; d++)
        {
            orbit::Settings s = orbit::factoryPreset (10);       // PARKED - TWIN FIELD: a plain held start
            for (int l = 0; l < orbit::layers; l++) { s.src[l].kind = orbit::Src::off; s.imp[l].kind = orbit::Imp::off; }
            s.src[0].kind = orbit::Src::synth;
            s.src[0].patch.name = "Velvet Pad";
            s.impInput[0] = true;
            for (int k = 0; k < 5; k++) s.core[(size_t) MFO_LPARAM (0, dims[k])] = k == d ? 1.0f : 0.0f;
            s.core[MFO_P_PUCK_X] = -0.7f; s.core[MFO_P_PUCK_Y] = 0.7f;
            s.mods = {};
            for (auto& f : s.fxOn) f = false;
            s.core[MFO_P_MASTER] = 0.8f;
            const auto blobD = orbitBlob (*p, &s, "orbit");
            p->setStateInformation (blobD.getData(), (int) blobD.getSize());
            const auto a = renderIn (*p, 48000.0, 256, 3.0, oneNote, sine220, 2);
            const auto bnz = renderIn (*p, 48000.0, 256, 3.0, oneNote, [&] (int c, int64_t i) { return 0.25f * noiseIn (c, i); }, 2);
            const auto z = renderIn (*p, 48000.0, 256, 3.0, oneNote, [] (int, int64_t) { return 0.0f; }, 2);
            const double dsn = relDiff (a, bnz, 48000), dsz = relDiff (a, z, 48000);
            check (stats (a).finite && dsn > 0.1 && dsz > 0.1, juce::String (dn[d]) + " with INPUT: sine vs noise on the sidechain differ by " + juce::String (100.0 * dsn, 1)
                       + " %, sine vs silence by " + juce::String (100.0 * dsz, 1) + " % (rms sine " + juce::String (db (stats (a, 48000).rms), 1)
                       + ", noise " + juce::String (db (stats (bnz, 48000).rms), 1) + ", silent " + juce::String (db (stats (z, 48000).rms), 1) + " dBFS)");
        }
        // latency: INPUT as a source, everything else off: a click comes out exactly the reported latency later
        {
            orbit::Settings s = orbit::factoryPreset (10);
            for (int l = 0; l < orbit::layers; l++) { s.src[l].kind = orbit::Src::off; s.imp[l].kind = orbit::Imp::off; s.core[(size_t) MFO_LPARAM (l, MFO_L_BODY)] = 0.0f; }
            s.srcInput[0] = true;
            s.core[MFO_P_PUCK_X] = -0.7f; s.core[MFO_P_PUCK_Y] = 0.7f;
            s.core[MFO_P_DRIFT] = 0.0f; s.core[MFO_P_DRIVE] = 0.0f; s.core[MFO_P_MASTER] = 1.0f;
            s.mods = {};
            for (auto& f : s.fxOn) f = false;
            const auto blob = orbitBlob (*p, &s, "orbit");
            p->setStateInformation (blob.getData(), (int) blob.getSize());
            const int at = 24000;
            const auto o = renderIn (*p, 48000.0, 256, 1.0, {}, [&] (int, int64_t i) { return i == at ? 0.3f : 0.0f; }, 2);
            int arg = -1; float best = 0;
            for (int i = 0; i < o.getNumSamples(); i++) if (std::fabs (o.getSample (0, i)) > best) { best = std::fabs (o.getSample (0, i)); arg = i; }
            const int lat = p->getLatencySamples();
            check (arg - at == lat && lat == 347, "INPUT source latency: a click on the sidechain comes out " + juce::String (arg - at) + " samples later = the reported "
                       + juce::String (lat) + " samples (" + juce::String (lat / 48.0, 2) + " ms at 48 kHz, compensated by the host)");
        }
    }

    std::printf ("[1.2.0: held orbit, stopped orbit, puck automation (engine host)]\n");
    {
        auto runner = [] (const orbit::Settings& s)
        {
            auto r = std::make_unique<orbit::Runner>();
            r->prepare (48000.0, 256);
            orbitApply (*r, s);
            r->createEngine();
            r->waitIdle (20000);
            return r;
        };
        const auto& hp = orbit::hostParams();
        const int hX = orbit::hostIndexForCore (MFO_P_PUCK_X), hY = orbit::hostIndexForCore (MFO_P_PUCK_Y);
        const int hMotion = orbit::hostIndexForSpecial (orbit::spMotion), hRate = orbit::hostIndexForSpecial (orbit::spRate);
        std::vector<float> l (256), rr (256);
        {
            orbit::Settings s = orbit::factoryPreset (10);
            s.core[MFO_P_ORBIT_WOBBLE] = 0.8f;      // wobble turned up: it must not move a held puck (puck drift 0)
            s.mods = {};
            auto r = runner (s);
            for (int k = 0; k < 40; k++) r->process (l.data(), rr.data(), 256, nullptr, 0);
            const auto m0 = r->meter();
            bool same = true;
            for (int k = 0; k < 400; k++)
            {
                r->process (l.data(), rr.data(), 256, nullptr, 0);
                const auto m = r->meter();
                same = same && m.puckX == m0.puckX && m.puckY == m0.puckY && std::memcmp (m.w, m0.w, sizeof m.w) == 0;
            }
            check (same, "held puck with WOBBLE 0.8 and PUCK DRIFT 0: position and blend weights bit-identical over 2 s ("
                             + juce::String (m0.w[0], 3) + " / " + juce::String (m0.w[1], 3) + " / " + juce::String (m0.w[2], 3) + " / " + juce::String (m0.w[3], 3) + ")");
            // automation: Puck X/Y to node 1, then node 3 -> the blend weights follow
            r->setHostNorm (hX, orbit::normFromCore (hp[(size_t) hX], -0.7f));
            r->setHostNorm (hY, orbit::normFromCore (hp[(size_t) hY], 0.6f));
            for (int k = 0; k < 60; k++) r->process (l.data(), rr.data(), 256, nullptr, 0);
            const auto m1 = r->meter();
            r->setHostNorm (hX, orbit::normFromCore (hp[(size_t) hX], 0.7f));
            r->process (l.data(), rr.data(), 64, nullptr, 0);
            const auto mStep = r->meter();
            r->setHostNorm (hY, orbit::normFromCore (hp[(size_t) hY], 0.6f));
            for (int k = 0; k < 60; k++) r->process (l.data(), rr.data(), 256, nullptr, 0);
            const auto m2 = r->meter();
            check (m1.w[0] > 0.98f && m2.w[1] > 0.98f && mStep.puckX < 0.0f,
                   "ORBIT Puck X/Y automation while held: node 1 -> L1 weight " + juce::String (m1.w[0], 4) + ", node 2 -> L2 weight " + juce::String (m2.w[1], 4)
                       + "; smoothed (x " + juce::String (mStep.puckX, 3) + " 64 samples after a jump to 0.7)");
        }
        {
            // ORBIT Motion on, CIRCLE, RATE at its minimum: stopped (no creep); back up: it moves
            orbit::Settings s = orbit::factoryPreset (10);
            s.path = orbit::pathCircle; s.shape = orbit::pathCircle; s.sync = false; s.rate = 0.0f;
            s.core[MFO_P_ORBIT_WOBBLE] = 0.0f;
            s.mods = {};
            auto r = runner (s);
            for (int k = 0; k < 20; k++) r->process (l.data(), rr.data(), 256, nullptr, 0);
            const auto a = r->meter();
            for (int k = 0; k < 600; k++) r->process (l.data(), rr.data(), 256, nullptr, 0);
            const auto b2 = r->meter();
            r->setHostNorm (hRate, 0.5f);
            for (int k = 0; k < 200; k++) r->process (l.data(), rr.data(), 256, nullptr, 0);
            const auto c = r->meter();
            r->setHostNorm (hMotion, 0.0f);
            r->setHostNorm (hRate, 0.5f);
            for (int k = 0; k < 20; k++) r->process (l.data(), rr.data(), 256, nullptr, 0);
            const auto d0 = r->meter();
            for (int k = 0; k < 200; k++) r->process (l.data(), rr.data(), 256, nullptr, 0);
            const auto d1 = r->meter();
            check (a.puckX == b2.puckX && a.puckY == b2.puckY && (c.puckX != b2.puckX || c.puckY != b2.puckY) && d0.puckX == d1.puckX && d0.puckY == d1.puckY,
                   "RATE 0 = stopped (no creep over 3.2 s); RATE up moves it; ORBIT Motion off holds it (host-automatable)");
        }
        {
            // a 1.1.0 state keeps its behaviour: wobble still moves a held puck there
            orbit::Settings s = orbit::factoryPreset (3);        // SPRING RAIN: held, wobble 0.35 (1.1.0)
            auto r = runner (s);
            for (int k = 0; k < 20; k++) r->process (l.data(), rr.data(), 256, nullptr, 0);
            const auto a = r->meter();
            for (int k = 0; k < 400; k++) r->process (l.data(), rr.data(), 256, nullptr, 0);
            const auto b2 = r->meter();
            check (s.legacyWobble && (a.puckX != b2.puckX || a.puckY != b2.puckY), "1.1.0 presets keep their wobble on a held puck (SPRING RAIN, legacy)");
        }
        {
            int held = 0, moving = 0, sweep = 0;
            for (int i = 0; i < orbit::factoryCount(); i++)
            {
                const auto s = orbit::factoryPreset (i);
                if (s.path == orbit::pathHold) held++; else moving++;
                bool mx = false;
                for (const auto& m : s.mods) mx = mx || ((m.dst == MFO_P_PUCK_X || m.dst == MFO_P_PUCK_Y) && m.src >= MFO_MS_MACRO1 && m.src <= MFO_MS_MACRO4 && m.amount != 0.0f);
                if (s.path == orbit::pathHold && mx) sweep++;
            }
            check (held >= 3 && moving >= 2 && sweep >= 3 && orbit::Settings().path == orbit::pathHold,
                   juce::String (held) + " factory presets hold the puck (" + juce::String (sweep) + " with a macro on puck X/Y), " + juce::String (moving)
                       + " move; a new patch starts held");
        }
    }

    std::printf ("[1.2.0: engine host with input: allocation, CPU]\n");
    {
        for (int pi : { 8, 9 })
        {
            orbit::Runner r;
            r.prepare (48000.0, 256);
            orbitApply (r, orbit::factoryPreset (pi));
            r.createEngine();
            r.waitIdle (20000);
            std::vector<float> l (2048), rr (2048), il (2048), ir (2048);
            for (size_t i = 0; i < il.size(); i++) { il[i] = noiseIn (0, (int64_t) i); ir[i] = noiseIn (1, (int64_t) i); }
            mfm1::MidiEvent mev[2] = { { 10, { 0x90, 48, 100 }, 3 }, { 77, { 0x90, 55, 100 }, 3 } };
            r.process (l.data(), rr.data(), il.data(), ir.data(), 64, nullptr, 0);
            allocs = 0;
            countAllocs = true;
            bool fin = true;
            for (int k = 0; k < 600; k++)
            {
                const int n = (k % 5) * 97 + 31;
                r.process (l.data(), rr.data(), il.data(), k % 7 == 3 ? nullptr : ir.data(), n, k == 3 ? mev : nullptr, k == 3 ? 2 : 0);
                for (int i = 0; i < n; i++) fin = fin && std::isfinite (l[(size_t) i]) && std::isfinite (rr[(size_t) i]);
            }
            countAllocs = false;
            check (allocs.load() == 0 && fin, orbit::factoryName (pi) + ": no heap allocation on the audio thread with the input over 600 blocks ("
                                                  + juce::String ((int) allocs.load()) + "), all finite");
            // CPU at 48 kHz / 256, five-note chords, noise on the sidechain
            r.reset();
            const int total = 48000 * 8;
            std::vector<mfm1::MidiEvent> ev;
            double best = 1e9;
            for (int run = 0; run < 3; run++)
            {
                const auto t0 = std::chrono::steady_clock::now();
                for (int pos = 0; pos < total; pos += 256)
                {
                    ev.clear();
                    if (pos % 96000 == 0) for (int nn : { 48, 55, 60, 64, 67 }) ev.push_back ({ 0, { 0x90, (uint8_t) nn, 100 }, 3 });
                    if (pos % 96000 == 84992) for (int nn : { 48, 55, 60, 64, 67 }) ev.push_back ({ 0, { 0x80, (uint8_t) nn, 0 }, 3 });
                    r.process (l.data(), rr.data(), il.data() + (pos % 1024), ir.data() + (pos % 1024), 256, ev.data(), (int) ev.size());
                }
                best = std::min (best, std::chrono::duration<double> (std::chrono::steady_clock::now() - t0).count() / 8.0);
            }
            check (best < 0.6, "ORBIT CPU at 48 kHz / 256 with the input, " + orbit::factoryName (pi) + ": " + juce::String (best * 100.0, 1) + " % of one core");
        }
    }

    std::printf ("[1.2.0: 1.1.0 states load and render bit-identically]\n");
    {
        juce::File ref (refPath);
        const auto repoDir = juce::File (path).getParentDirectory().getParentDirectory().getParentDirectory();
        const auto fxDir = repoDir.getChildFile ("plugin/tests/fixtures");
        juce::OwnedArray<juce::PluginDescription> rt;
        if (ref.exists()) vst3->findAllTypesForFile (rt, ref.getFullPathName());
        const bool haveRef = rt.size() == 1 && rt[0]->version == "1.1.0";
        info ("reference: " + ref.getFullPathName() + (haveRef ? " (1.1.0)" : " (not found: fixtures only)"));
        auto makeRef = [&]
        {
            juce::String err;
            auto q = fm.createPluginInstance (*rt[0], 48000.0, 256, err);
            return q;
        };
        // fixtures: one REGOLITH (CHONK) and three ORBIT programs saved by 1.1.0 (written from the reference once)
        const int progs[4] = { 2, mfm1_factory::count + 0, mfm1_factory::count + 3, mfm1_factory::count + 6 };
        int ok = 0, have = 0;
        juce::StringArray notes;
        for (int pr : progs)
        {
            const auto fx = fxDir.getChildFile ("state_1.1.0_program" + juce::String (pr + 1) + ".bin");
            if (haveRef && ! fx.existsAsFile())
            {
                auto q = makeRef();
                q->setCurrentProgram (pr);
                juce::MemoryBlock mb;
                q->getStateInformation (mb);
                fx.replaceWithData (mb.getData(), mb.getSize());
            }
            juce::MemoryBlock st;
            if (! fx.loadFileAsData (st)) continue;
            have++;
            auto fresh = make();
            fresh->setStateInformation (st.getData(), (int) st.getSize());
            const auto out = renderPlugin (*fresh, 48000.0, 256, 6.0, threeChords);
            bool same = stats (out).finite && stats (out).rms > 0.001;
            juce::String note = juce::String (pr + 1) + ": rms " + juce::String (db (stats (out).rms), 1);
            if (haveRef)
            {
                auto q = makeRef();
                q->setStateInformation (st.getData(), (int) st.getSize());
                const auto want = renderPlugin (*q, 48000.0, 256, 6.0, threeChords);
                const double d = maxDiff (out, want);
                same = same && d == 0.0 && fresh->getLatencySamples() == q->getLatencySamples();
                note += ", max diff vs 1.1.0 " + juce::String (d, 12);
            }
            if (same) ok++;
            notes.add (note);
        }
        check (have == 4 && ok == 4 && (haveRef || ! refRequired), juce::String (ok) + " of " + juce::String (have) + " 1.1.0 states (programs 3, 17, 20, 23) load and render "
                   + (haveRef ? "bit-identically to the 1.1.0 release" : "(fixtures only: no 1.1.0 bundle to compare with; give one with --ref)") + ": " + notes.joinIntoString ("; "));
        // the 1.1.0 parameter set: every 1.1.0 id is still there with its name
        if (haveRef)
        {
            auto q = makeRef();
            auto fresh = make();
            std::map<juce::String, juce::String> now;
            for (auto* prm : fresh->getParameters())
                if (auto* hp = dynamic_cast<juce::HostedAudioProcessorParameter*> (prm)) now[hp->getParameterID()] = prm->getName (64);
            int same = 0, total = 0;
            for (auto* prm : q->getParameters())
                if (auto* hp = dynamic_cast<juce::HostedAudioProcessorParameter*> (prm))
                {
                    total++;
                    const auto it = now.find (hp->getParameterID());
                    if (it != now.end() && it->second == prm->getName (64)) same++;
                }
            check (total > 0 && same == total, juce::String (same) + " of " + juce::String (total) + " 1.1.0 host parameters keep their id and name");
            bool names = true;
            for (int i = 0; i < q->getNumPrograms(); i++) names = names && q->getProgramName (i) == fresh->getProgramName (i);
            check (names && q->getNumPrograms() == 24, "programs 1-24 named as in 1.1.0");
        }
    }

    // ------------------------------------------------------------------------------ 5. editor ---
    if (shots)
    {
        std::printf ("[editor: WebView2 panel]\n");
        const auto statusA = outDir.getChildFile ("webview2_status_host.txt");
        statusA.deleteFile();
        SetEnvironmentVariableW (L"ATOMIC_LOUNGE_TEST_STATUS", statusA.getFullPathName().toWideCharPointer());
        auto p = make();
        std::unique_ptr<juce::AudioProcessorEditor> ed (p->createEditorIfNeeded());
        check (ed != nullptr, "editor created");
        if (ed != nullptr)
        {
            info ("editor size " + juce::String (ed->getWidth()) + " x " + juce::String (ed->getHeight()) + (ed->isResizable() ? ", resizable" : ", fixed size"));
            check (ed->getWidth() == 1440 && ed->getHeight() == 900, "default size 1440 x 900");
            juce::DocumentWindow win ("Atomic Lounge (host test)", juce::Colours::black, 0, true);
            win.setUsingNativeTitleBar (true);
            win.setContentNonOwned (ed.get(), true);
            win.setTopLeftPosition (40, 40);
            win.setVisible (true);
            const auto t0 = juce::Time::getMillisecondCounter();
            // the panel reports its preset over the bridge as JSON.stringify writes it (no spaces);
            // the plugin's own initial record has spaces: the change proves page load + JS -> native
            const bool up = pumpUntil ([&] { return stateUi (*p).contains ("\"kind\":\"F\""); }, 25000);
            check (up, "panel loaded and reported its state to the plugin (" + juce::String ((int) (juce::Time::getMillisecondCounter() - t0)) + " ms): " + stateUi (*p));
            // ---- 1.2.0: WebView2 per host program; two editors in one host; a second host at the same time; the fallback
            {
                const auto st = statusA.loadFileAsString().removeCharacters ("\r");
                const auto want = juce::File::getSpecialLocation (juce::File::windowsLocalAppData).getChildFile ("Atomic Lounge").getChildFile ("WebView2")
                                      .getChildFile (juce::File::getSpecialLocation (juce::File::currentExecutableFile).getFileNameWithoutExtension());
                check (st.contains ("folder " + want.getFullPathName() + "\n") && st.contains ("\nloaded"),
                       "WebView2 data folder per host program: " + want.getFullPathName() + " (status: " + st.replace ("\n", " | ").trim() + ")");
                // a second editor (another instance) in the same host, both open
                auto p2 = make();
                std::unique_ptr<juce::AudioProcessorEditor> ed2 (p2->createEditorIfNeeded());
                juce::DocumentWindow win2 ("Atomic Lounge (host test, editor 2)", juce::Colours::black, 0, true);
                win2.setUsingNativeTitleBar (true);
                win2.setContentNonOwned (ed2.get(), true);
                win2.setTopLeftPosition (90, 90);
                win2.setVisible (true);
                auto loadedCount = [&] { int k = 0; for (const auto& ln : juce::StringArray::fromLines (statusA.loadFileAsString())) k += ln.startsWith ("loaded") ? 1 : 0; return k; };
                const bool two = pumpUntil ([&] { return loadedCount() >= 2 && stateUi (*p2).contains ("\"kind\":\"F\""); }, 25000);
                check (two && stateUi (*p2).contains ("\"kind\":\"F\""), "two editors open at once in one host: both panels load (one WebView2 folder, one browser process)");
                // a second host program while both editors stay open
                auto launch = [&] (const juce::String& exeName, const juce::File& status, int& code)
                {
                    const auto self = juce::File::getSpecialLocation (juce::File::currentExecutableFile);
                    const auto copy = outDir.getChildFile (exeName + ".exe");
                    copy.deleteFile();
                    self.copyFileTo (copy);
                    status.deleteFile();
                    juce::ChildProcess cp;
                    code = -1;
                    if (! cp.start (juce::StringArray { copy.getFullPathName(), "--editor-child", path, status.getFullPathName(), "40" }, 0)) return;
                    const auto t0 = juce::Time::getMillisecondCounter();
                    while (cp.isRunning() && juce::Time::getMillisecondCounter() - t0 < 60000) juce::MessageManager::getInstance()->runDispatchLoopUntil (50);
                    if (cp.isRunning()) cp.kill();
                    code = (int) cp.getExitCode();
                };
                const auto statusB = outDir.getChildFile ("webview2_status_hostB.txt");
                int codeB = -1;
                launch ("AtomicLoungeHostB", statusB, codeB);
                const auto sb = statusB.loadFileAsString().removeCharacters ("\r");
                check (codeB == 0 && sb.contains ("WebView2\\AtomicLoungeHostB\n") && sb.contains ("\nloaded"),
                       "a second host program opens its own panel while this host has two open (exit " + juce::String (codeB) + "; " + sb.replace ("\n", " | ").trim() + ")");
                // the fallback: a host where WebView2 cannot start (runtime folder forced to a missing one)
                const auto statusC = outDir.getChildFile ("webview2_status_hostC.txt");
                SetEnvironmentVariableW (L"WEBVIEW2_BROWSER_EXECUTABLE_FOLDER", outDir.getChildFile ("no-such-webview2-runtime").getFullPathName().toWideCharPointer());
                int codeC = -1;
                launch ("AtomicLoungeHostC", statusC, codeC);
                SetEnvironmentVariableW (L"WEBVIEW2_BROWSER_EXECUTABLE_FOLDER", nullptr);
                const auto sc = statusC.loadFileAsString().removeCharacters ("\r");
                check (codeC == 2 && sc.contains ("\nfallback") && sc.contains ("hr=0x8"),
                       "WebView2 cannot start: the editor shows the reason, the HRESULT and Retry instead of a blank window (exit " + juce::String (codeC) + "; "
                           + sc.replace ("\n", " | ").trim() + ")");
                for (const char* n : { "AtomicLoungeHostB.exe", "AtomicLoungeHostC.exe" }) outDir.getChildFile (n).deleteFile();
                win2.setVisible (false);
                win2.clearContentComponent();
                ed2.reset();
                pumpUntil ({}, 300);
                p2->releaseResources();
            }
            p->setCurrentProgram (3);
            const bool moved = pumpUntil ([&] { return stateUi (*p).contains ("\"name\":\"Velvet Pad\""); }, 6000);
            check (moved, "host program change reached the panel and came back: " + stateUi (*p));
            auto* cut = paramById (*p, "f1_cutoff");
            cut->setValueNotifyingHost (0.42f);
            pumpUntil ({}, 1200);
            check (std::fabs (cut->getValue() - 0.42f) < 1e-3f, "host automation shown on the panel without an echo back (cutoff still " + juce::String (cut->getValue(), 4) + ")");
            pumpUntil ({}, 1500);
            auto* peer = win.getPeer();
            if (peer != nullptr)
            {
                auto img = captureClient ((HWND) peer->getNativeHandle());
                const auto f = outDir.getChildFile ("editor_" + juce::String (img.getWidth()) + "x" + juce::String (img.getHeight()) + ".png");
                savePng (img, f);
                // a rendered panel has many colours; a blank or failed WebView is one or two
                std::set<juce::uint32> cols;
                for (int y = 0; y < img.getHeight(); y += 7)
                    for (int x2 = 0; x2 < img.getWidth(); x2 += 7) cols.insert (img.getPixelAt (x2, y).getARGB() & 0xfcfcfcu);
                check (cols.size() > 200, "screenshot shows a drawn panel (" + juce::String ((int) cols.size()) + " colours): " + f.getFullPathName());
            }
            ed->setSize (960, 600);
            pumpUntil ({}, 1500);
            check (std::abs (ed->getWidth() * 1200 - ed->getHeight() * 1920) < 2000, "resize keeps 16:10 (" + juce::String (ed->getWidth()) + " x " + juce::String (ed->getHeight()) + ")");
            if (peer != nullptr)
            {
                auto img = captureClient ((HWND) peer->getNativeHandle());
                savePng (img, outDir.getChildFile ("editor_" + juce::String (img.getWidth()) + "x" + juce::String (img.getHeight()) + ".png"));
            }
            win.setVisible (false);
            win.clearContentComponent();
            ed.reset();
            pumpUntil ({}, 300);
        }
    }

    if (shots)
    {
        std::printf ("[editor: the ORBIT panel]\n");
        auto p = make();
        const auto s0 = orbit::factoryPreset (0);
        setPluginState (*p, orbitBlob (*p, &s0, "orbit"));
        p->prepareToPlay (48000.0, 256);
        std::unique_ptr<juce::AudioProcessorEditor> ed (p->createEditorIfNeeded());
        check (ed != nullptr, "ORBIT editor created");
        if (ed != nullptr)
        {
            juce::DocumentWindow win ("Atomic Lounge ORBIT (host test)", juce::Colours::black, 0, true);
            win.setUsingNativeTitleBar (true);
            win.setContentNonOwned (ed.get(), true);
            win.setTopLeftPosition (40, 40);
            win.setVisible (true);
            // play a chord while the panel runs, so the meters, puck and voices move
            juce::AudioBuffer<float> buf (2, 256);
            juce::MidiBuffer midi;
            const uint8_t notes[4] = { 48, 55, 60, 63 };
            auto* peer = win.getPeer();
            auto shot = [&] (const juce::String& name)
            {
                if (peer == nullptr) return juce::Image();
                auto img = captureClient ((HWND) peer->getNativeHandle());
                savePng (img, outDir.getChildFile (name + "_" + juce::String (img.getWidth()) + "x" + juce::String (img.getHeight()) + ".png"));
                return img;
            };
            const auto t0 = juce::Time::getMillisecondCounter();
            int blocks = 0;
            while (juce::Time::getMillisecondCounter() - t0 < 9000)
            {
                midi.clear();
                if (blocks == 200) for (auto n : notes) { const uint8_t m[3] = { 0x90, n, 100 }; midi.addEvent (m, 3, 0); }
                buf.clear();
                p->processBlock (buf, midi);
                blocks++;
                juce::MessageManager::getInstance()->runDispatchLoopUntil (5);
            }
            auto img = shot ("editor_orbit");
            if (img.isValid())
            {
                // the cream lacquer panel: most of it is warm and light; many colours (a rendered page, not a blank WebView)
                std::set<juce::uint32> cols;
                int cream = 0, total = 0;
                for (int y = 0; y < img.getHeight(); y += 5)
                    for (int x2 = 0; x2 < img.getWidth(); x2 += 5)
                    {
                        const auto c = img.getPixelAt (x2, y);
                        cols.insert (c.getARGB() & 0xfcfcfcu);
                        total++;
                        if (c.getRed() > 200 && c.getGreen() > 185 && c.getBlue() > 150 && c.getRed() >= c.getBlue()) cream++;
                    }
                check (cols.size() > 300 && cream > total / 3, "the ORBIT panel is drawn in the running plugin (" + juce::String ((int) cols.size()) + " colours, "
                                                                   + juce::String (100 * cream / std::max (1, total)) + " % cream lacquer)");
            }
            ed->setSize (960, 600);
            pumpUntil ({}, 1500);
            shot ("editor_orbit");
            win.setVisible (false);
            win.clearContentComponent();
            ed.reset();
            pumpUntil ({}, 300);
        }
        p->releaseResources();
    }

    std::printf ("\n%d passed, %d failed\n", passes, fails);
    return fails == 0 ? 0 : 1;
}
