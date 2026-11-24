// SPDX-License-Identifier: AGPL-3.0-only
// Vintage Chonk plugin, ORBIT mode: the Vintage Chonk master effects (dsp/effects/mfe_fx.c: OCT FUZZ, TAPE,
// ENSEMBLE, BBD ECHO, OIL CAN, TWIN TANK / SPRING) on a host-rate float bus.
//
// mfe_fx is not rate-agnostic (Q15 integer audio, every time constant and delay computed for 44.1 kHz:
// HZ(), CHROMA_SR = MFE_SR), so it runs at 44.1 kHz between two windowed-sinc resamplers (the same Kaiser
// design as EngineRunner), in 16-frame blocks (the Chroma sub-block).
//
// Timing (deterministic, independent of the host block size): effect input sample j (44.1 kHz) reads the
// bus at host time j * fs / 44100; host output sample m reads the effect output at effect time
// (m - L) * 44100 / fs. L (host samples, reported as part of the latency) is the smallest delay for which
// everything is already known when sample m is produced, sample by sample:
//   L = ceil((H2 + B + 1) * fs / 44100) + H1 + 2   (H1 / H2: half-widths of the two kernels, B = 16)
// At exactly 44.1 kHz both resamplers are bypassed and L = B.
// With every effect off the bus passes the resamplers only (float, no Q15 rounding, no clipping).
#pragma once
#include <atomic>
#include <cstdint>
#include <vector>
extern "C" {
#include "mfe.h"
typedef struct mfe_fx_state mfe_fx_state_t;
size_t mfe_fx_size (void);
void mfe_fx_init (mfe_fx_state_t* f);
void mfe_fx_set_tempo (mfe_fx_state_t* f, uint32_t bpm_q8);
void mfe_fx_process (mfe_fx_state_t* f, const mfe_fx_t* p, int32_t* lr, unsigned frames);
}

namespace mfm1 {

// windowed-sinc polyphase kernel (Kaiser beta 9), as EngineRunner: rows of taps() coefficients per phase
struct SincKernel
{
    static constexpr int phases = 512;
    int halfWidth = 0, taps = 0;
    std::vector<float> table;
    void design (double inRate, double outRate);   // cut-off 0.92 x the lower Nyquist
};

class FxChain
{
public:
    static constexpr int fxRate = MFE_SR;
    static constexpr int block = 16;

    FxChain();
    ~FxChain();
    void prepare (double hostRate);                 // allocates
    int latencySamples() const { return latency; }  // host samples
    void reset();

    // audio thread
    void setTempo (double bpm);
    void setParams (const mfe_fx_t& p) { params = p; }
    void process (float* l, float* r, int n);       // in place, host rate

    static bool anyOn (const mfe_fx_t& p);

private:
    void runBlock();

    mfe_fx_state_t* st = nullptr;
    mfe_fx_t params {};
    double fs = 44100.0;
    int64_t fsInt = 44100;
    bool bypass = true;
    SincKernel down, up;
    int latency = block;

    static constexpr int ringSize = 8192;           // host and effect rings (power of two)
    std::vector<float> hostRing, fxRing;            // stereo interleaved
    int64_t hostW = 0;                               // host samples written
    int64_t fxIn = 0, fxOut = 0;                     // effect samples consumed / produced
    float blk[2 * block];
    int32_t q[2 * block];
    bool wasOn = false;
};

}
