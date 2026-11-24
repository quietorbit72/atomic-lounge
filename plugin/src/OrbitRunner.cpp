// SPDX-License-Identifier: AGPL-3.0-only
// Atomic Lounge plugin (formerly Vintage Chonk), ORBIT mode: the ORBIT core inside the plugin (see OrbitRunner.h).
#include "OrbitRunner.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>

namespace orbit {

ImpulseAsset::~ImpulseAsset()
{
    mfo_keytrack_destroy (kt);
    mfo_kernel_destroy (kernel);
}

static std::vector<float> peaksOf (const float* a, const float* b, int len, int cols)
{
    std::vector<float> out ((size_t) cols * 2, 0.0f);
    if (len <= 0 || a == nullptr) return out;
    float pk = 1e-9f;
    for (int i = 0; i < len; i++) pk = std::max (pk, std::fabs (b ? 0.5f * (a[i] + b[i]) : a[i]));
    for (int c = 0; c < cols; c++)
    {
        const int i0 = (int) ((int64_t) c * len / cols), i1 = std::max (i0 + 1, (int) ((int64_t) (c + 1) * len / cols));
        float mn = 0, mx = 0;
        for (int i = i0; i < i1 && i < len; i++)
        {
            const float v = b ? 0.5f * (a[i] + b[i]) : a[i];
            mn = std::min (mn, v);
            mx = std::max (mx, v);
        }
        out[(size_t) c * 2] = mn / pk;
        out[(size_t) c * 2 + 1] = mx / pk;
    }
    return out;
}

static juce::String secondsText (double s)
{
    if (s < 1.0) return juce::String ((int) std::lround (s * 1000.0)) + " ms";
    return juce::String (s, 2) + " s";
}

static juce::String noteName (float n)
{
    static const char* nm[12] = { "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B" };
    const int k = (int) std::lround (n);
    if (k < 0 || k > 127) return "?";
    return juce::String (nm[k % 12]) + juce::String (k / 12 - 1);
}

// ------------------------------------------------------------------------------------- set up ---
Runner::Runner()
{
    mfm1::initTablesOnce();
    for (auto& v : coreVal) v.store (0.0f);
    for (int i = 0; i < numCore; i++) coreVal[(size_t) i].store (mfo_param_info (i)->def);
    mfo_config_default (&cfg);
    cfg.max_voices = 16;
    for (auto& w : inWin) w.store (0.25f);
    hMotion = hostIndexForSpecial (spMotion);
    hDrift = hostIndexForSpecial (spPuckDrift);
    hTrim = hostIndexForSpecial (spInTrim);
    for (int l = 0; l < layers; l++)
    {
        hSrcIn[l] = hostIndexForSpecial (spSrcIn0 + l);
        hImpIn[l] = hostIndexForSpecial (spImpIn0 + l);
        hGate[l] = hostIndexForSpecial (spGate0 + l);
    }
    worker = std::thread ([this] { workerLoop(); });
}

Runner::~Runner()
{
    {
        std::lock_guard<std::mutex> lk (wm);
        quit = true;
    }
    wcv.notify_all();
    if (worker.joinable()) worker.join();
    for (int l = 0; l < layers; l++)
    {
        delete pendSrc[l].exchange (nullptr);
        delete pendImp[l].exchange (nullptr);
        delete actSrc[l];
        delete actImp[l];
    }
    while (retR.load() != retW.load()) { delete retireRing[retR.load() % retireSize]; retR++; }
    mfo_engine_destroy (eng);
}

int Runner::latencySamples() const
{
    return coreLatencyFor (cfg) + fx.latencySamples();
}

void Runner::prepare (double hostRate, int mb)
{
    const double newRate = hostRate > 1000.0 ? hostRate : 44100.0;
    maxBlock = mb;
    const bool rateChanged = std::llround (newRate) != std::llround (fs);
    fs = newRate;
    cfg.sample_rate = (float) fs;
    fx.prepare (fs);
    ageK = 1.0f;
    if (eng != nullptr && ! rateChanged)
    {
        reset();
        return;
    }
    if (eng != nullptr)
    {
        mfo_engine_destroy (eng);
        eng = mfo_engine_create (&cfg);
        appliedValid = false;
        rebuildAllNow();
        reset();
    }
    else
        rateStale = true;       // assets already built for another rate are rebuilt by the worker
}

void Runner::createEngine()
{
    if (eng != nullptr) return;
    cfg.sample_rate = (float) fs;
    eng = mfo_engine_create (&cfg);
    appliedValid = false;
    // the audio thread has not run ORBIT yet: what is waiting is applied here
    if (eng != nullptr) { applyPending(); reset(); }
}

void Runner::reset()
{
    if (eng != nullptr) mfo_engine_reset (eng);
    fx.reset();
    std::memset (sustained, 0, sizeof sustained);
    sustain = false;
    held = 0;
    noteEnv = 0.0f;
    ageLp[0] = ageLp[1] = 0.0f;
    drawnPhase = 0.0;
    spanPos = 0;
    vuL = vuR = pkL = pkR = 0.0f;
    inVuL = inVuR = inPk = 0.0f;
    for (auto& a : inApplied) a = InState();     // the engine's input flags were reset with it
}

// ----------------------------------------------------------------------------- message thread ---
void Runner::setCore (int id, float v)
{
    if (id < 0 || id >= numCore) return;
    const auto* in = mfo_param_info (id);
    coreVal[(size_t) id].store (juce::jlimit (in->min, in->max, v), std::memory_order_relaxed);
    wcv.notify_all();       // keytrack switches are noticed by the worker
}

void Runner::setExtras (bool legacyWobble, const float window[layers])
{
    legacy.store (legacyWobble);
    for (int l = 0; l < layers; l++) inWin[(size_t) l].store (window[l]);
}

void Runner::setPath (int mode, bool sync, const DrawnPath& p)
{
    pathMode.store (juce::jlimit ((int) pathCircle, (int) pathDrawn, mode));
    syncOn.store (sync);
    drawnBuf.write (p);
}

void Runner::setFx (const bool on[numFx], int ensMode, int revType, int bbdSync, int oilSync)
{
    FxSwitch s {};
    for (int i = 0; i < numFx; i++) s.on[i] = on[i];
    s.ens = ensMode;
    s.rev = revType;
    s.bbdSync = bbdSync;
    s.oilSync = oilSync;
    fxBuf.write (s);
}

void Runner::setMods (const std::array<ModSlot, MFO_NMOD>& m) { modBuf.write (m); }

void Runner::requestSource (int l, const SourceSpec& s)
{
    if (l < 0 || l >= layers) return;
    {
        std::lock_guard<std::mutex> lk (wm);
        wantSrc[l] = s;
        srcReq[l]++;
    }
    wcv.notify_all();
}

void Runner::requestImpulse (int l, const ImpulseSpec& s)
{
    if (l < 0 || l >= layers) return;
    {
        std::lock_guard<std::mutex> lk (wm);
        wantImp[l] = s;
        impReq[l]++;
    }
    wcv.notify_all();
}

void Runner::waitIdle (int ms)
{
    const auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds (ms);
    while (std::chrono::steady_clock::now() < until)
    {
        bool idle = busy.load() == 0;
        {
            std::lock_guard<std::mutex> lk (wm);
            for (int l = 0; l < layers; l++) idle = idle && srcReq[l] == srcDone[l] && impReq[l] == impDone[l];
        }
        if (idle && ! rateStale.load()) return;
        wcv.notify_all();
        std::this_thread::sleep_for (std::chrono::milliseconds (5));
    }
}

LayerView Runner::view (int l) const
{
    std::lock_guard<std::mutex> lk (const_cast<std::mutex&> (wm));
    return views[juce::jlimit (0, layers - 1, l)];
}

Runner::Meter Runner::meter() const
{
    Meter m {};
    m.cpu = mCpu.load();
    m.puckX = mPx.load();
    m.puckY = mPy.load();
    for (int i = 0; i < 4; i++) m.w[i] = mW[i].load();
    m.vuL = mVuL.load(); m.vuR = mVuR.load();
    m.peakL = mPkL.load(); m.peakR = mPkR.load();
    m.voices = mVoices.load();
    m.inL = mInL.load(); m.inR = mInR.load(); m.inPk = mInPk.load(); m.inCh = mInCh.load();
    return m;
}

void Runner::postMidi (const uint8_t* m, int n)
{
    const uint32_t w = uiW.load (std::memory_order_relaxed);
    const uint32_t r = uiR.load (std::memory_order_acquire);
    if (w - r >= uiRingSize) return;
    Msg& s = uiRing[w % uiRingSize];
    s.n = (uint8_t) (n > 3 ? 3 : n);
    std::memcpy (s.b, m, s.n);
    uiW.store (w + 1, std::memory_order_release);
}

// ------------------------------------------------------------------------------------- worker ---
SourceAsset* Runner::buildSource (const SourceSpec& s, double rate, LayerView& v)
{
    auto* a = new SourceAsset();
    a->rate = rate;
    v.srcError = {};
    v.srcTrace = false;
    if (s.kind == Src::synth)
    {
        mfe_patch_t p;
        if (! chonkPatch (s.patch, p))
        {
            v.srcName = "NO SOURCE"; v.srcSub = "PRESET NOT FOUND"; v.srcPeaks.clear();
            v.srcError = "Chonk preset " + s.patch.name + " not found";
            return a;
        }
        if (p.max_voices == 8) p.max_voices = 16;   // FM1 presets carry its 8: "all of them" (as the CHONK engine)
        a->kind = Src::synth;
        a->patch = p;
        // a few cycles of C3 for the window
        const int sr = 44100, len = (int) (0.35 * sr);
        std::vector<float> l ((size_t) len), r ((size_t) len);
        mfo_print_synth (&p, 48, 100, 0.3f, 0.35f, (float) sr, l.data(), r.data());
        const int from = (int) (0.22 * sr), span = (int) (2.5 * sr / 130.81);
        v.srcPeaks.assign (200, 0.0f);
        float pk = 1e-6f;
        for (int i = 0; i < span; i++) pk = std::max (pk, std::fabs (0.5f * (l[(size_t) (from + i)] + r[(size_t) (from + i)])));
        for (int i = 0; i < 200; i++)
        {
            const int k = from + i * span / 200;
            v.srcPeaks[(size_t) i] = 0.5f * (l[(size_t) k] + r[(size_t) k]) / pk;
        }
        v.srcTrace = true;
        v.srcName = s.patch.name.toUpperCase();
        v.srcSub = juce::String::fromUTF8 ("REGOLITH PRESET \xc2\xb7 SYNTH");
        return a;
    }
    if (s.kind == Src::sample && s.from == SampleFrom::print)
    {
        mfe_patch_t p;
        if (! chonkPatch (s.patch, p))
        {
            v.srcName = "NO SOURCE"; v.srcSub = "PRESET NOT FOUND"; v.srcPeaks.clear();
            return a;
        }
        const int n = std::max (1, (int) (s.printSeconds * rate));
        a->print[0].assign ((size_t) n, 0.0f);
        a->print[1].assign ((size_t) n, 0.0f);
        mfo_print_synth (&p, s.printNote, 110, s.printHold, s.printSeconds, (float) rate, a->print[0].data(), a->print[1].data());
        a->kind = Src::sample;
        a->buf = { { a->print[0].data(), a->print[1].data() }, 2, n, (float) rate, (float) s.printNote };
        v.srcPeaks = peaksOf (a->print[0].data(), a->print[1].data(), n, 220);
        v.srcName = s.patch.name.toUpperCase();
        v.srcSub = juce::String::fromUTF8 ("REGOLITH PRINT \xc2\xb7 ") + noteName ((float) s.printNote) + juce::String::fromUTF8 (" \xc2\xb7 ") + secondsText (s.printSeconds);
        return a;
    }
    if (s.kind == Src::sample && s.sample != nullptr && s.sample->length() > 1)
    {
        a->kind = Src::sample;
        a->sample = s.sample;
        const auto& d = *s.sample;
        a->buf = { { d.ch[0].data(), d.channels > 1 ? d.ch[1].data() : nullptr }, d.channels > 1 ? 2 : 1, d.length(), (float) d.rate, s.root };
        v.srcPeaks = peaksOf (d.ch[0].data(), d.channels > 1 ? d.ch[1].data() : nullptr, d.length(), 220);
        v.srcName = d.name.upToLastOccurrenceOf (".", false, false).toUpperCase();
        v.srcSub = d.path.fromLastOccurrenceOf (".", false, false).toUpperCase().substring (0, 4) + juce::String::fromUTF8 (" \xc2\xb7 ")
                   + secondsText (d.seconds()) + juce::String::fromUTF8 (" \xc2\xb7 ROOT ") + noteName (s.root);
        return a;
    }
    v.srcPeaks.clear();
    if (s.kind == Src::sample)
    {
        v.srcName = s.missing.isNotEmpty() ? juce::File (s.missing).getFileName().toUpperCase() : juce::String ("DROP A SAMPLE");
        v.srcSub = s.missing.isNotEmpty() ? juce::String ("MISSING FILE") : (juce::String::fromUTF8 ("WAV \xc2\xb7 AIFF \xc2\xb7 FLAC"));
        if (s.missing.isNotEmpty()) v.srcError = "Sample not found: " + s.missing;
    }
    else { v.srcName = "NO SOURCE"; v.srcSub = "LAYER OFF"; }
    return a;
}

ImpulseAsset* Runner::buildImpulse (int layer, const ImpulseSpec& s, double rate, bool wantKt, LayerView& v)
{
    auto* a = new ImpulseAsset();
    a->rate = rate;
    v.impError = {};
    if (s.kind == Imp::live)
    {
        a->live = s.liveLayer == layer ? (layer + 1) % layers : juce::jlimit (0, layers - 1, s.liveLayer);
        v.impName = "LAYER " + juce::String (a->live + 1) + " OUT";
        v.impSub = juce::String::fromUTF8 ("LIVE \xc2\xb7 CROSS-SYNTHESIS");
        v.impPeaks.clear();
        return a;
    }
    if (s.kind == Imp::off) { v.impName = "NO IMPULSE"; v.impSub = "DRY"; v.impPeaks.clear(); return a; }

    // the raw impulse
    std::vector<float> raw[2];
    mfo_buffer_t src {};
    float root = 60.0f;
    juce::String name, sub;
    if (s.kind == Imp::synth)
    {
        mfe_patch_t p;
        if (! chonkPatch (s.patch, p)) { v.impName = "NO IMPULSE"; v.impSub = "PRESET NOT FOUND"; v.impPeaks.clear(); return a; }
        const int n = std::max (1, (int) (s.printSeconds * rate));
        raw[0].assign ((size_t) n, 0.0f);
        raw[1].assign ((size_t) n, 0.0f);
        mfo_print_synth (&p, s.printNote, 110, s.printHold, s.printSeconds, (float) rate, raw[0].data(), raw[1].data());
        src = { { raw[0].data(), raw[1].data() }, 2, n, (float) rate, (float) s.printNote };
        root = (float) s.printNote;
        name = s.patch.name.toUpperCase();
        sub = juce::String::fromUTF8 ("REGOLITH PRINT \xc2\xb7 ") + noteName ((float) s.printNote);
    }
    else if (s.from == SampleFrom::generated)
    {
        const int n = std::max (64, (int) (juce::jlimit (0.05f, 6.0f, s.genSeconds) * rate));
        raw[0].assign ((size_t) n, 0.0f);
        mfo_generate_impulse (juce::jlimit (0, MFO_GEN_COUNT - 1, s.genType), raw[0].data(), n, (float) rate, s.seed);
        src = { { raw[0].data(), nullptr }, 1, n, (float) rate, 60.0f };
        name = juce::String (mfo_generate_name (s.genType));
        sub = "FACTORY";
    }
    else if (s.sample != nullptr && s.sample->length() > 1)
    {
        const auto& d = *s.sample;
        src = { { d.ch[0].data(), d.channels > 1 ? d.ch[1].data() : nullptr }, d.channels > 1 ? 2 : 1, d.length(), (float) d.rate, s.root };
        root = s.root;
        name = d.name.upToLastOccurrenceOf (".", false, false).toUpperCase();
        sub = "SAMPLE";
    }
    else
    {
        v.impName = s.missing.isNotEmpty() ? juce::File (s.missing).getFileName().toUpperCase() : juce::String ("DROP AN IMPULSE");
        v.impSub = s.missing.isNotEmpty() ? juce::String ("MISSING FILE") : (juce::String::fromUTF8 ("WAV \xc2\xb7 AIFF \xc2\xb7 FLAC"));
        if (s.missing.isNotEmpty()) v.impError = "Impulse not found: " + s.missing;
        v.impPeaks.clear();
        return a;
    }

    // shaped at the engine rate
    mfo_ir_opts_t o;
    mfo_ir_opts_default (&o);
    o.start = s.start; o.length = s.length; o.reverse = s.reverse ? 1 : 0; o.pitch = s.pitch;
    o.fade_in = s.fadeIn; o.fade_out = s.fadeOut; o.decay_tilt = s.tilt; o.damp = s.damp; o.age = s.age;
    o.normalize = 0;
    float* out[2] = { nullptr, nullptr };
    const int len = mfo_ir_prepare (&src, (float) rate, &o, out);
    if (len <= 0) { std::free (out[0]); std::free (out[1]); v.impName = name; v.impSub = "COULD NOT SHAPE"; return a; }
    const int nch = out[1] != nullptr ? 2 : 1;
    double en = 0.0;
    float pk = 1e-9f;
    for (int c = 0; c < nch; c++)
    {
        a->d[c].assign (out[c], out[c] + len);
        std::free (out[c]);
        for (float x : a->d[c]) { en += (double) x * x; pk = std::max (pk, std::fabs (x)); }
    }
    en /= nch;
    // the stream (TONE / GRAIN / MOTION / PITCH) at a healthy level; the BODY kernel at unit energy x gain
    const float sg = 0.9f / pk;
    for (int c = 0; c < nch; c++) for (auto& x : a->d[c]) x *= sg;
    const float kg = s.gain / (float) std::sqrt (std::max (1e-20, en * (double) sg * sg));
    a->buf = { { a->d[0].data(), nch > 1 ? a->d[1].data() : nullptr }, nch, len, (float) rate, root + s.pitch };
    const float* kp[2] = { a->d[0].data(), nch > 1 ? a->d[1].data() : nullptr };
    a->kernel = mfo_kernel_create (kp, nch, len, kg);
    if (wantKt)
    {
        a->kt = mfo_keytrack_create (&a->buf, (float) rate, 36, 96, 1.0f, kg);
        a->hasKt = true;
    }
    v.impPeaks = peaksOf (a->d[0].data(), nch > 1 ? a->d[1].data() : nullptr, len, 200);
    v.impName = name;
    v.impSub = secondsText ((double) len / rate) + (wantKt ? (juce::String::fromUTF8 (" \xc2\xb7 KEYTRACK")) : (juce::String::fromUTF8 (" \xc2\xb7 ") + sub));
    return a;
}

void Runner::workerLoop()
{
    std::unique_lock<std::mutex> lk (wm);
    while (! quit)
    {
        wcv.wait_for (lk, std::chrono::milliseconds (40));
        if (quit) break;
        // what the audio thread let go
        while (retR.load (std::memory_order_relaxed) != retW.load (std::memory_order_acquire))
        {
            const uint32_t r = retR.load (std::memory_order_relaxed);
            Asset* a = retireRing[r % retireSize];
            retR.store (r + 1, std::memory_order_release);
            delete a;
        }
        if (rateStale.exchange (false))
            for (int l = 0; l < layers; l++) { srcReq[l]++; impReq[l]++; }
        // keytrack sets: built when a sample-source layer's KEYTRACK is on (they are large)
        for (int l = 0; l < layers; l++)
        {
            const bool want = core (MFO_LPARAM (l, MFO_L_KEYTRACK)) > 0.5f && wantSrc[l].kind == Src::sample
                              && (wantImp[l].kind == Imp::sample || wantImp[l].kind == Imp::synth);
            if (impDone[l] == impReq[l] && want != impBuiltKt[l]) impReq[l]++;
        }
        for (int l = 0; l < layers; l++)
        {
            if (srcReq[l] != srcDone[l])
            {
                const uint32_t req = srcReq[l];
                const SourceSpec spec = wantSrc[l];
                LayerView v = views[l];
                const double rate = fs;
                busy++;
                lk.unlock();
                SourceAsset* a;
                {
                    std::lock_guard<std::mutex> bl (buildMutex);
                    a = buildSource (spec, rate, v);
                }
                a->serial = req;
                delete pendSrc[l].exchange (a);    // one the audio thread never took is simply dropped
                lk.lock();
                srcDone[l] = std::max (srcDone[l], req);
                v.serial = views[l].serial + 1;
                views[l].srcName = v.srcName; views[l].srcSub = v.srcSub; views[l].srcPeaks = v.srcPeaks;
                views[l].srcTrace = v.srcTrace; views[l].srcError = v.srcError; views[l].serial = v.serial;
                viewCount++;
                busy--;
            }
            if (impReq[l] != impDone[l])
            {
                const uint32_t req = impReq[l];
                const ImpulseSpec spec = wantImp[l];
                const bool kt = core (MFO_LPARAM (l, MFO_L_KEYTRACK)) > 0.5f && wantSrc[l].kind == Src::sample
                                && (spec.kind == Imp::sample || spec.kind == Imp::synth);
                LayerView v = views[l];
                const double rate = fs;
                busy++;
                lk.unlock();
                ImpulseAsset* a;
                {
                    std::lock_guard<std::mutex> bl (buildMutex);
                    a = buildImpulse (l, spec, rate, kt, v);
                }
                a->serial = req;
                delete pendImp[l].exchange (a);
                lk.lock();
                impDone[l] = std::max (impDone[l], req);
                impBuiltKt[l] = kt;
                views[l].impName = v.impName; views[l].impSub = v.impSub; views[l].impPeaks = v.impPeaks;
                views[l].impError = v.impError; views[l].serial = views[l].serial + 1;
                viewCount++;
                busy--;
            }
        }
    }
}

void Runner::rebuildAllNow()
{
    // prepare(): the audio thread is stopped; rebuild every layer for the new rate and apply it
    std::unique_lock<std::mutex> lk (wm);
    SourceSpec ss[layers];
    ImpulseSpec is[layers];
    bool kt[layers];
    for (int l = 0; l < layers; l++)
    {
        ss[l] = wantSrc[l];
        is[l] = wantImp[l];
        kt[l] = core (MFO_LPARAM (l, MFO_L_KEYTRACK)) > 0.5f && ss[l].kind == Src::sample && (is[l].kind == Imp::sample || is[l].kind == Imp::synth);
    }
    LayerView vs[layers];
    for (int l = 0; l < layers; l++) vs[l] = views[l];
    uint32_t sr[layers], ir[layers];
    for (int l = 0; l < layers; l++) { sr[l] = srcReq[l]; ir[l] = impReq[l]; }
    lk.unlock();
    std::lock_guard<std::mutex> bl (buildMutex);
    for (int l = 0; l < layers; l++)
    {
        delete pendSrc[l].exchange (nullptr);
        delete pendImp[l].exchange (nullptr);
        auto* s = buildSource (ss[l], fs, vs[l]);
        auto* i = buildImpulse (l, is[l], fs, kt[l], vs[l]);
        delete actSrc[l];
        delete actImp[l];
        actSrc[l] = nullptr;
        actImp[l] = nullptr;
        applySource (l, s);
        applyImpulse (l, i);
    }
    lk.lock();
    for (int l = 0; l < layers; l++)
    {
        srcDone[l] = std::max (srcDone[l], sr[l]);
        impDone[l] = std::max (impDone[l], ir[l]);
        impBuiltKt[l] = kt[l];
        vs[l].serial = views[l].serial + 1;
        views[l] = vs[l];
    }
    viewCount++;
}

// ---------------------------------------------------------------------------------- audio side ---
void Runner::retire (Asset* a)
{
    if (a == nullptr) return;
    const uint32_t w = retW.load (std::memory_order_relaxed);
    retireRing[w % retireSize] = a;
    retW.store (w + 1, std::memory_order_release);
}

void Runner::applySource (int l, SourceAsset* a)
{
    if (eng != nullptr)
    {
        if (a->kind == Src::synth) mfo_layer_set_synth (eng, l, &a->patch);
        else if (a->kind == Src::sample) mfo_layer_set_sample (eng, l, &a->buf);
        else mfo_layer_set_source_off (eng, l);
    }
    SourceAsset* old = actSrc[l];
    actSrc[l] = a;
    retire (old);
}

void Runner::applyImpulse (int l, ImpulseAsset* a)
{
    if (eng != nullptr)
    {
        if (a->live >= 0 || a->d[0].empty())
        {
            mfo_layer_set_impulse (eng, l, nullptr, nullptr);
            mfo_layer_set_keytrack (eng, l, nullptr);
        }
        else
        {
            mfo_layer_set_impulse (eng, l, &a->buf, a->kernel);
            mfo_layer_set_keytrack (eng, l, a->kt);
        }
    }
    ImpulseAsset* old = actImp[l];
    actImp[l] = a;
    retire (old);
}

void Runner::applyPending()
{
    for (int l = 0; l < layers; l++)
    {
        // room for the asset it replaces (and one to spare) in the retire ring, else next block
        if (retW.load (std::memory_order_relaxed) - retR.load (std::memory_order_acquire) + 2 >= retireSize) return;
        if (pendSrc[l].load (std::memory_order_relaxed) != nullptr)
            if (auto* a = pendSrc[l].exchange (nullptr, std::memory_order_acq_rel)) applySource (l, a);
        if (retW.load (std::memory_order_relaxed) - retR.load (std::memory_order_acquire) + 2 >= retireSize) return;
        if (pendImp[l].load (std::memory_order_relaxed) != nullptr)
            if (auto* a = pendImp[l].exchange (nullptr, std::memory_order_acq_rel))
            {
                if (a->live < 0 && ! a->d[0].empty() && std::llround (a->rate) != std::llround (fs))
                {
                    retire (a);            // built for another rate: rebuild
                    rateStale.store (true);
                }
                else
                    applyImpulse (l, a);
            }
    }
}

void Runner::setTransport (double b, bool playing, double ppq)
{
    if (b > 1.0 && b < 1000.0) bpm = b;
    hostPlaying = playing;
    hostPpq = ppq;
}

void Runner::handleMidi (const uint8_t* b, int n)
{
    if (eng == nullptr || n < 1) return;
    const uint8_t st = b[0] & 0xF0;
    const int d1 = n > 1 ? b[1] & 127 : 0, d2 = n > 2 ? b[2] & 127 : 0;
    switch (st)
    {
        case 0x90:
            if (d2 > 0)
            {
                sustained[d1] = false;
                mfo_note_on (eng, d1, d2);
                held++;
                break;
            }
            [[fallthrough]];
        case 0x80:
            held = std::max (0, held - 1);
            if (sustain) sustained[d1] = true;
            else mfo_note_off (eng, d1);
            break;
        case 0xB0:
            if (d1 == 1) mfo_set_mod_source (eng, MFO_MS_MODWHEEL, d2 / 127.0f);
            else if (d1 == 64)
            {
                const bool on = d2 >= 64;
                if (sustain && ! on)
                    for (int k = 0; k < 128; k++)
                        if (sustained[k]) { sustained[k] = false; mfo_note_off (eng, k); }
                sustain = on;
            }
            else if (d1 == 120 || d1 == 123)
            {
                mfo_all_notes_off (eng);
                std::memset (sustained, 0, sizeof sustained);
                held = 0;
            }
            break;
        case 0xE0:
        {
            const int v = d1 | (d2 << 7);
            bendSemis = (float) (v - 8192) / 8192.0f * 2.0f;
            mfo_pitch_bend (eng, bendSemis);
            break;
        }
        case 0xD0: mfo_set_mod_source (eng, MFO_MS_AFTERTOUCH, d1 / 127.0f); break;
        default: break;
    }
}

void Runner::updateControls (double dt)
{
    // host-fed modulation sources
    static constexpr float accent[16] = { 1, 0, .5f, .3f, 1, 0, .6f, 0, .9f, .4f, 0, .7f, 1, 0, .5f, .8f };
    beat += dt * bpm / 60.0;
    const double b16 = beat * 4.0;
    const int step = (int) std::floor (b16);
    const float f16 = (float) (b16 - step);
    const int eighth = (int) std::floor (beat * 2.0);
    if (eighth != lastEighth)
    {
        lastEighth = eighth;
        rng = rng * 1664525u + 1013904223u;
        sh = (float) (rng >> 8) / 8388608.0f - 1.0f;
    }
    noteEnv += ((held > 0 ? 1.0f : 0.0f) - noteEnv) * (1.0f - std::exp ((float) -dt / (held > 0 ? 0.05f : 0.6f)));
    rng = rng * 1664525u + 1013904223u;
    walkV += ((float) (rng >> 8) / 8388608.0f - 1.0f) * (float) std::sqrt (dt) * 0.8f - walkV * (float) dt * 0.3f;
    walkV = juce::jlimit (-1.0f, 1.0f, walkV);
    walkL += (walkV - walkL) * (1.0f - std::exp ((float) -dt / 0.5f));
    ext[0] = std::sin (2.0f * 3.14159265f * (float) std::fmod (beat / 4.0, 1.0));  // EXT1: 1-bar sine
    ext[1] = accent[((step % 16) + 16) % 16] * std::exp (-f16 * 3.0f);                // EXT2: stepper 1/16 accents
    ext[2] = sh;                                                                         // EXT3: S&H 1/8
    ext[3] = bendSemis / 2.0f;                                                           // EXT4: pitch bend
    ext[4] = sustain ? 1.0f : 0.0f;                                                      // EXT5: sustain pedal
    ext[5] = noteEnv;                                                                    // EXT6: note envelope
    ext[6] = (float) (beat - std::floor (beat));                                         // EXT7: beat clock ramp
    ext[7] = walkL;                                                                      // EXT8: slow random walk
    for (int i = 0; i < 8; i++) mfo_set_mod_source (eng, MFO_MS_EXT1 + i, ext[i]);

    // parameters: the set values, the host-automated ones, then what ORBIT derives
    float v[numCore];
    for (int i = 0; i < numCore; i++) v[i] = coreVal[(size_t) i].load (std::memory_order_relaxed);
    const auto& hp = hostParams();
    float rateN = 0.35f, spreadN = 0.7f, ageN = 0.0f;
    float fxAmt[numFx] {};
    for (int i = 0; i < (int) hp.size(); i++)
    {
        const auto& h = hp[(size_t) i];
        const float x = hostNorm[i];
        if (h.map != Map::special) v[h.target] = coreFromNorm (h, x);
        else if (h.target == spRate) rateN = x;
        else if (h.target == spSpread) spreadN = x;
        else if (h.target == spTapeAge) ageN = x;
        else if (h.target >= spFx0 && h.target < spFx0 + numFx) fxAmt[h.target - spFx0] = x;
        else if (h.target >= spMacro0 && h.target < spMacro0 + numMacros) mfo_set_mod_source (eng, MFO_MS_MACRO1 + (h.target - spMacro0), x);
    }
    const bool sync = syncOn.load (std::memory_order_relaxed);
    const bool motion = hMotion < 0 || hostNorm[hMotion] > 0.5f;
    const int pm = motion ? pathMode.load (std::memory_order_relaxed) : (int) pathHold;
    const bool leg = legacy.load (std::memory_order_relaxed);
    // RATE at its minimum (free running) = stopped; 1.1.0 states keep their slow creep
    const bool stopped = ! leg && ! sync && rateN <= 0.0f;
    mfo_set_orbit_freeze (eng, stopped ? 1 : 0);
    mfo_set_held_wobble (eng, leg ? -1.0f : (hDrift >= 0 ? hostNorm[hDrift] : 0.0f));
    inGain = hTrim >= 0 ? std::exp2 (inTrimDbFromNorm (hostNorm[hTrim]) / 6.0206f) : 1.0f;
    mfo_set_input_gain (eng, inGain);
    for (int l = 0; l < layers; l++)
    {
        InState w;
        w.src = hSrcIn[l] >= 0 && hostNorm[hSrcIn[l]] > 0.5f ? 1 : 0;
        w.imp = hImpIn[l] >= 0 && hostNorm[hImpIn[l]] > 0.5f ? 1 : 0;
        w.gate = hGate[l] >= 0 && hostNorm[hGate[l]] > 0.5f ? 1 : 0;
        w.win = inWin[(size_t) l].load (std::memory_order_relaxed);
        auto& a = inApplied[l];
        if (w.src != a.src || w.imp != a.imp || w.gate != a.gate || w.win != a.win)
        {
            mfo_layer_set_input (eng, l, w.src, w.imp, w.gate, w.win);
            // the layer's own source comes back when INPUT is switched off
            if (a.src == 1 && w.src == 0 && actSrc[l] != nullptr)
            {
                if (actSrc[l]->kind == Src::synth) mfo_layer_set_synth (eng, l, &actSrc[l]->patch);
                else if (actSrc[l]->kind == Src::sample) mfo_layer_set_sample (eng, l, &actSrc[l]->buf);
            }
            a = w;
        }
    }
    v[MFO_P_ORBIT_PATH] = pm == pathCircle ? 1.0f : pm == pathFig8 ? 3.0f : 0.0f;
    v[MFO_P_ORBIT_SYNC] = sync ? (float) rateBeats (rateN) : 0.0f;
    v[MFO_P_ORBIT_RATE] = rateHz (rateN);
    v[MFO_P_ORBIT_SIZE_X] = spreadN;
    v[MFO_P_ORBIT_SIZE_Y] = spreadN * 0.64f;
    v[MFO_P_DRIVE] = std::max (v[MFO_P_DRIVE], ageN * 0.8f);
    for (int l = 0; l < layers; l++)
        v[MFO_LPARAM (l, MFO_L_MOD_SOURCE)] = actImp[l] != nullptr && actImp[l]->live >= 0 ? (float) (actImp[l]->live + 1) : 0.0f;
    if (pm == pathDrawn && drawn.n >= 2)
    {
        const double hz = stopped ? 0.0 : sync ? bpm / 60.0 / rateBeats (rateN) : rateHz (rateN);
        const double dir = v[MFO_P_ORBIT_DIR] > 0.5f ? -1.0 : 1.0;
        drawnPhase += hz * dt * dir;
        drawnPhase -= std::floor (drawnPhase);
        const float total = drawnLen[drawn.n];
        const float at = (float) drawnPhase * total;
        int k = 0;
        while (k < drawn.n - 1 && drawnLen[k + 1] < at) k++;
        const int k2 = (k + 1) % drawn.n;
        const float seg = std::max (1e-6f, drawnLen[k + 1] - drawnLen[k]);
        const float t = juce::jlimit (0.0f, 1.0f, (at - drawnLen[k]) / seg);
        float px = drawn.x[k] + (drawn.x[k2] - drawn.x[k]) * t, py = drawn.y[k] + (drawn.y[k2] - drawn.y[k]) * t;
        float cx = 0, cy = 0;
        for (int i = 0; i < drawn.n; i++) { cx += drawn.x[i]; cy += drawn.y[i]; }
        cx /= drawn.n; cy /= drawn.n;
        const float sc = spreadN / 0.7f;
        v[MFO_P_PUCK_X] = juce::jlimit (-1.0f, 1.0f, v[MFO_P_PUCK_X] + (px - cx) * sc);
        v[MFO_P_PUCK_Y] = juce::jlimit (-1.0f, 1.0f, v[MFO_P_PUCK_Y] + (py - cy) * sc);
    }
    for (int i = 0; i < numCore; i++)
        if (! appliedValid || v[i] != applied[i]) { mfo_set_param (eng, i, v[i]); applied[i] = v[i]; }
    appliedValid = true;

    // effects and tape age
    mfe_fx_t p;
    mfe_fx_defaults (&p);
    auto q = [] (float x) { return (int16_t) juce::jlimit (0, 32767, (int) std::lround (x * 32767.0f)); };
    p.fuzz_amount = fxSw.on[0] ? std::max<int16_t> (1, q (fxAmt[0])) : (int16_t) 0;
    p.tape_drive = fxSw.on[1] ? std::max<int16_t> (1, q (fxAmt[1])) : (int16_t) 0;
    p.ens_mode = fxSw.on[2] ? (uint8_t) juce::jlimit (1, 4, fxSw.ens) : (uint8_t) 0;
    p.ens_mix = std::max<int16_t> (1, q (fxAmt[2]));
    p.bbd_mix = fxSw.on[3] ? std::max<int16_t> (1, q (fxAmt[3])) : (int16_t) 0;
    p.oil_mix = fxSw.on[4] ? std::max<int16_t> (1, q (fxAmt[4])) : (int16_t) 0;
    p.rev_type = (uint8_t) juce::jlimit (0, 1, fxSw.rev);
    p.rev_amount = fxSw.on[5] ? std::max<int16_t> (1, q (fxAmt[5])) : (int16_t) 0;
    p.fx_sync = (uint8_t) ((juce::jlimit (0, 15, fxSw.bbdSync) << 4) | juce::jlimit (0, 15, fxSw.oilSync));
    fx.setParams (p);
    const float fc = 20000.0f * std::exp2 (-ageN * 2.6f);
    ageK = ageN < 0.001f ? 1.0f : 1.0f - std::exp (-2.0f * 3.14159265f * std::min (fc, 0.45f * (float) fs) / (float) fs);
}

void Runner::renderSpan (float* l, float* r, const float* il, const float* ir, int n)
{
    updateControls ((double) n / fs);
    mfo_render_input (eng, il, ir, l, r, n);
}

static inline float cleanIn (float x) { return std::isfinite (x) ? juce::jlimit (-4.0f, 4.0f, x) : 0.0f; }

void Runner::process (float* left, float* right, const float* inL, const float* inR, int n, const mfm1::MidiEvent* events, int numEvents)
{
    const auto t0 = std::chrono::steady_clock::now();
    if (eng == nullptr)
    {
        if (left) std::memset (left, 0, sizeof (float) * (size_t) n);
        if (right) std::memset (right, 0, sizeof (float) * (size_t) n);
        return;
    }
    applyPending();
    {
        DrawnPath p;
        if (drawnBuf.read (p))
        {
            drawn = p;
            drawn.n = juce::jlimit (0, maxDrawn, drawn.n);
            drawnLen[0] = 0.0f;
            for (int i = 0; i < drawn.n; i++)
            {
                const int j = (i + 1) % drawn.n;
                drawnLen[i + 1] = drawnLen[i] + std::hypot (drawn.x[j] - drawn.x[i], drawn.y[j] - drawn.y[i]);
            }
        }
        std::array<ModSlot, MFO_NMOD> m;
        if (modBuf.read (m))
            for (int s = 0; s < MFO_NMOD; s++) mfo_set_mod (eng, s, m[(size_t) s].src, m[(size_t) s].dst, m[(size_t) s].amount);
        fxBuf.read (fxSw);
    }
    if (hostPlaying) beat = hostPpq;

    // panel notes: now
    uint32_t ur = uiR.load (std::memory_order_relaxed);
    const uint32_t uw = uiW.load (std::memory_order_acquire);
    while (ur != uw) { const Msg& s = uiRing[ur % uiRingSize]; handleMidi (s.b, s.n); ur++; }
    uiR.store (ur, std::memory_order_release);

    // render in spans of at most 64 samples, split at the MIDI events
    constexpr int chunk = 1024;
    float bufL[chunk], bufR[chunk], inBL[chunk], inBR[chunk];
    const bool hasIn = inL != nullptr;
    mInCh.store (inL == nullptr ? 0 : inR == nullptr ? 1 : 2, std::memory_order_relaxed);
    int ei = 0;
    for (int base = 0; base < n; base += chunk)
    {
        const int m = std::min (chunk, n - base);
        if (hasIn)
        {
            float sl = 0, sr = 0;
            for (int i = 0; i < m; i++)
            {
                inBL[i] = cleanIn (inL[base + i]);
                inBR[i] = inR != nullptr ? cleanIn (inR[base + i]) : inBL[i];
                const float a = inBL[i] * inGain, b = inBR[i] * inGain;
                sl += a * a; sr += b * b;
                inPk = std::max (inPk, std::max (std::fabs (a), std::fabs (b)));
            }
            const float k = 1.0f - std::exp ((float) -m / (0.3f * (float) fs));
            inVuL += (std::sqrt (sl / m) - inVuL) * k;
            inVuR += (std::sqrt (sr / m) - inVuR) * k;
            inPk *= std::exp ((float) -m / (1.5f * (float) fs));
        }
        int pos = 0;
        while (pos < m)
        {
            while (ei < numEvents && std::max (0, std::min (events[ei].offset, n - 1)) <= base + pos)
            {
                const auto& e = events[ei++];
                if (e.size > 0 && e.bytes[0] >= 0x80 && e.bytes[0] < 0xF0)
                {
                    midiIn.fetch_add (1, std::memory_order_relaxed);
                    handleMidi (e.bytes, e.size);
                }
            }
            int end = std::min (m, pos + MFO_BLOCK - (int) (spanPos % MFO_BLOCK));
            if (ei < numEvents) end = std::min (end, std::max (pos + 1, std::min (events[ei].offset, n - 1) - base));
            renderSpan (bufL + pos, bufR + pos, hasIn ? inBL + pos : nullptr, hasIn ? inBR + pos : nullptr, end - pos);
            spanPos += end - pos;
            pos = end;
        }
        // tape age roll-off
        if (ageK < 1.0f)
            for (int i = 0; i < m; i++)
            {
                ageLp[0] += (bufL[i] - ageLp[0]) * ageK;
                ageLp[1] += (bufR[i] - ageLp[1]) * ageK;
                bufL[i] = ageLp[0];
                bufR[i] = ageLp[1];
            }
        fx.setTempo (bpm);
        fx.process (bufL, bufR, m);
        float sl = 0, sr = 0;
        for (int i = 0; i < m; i++)
        {
            sl += bufL[i] * bufL[i];
            sr += bufR[i] * bufR[i];
            pkL = std::max (pkL, std::fabs (bufL[i]));
            pkR = std::max (pkR, std::fabs (bufR[i]));
        }
        const float k = 1.0f - std::exp ((float) -m / (0.3f * (float) fs));
        vuL += (std::sqrt (sl / m) - vuL) * k;
        vuR += (std::sqrt (sr / m) - vuR) * k;
        pkL *= std::exp ((float) -m / (1.5f * (float) fs));
        pkR *= std::exp ((float) -m / (1.5f * (float) fs));
        if (right != nullptr)
        {
            if (left != nullptr) std::memcpy (left + base, bufL, sizeof (float) * (size_t) m);
            std::memcpy (right + base, bufR, sizeof (float) * (size_t) m);
        }
        else if (left != nullptr)
            for (int i = 0; i < m; i++) left[base + i] = 0.5f * (bufL[i] + bufR[i]);
    }
    while (ei < numEvents) { const auto& e = events[ei++]; if (e.size > 0 && e.bytes[0] >= 0x80 && e.bytes[0] < 0xF0) handleMidi (e.bytes, e.size); }

    // meters
    float px = 0, py = 0, w[4];
    mfo_orbit_position (eng, &px, &py);
    mfo_blend_weights (eng, px, py, w);
    mPx.store (px); mPy.store (py);
    for (int i = 0; i < 4; i++) mW[i].store (w[i]);
    mVuL.store (vuL); mVuR.store (vuR); mPkL.store (pkL); mPkR.store (pkR);
    mVoices.store (mfo_active_voices (eng));
    if (! hasIn) { inVuL = inVuR = inPk = 0.0f; }
    mInL.store (inVuL); mInR.store (inVuR); mInPk.store (inPk);
    const double el = std::chrono::duration<double> (std::chrono::steady_clock::now() - t0).count();
    const float load = (float) (el / ((double) n / fs));
    cpuAvg += (load - cpuAvg) * 0.05f;
    mCpu.store (cpuAvg);
}

}
