// SPDX-License-Identifier: AGPL-3.0-only
// Vintage Chonk plugin, ORBIT mode: the master effects at 44.1 kHz on a host-rate bus (see FxChain.h).
#include "FxChain.h"
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>

namespace mfm1 {

static double besselI0 (double x)
{
    double sum = 1.0, term = 1.0, q = x * x / 4.0;
    for (int k = 1; k < 64; k++)
    {
        term *= q / ((double) k * k);
        sum += term;
        if (term < 1e-12 * sum) break;
    }
    return sum;
}

void SincKernel::design (double inRate, double outRate)
{
    const double r = std::min (1.0, outRate / inRate);
    halfWidth = (int) std::ceil (32.0 / r);
    taps = 2 * halfWidth;
    const double c = 0.92 * r, beta = 9.0, i0b = besselI0 (beta);
    table.assign ((size_t) (phases + 1) * (size_t) taps, 0.0f);
    std::vector<double> h ((size_t) taps);
    for (int p = 0; p <= phases; p++)
    {
        const double frac = (double) p / phases;
        double sum = 0.0;
        for (int k = 0; k < taps; k++)
        {
            const double t = (double) (k - halfWidth + 1) - frac, u = t / halfWidth;
            const double w = std::fabs (u) < 1.0 ? besselI0 (beta * std::sqrt (1.0 - u * u)) / i0b : 0.0;
            const double xx = 3.14159265358979323846 * c * t;
            h[(size_t) k] = c * (std::fabs (xx) < 1e-9 ? 1.0 : std::sin (xx) / xx) * w;
            sum += h[(size_t) k];
        }
        for (int k = 0; k < taps; k++) table[(size_t) p * (size_t) taps + (size_t) k] = (float) (h[(size_t) k] / sum);
    }
}

static inline int64_t floorDiv (int64_t a, int64_t b)
{
    int64_t q = a / b;
    if ((a % b != 0) && ((a < 0) != (b < 0))) q--;
    return q;
}

FxChain::FxChain()
{
    st = static_cast<mfe_fx_state_t*> (std::calloc (1, mfe_fx_size()));
    mfe_fx_init (st);
    mfe_fx_defaults (&params);
    hostRing.assign ((size_t) ringSize * 2, 0.0f);
    fxRing.assign ((size_t) ringSize * 2, 0.0f);
    prepare (44100.0);
}

FxChain::~FxChain() { std::free (st); }

bool FxChain::anyOn (const mfe_fx_t& p)
{
    const bool ens = p.ens_mode == MFE_ENS_VIBRATO || (p.ens_mode >= MFE_ENS_I && p.ens_mode <= MFE_ENS_I_II && p.ens_mix > 0);
    return p.fuzz_amount > 0 || p.tape_drive > 0 || ens || p.bbd_mix > 0 || p.oil_mix > 0 || p.rev_amount > 0;
}

void FxChain::prepare (double hostRate)
{
    fs = hostRate > 1000.0 ? hostRate : 44100.0;
    fsInt = (int64_t) std::llround (fs);
    bypass = fsInt == fxRate;
    if (bypass)
        latency = block;
    else
    {
        down.design ((double) fsInt, fxRate);
        up.design (fxRate, (double) fsInt);
        latency = (int) std::ceil ((double) (up.halfWidth + block + 1) * (double) fsInt / fxRate) + down.halfWidth + 2;
    }
    reset();
}

void FxChain::reset()
{
    const uint32_t tempo = 120u << 8;
    mfe_fx_init (st);
    mfe_fx_set_tempo (st, tempo);
    std::fill (hostRing.begin(), hostRing.end(), 0.0f);
    std::fill (fxRing.begin(), fxRing.end(), 0.0f);
    hostW = fxIn = fxOut = 0;
    wasOn = false;
}

void FxChain::setTempo (double bpm)
{
    if (bpm > 1.0 && bpm < 1000.0) mfe_fx_set_tempo (st, (uint32_t) std::lround (bpm * 256.0));
}

void FxChain::runBlock()
{
    constexpr int64_t mask = ringSize - 1;
    for (int i = 0; i < block; i++, fxIn++)
    {
        float l = 0.0f, r = 0.0f;
        if (bypass)
        {
            if (fxIn >= 0) { l = hostRing[(size_t) (fxIn & mask) * 2]; r = hostRing[(size_t) (fxIn & mask) * 2 + 1]; }
        }
        else
        {
            const int64_t num = fxIn * fsInt;                      // host position in 1/44100 host samples
            const int64_t idx = floorDiv (num, fxRate);
            const double pf = (double) (num - idx * fxRate) / fxRate * SincKernel::phases;
            int p = (int) pf;
            if (p >= SincKernel::phases) p = SincKernel::phases - 1;
            const float a = (float) (pf - p);
            const float* c0 = &down.table[(size_t) p * (size_t) down.taps];
            const float* c1 = c0 + down.taps;
            const int64_t j0 = idx - down.halfWidth + 1;
            for (int k = 0; k < down.taps; k++)
            {
                const int64_t j = j0 + k;
                if (j < 0) continue;
                const float c = c0[k] + a * (c1[k] - c0[k]);
                l += c * hostRing[(size_t) (j & mask) * 2];
                r += c * hostRing[(size_t) (j & mask) * 2 + 1];
            }
        }
        blk[2 * i] = l;
        blk[2 * i + 1] = r;
    }
    const bool on = anyOn (params);
    if (on)
    {
        // Q15 in int32 (the chain's own format; the Chroma effects clamp to 16 bits inside)
        for (int i = 0; i < 2 * block; i++)
        {
            const float v = std::max (-256.0f, std::min (256.0f, blk[i]));
            q[i] = (int32_t) std::lrint (v * 32768.0f);
        }
        mfe_fx_process (st, &params, q, (unsigned) block);
        for (int i = 0; i < 2 * block; i++) blk[i] = (float) q[i] * (1.0f / 32768.0f);
    }
    else if (wasOn)
        mfe_fx_process (st, &params, q, 0);   // lets every effect see "off" (its state is cleared when it comes back)
    wasOn = on;
    for (int i = 0; i < block; i++)
    {
        const size_t at = (size_t) ((fxOut + i) & mask) * 2;
        fxRing[at] = blk[2 * i];
        fxRing[at + 1] = blk[2 * i + 1];
    }
    fxOut += block;
}

void FxChain::process (float* l, float* r, int n)
{
    constexpr int64_t mask = ringSize - 1;
    for (int s = 0; s < n; s++)
    {
        const size_t w = (size_t) (hostW & mask) * 2;
        hostRing[w] = l[s];
        hostRing[w + 1] = r[s];
        const int64_t m = hostW++;
        float ol = 0.0f, orr = 0.0f;
        if (bypass)
        {
            const int64_t idx = m - latency;
            while (fxOut <= idx) runBlock();
            if (idx >= 0) { ol = fxRing[(size_t) (idx & mask) * 2]; orr = fxRing[(size_t) (idx & mask) * 2 + 1]; }
        }
        else
        {
            const int64_t num = (m - latency) * (int64_t) fxRate;   // effect position in 1/fs effect samples
            const int64_t idx = floorDiv (num, fsInt);
            while (fxOut <= idx + up.halfWidth) runBlock();
            const double pf = (double) (num - idx * fsInt) / (double) fsInt * SincKernel::phases;
            int p = (int) pf;
            if (p >= SincKernel::phases) p = SincKernel::phases - 1;
            const float a = (float) (pf - p);
            const float* c0 = &up.table[(size_t) p * (size_t) up.taps];
            const float* c1 = c0 + up.taps;
            const int64_t j0 = idx - up.halfWidth + 1;
            for (int k = 0; k < up.taps; k++)
            {
                const int64_t j = j0 + k;
                if (j < 0) continue;
                const float c = c0[k] + a * (c1[k] - c0[k]);
                ol += c * fxRing[(size_t) (j & mask) * 2];
                orr += c * fxRing[(size_t) (j & mask) * 2 + 1];
            }
        }
        l[s] = ol;
        r[s] = orr;
    }
}

}
