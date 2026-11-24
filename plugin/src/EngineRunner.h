// SPDX-License-Identifier: AGPL-3.0-only
// Vintage Chonk plugin: runs the MFE engine (fixed 44.1 kHz, 128-frame blocks) inside a host running at
// any sample rate and block size. No JUCE here, so the tests can drive it directly.
//
// Timing model (deterministic: the output depends only on event times, never on the host block size):
//  * output sample m (counted from reset) reads the engine at input position x = m * 44100 / fs - L,
//    L = D + H input samples: D = 64 (half an engine block of look-ahead, see below), H = the
//    resampler's half-width (0 at exactly 44.1 kHz, where the engine output is copied).
//  * engine blocks are rendered on demand as the resampler needs input.
//  * a MIDI event at host sample m has engine time t = m * 44100 / fs. It is applied at the start of
//    the engine block whose start is nearest to t (error at most +-64 engine samples = 1.45 ms); because
//    of D, every such event is already known when that block is rendered.
//  * host latency = L * fs / 44100 samples (reported to the host for delay compensation).
//
// Threads: process() runs on the audio thread and never allocates or locks. Patches arrive through a
// two-slot mailbox and notes from the panel through a lock-free single-producer ring, as on the tablet.
#pragma once
#include <atomic>
#include <cstdint>
#include <vector>
extern "C" {
#include "mfe.h"
}

namespace mfm1 {

struct MidiEvent
{
    int offset;          // sample offset inside the host block
    uint8_t bytes[3];
    uint8_t size;
};

class EngineRunner
{
public:
    static constexpr int engineRate = MFE_SR;
    static constexpr int lookAhead = MFE_BLOCK / 2;     // D

    EngineRunner();
    ~EngineRunner();

    // not real-time: allocates. Any rate from 8 kHz to 384 kHz.
    void prepare (double hostRate, int maxBlock);
    int latencySamples() const { return latency; }      // at the host rate
    double hostRate() const { return fsHost; }

    // audio thread ------------------------------------------------------------------------------
    // events must be sorted by offset. left/right may alias nothing; n may be any size.
    void process (float* left, float* right, int n, const MidiEvent* events, int numEvents);
    void reset();                                        // all notes off, silence, time back to 0
    void setTempo (double bpm);                          // host tempo, applied at the next engine block

    // Called by the audio thread before each engine block with the patch it is about to use; return
    // true if the hook changed it (host parameters). May be empty.
    using PatchHook = bool (*) (void* user, mfe_patch_t& patch, bool patchIsNew);
    void setPatchHook (PatchHook hook, void* user) { patchHook = hook; hookUser = user; }

    // any thread (one producer) ------------------------------------------------------------------
    void postPatch (const mfe_patch_t& p);               // triple buffer (copied)
    void postMidi (const uint8_t* msg, int n);           // panel notes etc., played at the next engine block

    // resampler kernel, for tests: (phases + 1) rows of taps() coefficients; empty at 44.1 kHz
    const std::vector<float>& kernel() const { return table; }
    int kernelTaps() const { return taps; }
    int kernelHalfWidth() const { return halfWidth; }
    static constexpr int kernelPhases() { return phases; }

    // statistics (any thread)
    unsigned activeVoices() const { return voices.load (std::memory_order_relaxed); }
    uint32_t midiCount() const { return midiIn.load (std::memory_order_relaxed); }
    uint32_t droppedEvents() const { return dropped.load (std::memory_order_relaxed); }

private:
    void renderEngineBlock();
    void applyDueEvents (int64_t blockStart);
    float tapAt (int64_t idx, int ch) const { return ring[(size_t) (idx & ringMask) * 2 + (size_t) ch]; }

    mfe_engine_t* eng = nullptr;
    mfe_patch_t enginePatch {};
    bool havePatch = false;

    // resampler
    double fsHost = 44100.0;
    int64_t fsInt = 44100;
    bool bypass = true;          // exactly 44.1 kHz: copy
    int halfWidth = 0;           // H in input samples
    int taps = 0;                // 2 H
    static constexpr int phases = 512;
    std::vector<float> table;    // (phases + 1) x taps
    int latency = 0;

    // engine output ring (stereo float at 44.1 kHz)
    std::vector<float> ring;
    int64_t ringMask = 0;
    int64_t rendered = 0;        // input samples rendered so far
    int64_t outPos = 0;          // output samples produced since reset
    int32_t blockBuf[MFE_BLOCK * 2];

    // events waiting for their engine block: (host sample position, bytes)
    struct Pending { int64_t at; uint8_t b[3]; uint8_t n; };
    std::vector<Pending> pending;  // fixed capacity, used as a FIFO
    size_t pendHead = 0, pendCount = 0;

    // panel -> audio ring
    struct Msg { uint8_t b[3]; uint8_t n; };
    static constexpr uint32_t uiRingSize = 1024;
    Msg uiRing[uiRingSize];
    std::atomic<uint32_t> uiW { 0 }, uiR { 0 };

    // patch hand-over: a triple buffer (the tablet's two-slot mailbox, made race-free when the writer
    // posts twice while the audio thread copies). Writer owns slot `back`, audio owns `front`, the
    // third waits in `middle` (bit 4 = new).
    mfe_patch_t slots[3];
    int back = 0, front = 1;
    std::atomic<int> middle { 2 };

    std::atomic<uint32_t> tempoQ8 { 120u << 8 };
    uint32_t appliedTempo = 0;

    PatchHook patchHook = nullptr;
    void* hookUser = nullptr;

    std::atomic<unsigned> voices { 0 };
    std::atomic<uint32_t> midiIn { 0 }, dropped { 0 };
};

// engine tables are global to the process: build them once
void initTablesOnce();

}
