// SPDX-License-Identifier: AGPL-3.0-only
// Atomic Lounge plugin (formerly Vintage Chonk), ORBIT mode: hosts the ORBIT core (dsp/orbit, mfo.h) inside the plugin.
//
//  * the engine is created at the host rate (prepare); its latency (STFT, mfo_latency_samples) plus the
//    effect wrapper's (FxChain) is what the plugin reports in ORBIT mode;
//  * sources and impulses (decoded samples, Chonk prints, generated impulses, shaped IRs, BODY kernels,
//    keytrack sets) are built on a worker thread, handed to the audio thread through one-slot mailboxes
//    and applied between render calls; what the audio thread lets go goes back through a retire ring
//    and is freed on the worker, never on the audio thread;
//  * render calls are split at MIDI event offsets and at least every 64 samples (the host-fed modulation
//    sources EXT1-8 / MACRO1-4 / wheel / aftertouch and the drawn orbit path update there);
//  * the live input (the sidechain bus, 1.2.0) goes in alongside: copied, cleaned (NaN / Inf -> 0, +-4 clamp),
//    trimmed in the core; layers use it as SOURCE / IMPULSE when their host flags say so (ORBIT Lx Source / Impulse
//    Input); the motion switch, puck drift and the stopped orbit (RATE 0) come from the host parameters too;
//  * after the engine: master TAPE AGE (a gentle roll-off; the saturation is the core's DRIVE) and the
//    Vintage Chonk effects (FxChain).
// The audio path never allocates, locks or waits.
#pragma once
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <thread>
#include "EngineRunner.h"   // MidiEvent
#include "FxChain.h"
#include "TripleBuffer.h"
#include "OrbitModel.h"

namespace orbit {

// built off the audio thread, immutable once posted
struct Asset
{
    virtual ~Asset() = default;
    double rate = 0.0;              // engine rate it was built for
    uint32_t serial = 0;            // request serial (newest wins)
};
struct SourceAsset final : Asset
{
    Src kind = Src::off;
    SampleRef sample;               // keeps a file's audio alive
    std::vector<float> print[2];    // a Chonk print used as the sample
    mfo_buffer_t buf {};
    mfe_patch_t patch {};
};
struct ImpulseAsset final : Asset
{
    ~ImpulseAsset() override;
    int live = -1;                  // LIVE: the layer it listens to
    std::vector<float> d[2];        // shaped impulse at the engine rate (the modulator stream)
    mfo_buffer_t buf {};
    mfo_kernel_t* kernel = nullptr; // BODY
    mfo_keytrack_t* kt = nullptr;   // per-voice BODY, built when the layer's KEYTRACK is on
    bool hasKt = false;
};

// what the panel shows of a layer's source / impulse
struct LayerView
{
    juce::String srcName, srcSub, impName, impSub;
    std::vector<float> srcPeaks, impPeaks;   // min/max pairs, 0..1 scaled (srcTrace: one line)
    bool srcTrace = false;
    juce::String srcError, impError;
    int serial = 0;
};

class Runner
{
public:
    Runner();
    ~Runner();

    // ---- message thread (audio stopped for prepare) --------------------------------------------
    void prepare (double hostRate, int maxBlock);
    bool hasEngine() const { return eng != nullptr; }
    void createEngine();                 // only while the audio thread is not running ORBIT
    int latencySamples() const;          // core STFT + effects, host samples
    double rate() const { return fs; }

    void setCore (int id, float v);      // a core parameter's set value (not the host-automated ones)
    float core (int id) const { return coreVal[(size_t) id].load (std::memory_order_relaxed); }
    void setPath (int shape, bool sync, const DrawnPath& p);   // shape = the moving path; ORBIT Motion turns it on
    void setExtras (bool legacyWobble, const float window[layers]);
    void setFx (const bool on[numFx], int ensMode, int revType, int bbdSync, int oilSync);
    void setMods (const std::array<ModSlot, MFO_NMOD>& m);
    void requestSource (int layer, const SourceSpec& s);
    void requestImpulse (int layer, const ImpulseSpec& s);
    void waitIdle (int ms);              // tests: until the worker has built everything asked

    // ---- audio thread ----------------------------------------------------------------------------
    void setHostNorm (int hostIndex, float v) { if (hostIndex >= 0 && hostIndex < 64) hostNorm[hostIndex] = v; }
    void setTransport (double bpm, bool playing, double ppq);
    // inL / inR: the sidechain input (nullptr = none; inR nullptr = mono), n frames
    void process (float* left, float* right, const float* inL, const float* inR, int n, const mfm1::MidiEvent* events, int numEvents);
    void process (float* left, float* right, int n, const mfm1::MidiEvent* events, int numEvents) { process (left, right, nullptr, nullptr, n, events, numEvents); }
    void reset();

    // any thread
    void postMidi (const uint8_t* m, int n);
    struct Meter { float cpu, puckX, puckY, w[4], vuL, vuR, peakL, peakR; unsigned voices; float inL, inR, inPk; int inCh; };
    Meter meter() const;
    LayerView view (int layer) const;
    int viewSerial() const { return viewCount.load(); }
    uint32_t midiCount() const { return midiIn.load (std::memory_order_relaxed); }
    static int coreLatencyFor (const mfo_config_t& c) { return c.spectral ? 2 * c.stft_hop : 0; }
    mfo_engine_t* engineForTests() { return eng; }

private:
    void applyPending();
    void applySource (int l, SourceAsset* a);
    void applyImpulse (int l, ImpulseAsset* a);
    void retire (Asset* a);
    void updateControls (double dt);
    void renderSpan (float* l, float* r, const float* il, const float* ir, int n);
    void handleMidi (const uint8_t* b, int n);

    // worker
    void workerLoop();
    SourceAsset* buildSource (const SourceSpec& s, double rate, LayerView& v);
    ImpulseAsset* buildImpulse (int layer, const ImpulseSpec& s, double rate, bool kt, LayerView& v);
    void rebuildAllNow();                // prepare: everything for the new rate, on this thread

    mfo_engine_t* eng = nullptr;
    mfo_config_t cfg {};
    double fs = 48000.0;
    int maxBlock = 512;
    mfm1::FxChain fx;

    // assets: mailboxes (worker -> audio), active (audio), retire ring (audio -> worker)
    std::atomic<SourceAsset*> pendSrc[layers] {};
    std::atomic<ImpulseAsset*> pendImp[layers] {};
    SourceAsset* actSrc[layers] {};
    ImpulseAsset* actImp[layers] {};
    static constexpr uint32_t retireSize = 64;
    Asset* retireRing[retireSize] {};
    std::atomic<uint32_t> retW { 0 }, retR { 0 };

    // worker state
    std::thread worker;
    std::mutex wm;                       // guards the request specs and the views
    std::condition_variable wcv;
    bool quit = false;
    SourceSpec wantSrc[layers];
    ImpulseSpec wantImp[layers];
    uint32_t srcReq[layers] {}, impReq[layers] {}, srcDone[layers] {}, impDone[layers] {};
    bool impBuiltKt[layers] {};
    std::mutex buildMutex;               // one build at a time (worker or prepare)
    LayerView views[layers];
    std::atomic<int> viewCount { 0 };
    std::atomic<int> busy { 0 };
    std::atomic<bool> rateStale { false };

    // parameters
    std::array<std::atomic<float>, numCore> coreVal;
    float applied[numCore] {};
    bool appliedValid = false;
    float hostNorm[64] {};
    std::atomic<int> pathMode { pathCircle };     // the shape
    std::atomic<bool> legacy { false };
    std::array<std::atomic<float>, layers> inWin {};
    int hMotion = -1, hDrift = -1, hTrim = -1, hSrcIn[layers] {}, hImpIn[layers] {}, hGate[layers] {};
    struct InState { int src = -1, imp = -1, gate = -1; float win = -1.0f; } inApplied[layers];
    std::atomic<bool> syncOn { false };
    mfm1::TripleBuffer<DrawnPath> drawnBuf;
    mfm1::TripleBuffer<std::array<ModSlot, MFO_NMOD>> modBuf;
    struct FxSwitch { bool on[numFx]; int ens, rev, bbdSync, oilSync; };
    mfm1::TripleBuffer<FxSwitch> fxBuf;
    FxSwitch fxSw {};
    DrawnPath drawn {};
    float drawnLen[maxDrawn + 1] {};
    double drawnPhase = 0.0;
    int64_t spanPos = 0;            // samples rendered since reset (spans keep to the core's 64-sample grid)

    // performance state (audio thread)
    double bpm = 120.0, beat = 0.0;
    bool hostPlaying = false;
    double hostPpq = 0.0;
    bool sustain = false;
    bool sustained[128] {};
    int held = 0;
    float bendSemis = 0.0f;
    float ext[8] {};
    float noteEnv = 0.0f, walkV = 0.0f, walkL = 0.0f;
    uint32_t rng = 0x5eed1234u;
    int lastStep = -1;
    float stepGate = 0.0f, sh = 0.0f;
    int lastEighth = -1;
    float ageLp[2] {}, ageK = 1.0f;

    // panel -> audio notes
    struct Msg { uint8_t b[3]; uint8_t n; };
    static constexpr uint32_t uiRingSize = 1024;
    Msg uiRing[uiRingSize];
    std::atomic<uint32_t> uiW { 0 }, uiR { 0 };

    // meters
    std::atomic<float> mCpu { 0 }, mPx { 0 }, mPy { 0 }, mW[4] {}, mVuL { 0 }, mVuR { 0 }, mPkL { 0 }, mPkR { 0 };
    std::atomic<unsigned> mVoices { 0 };
    std::atomic<float> mInL { 0 }, mInR { 0 }, mInPk { 0 };
    std::atomic<int> mInCh { 0 };
    float inVuL = 0, inVuR = 0, inPk = 0, inGain = 1.0f;
    std::atomic<uint32_t> midiIn { 0 };
    float vuL = 0, vuR = 0, pkL = 0, pkR = 0, cpuAvg = 0;
};

}
