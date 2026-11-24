// SPDX-License-Identifier: AGPL-3.0-only
// Vintage Chonk plugin: engine host (see EngineRunner.h for the timing model).
#include "EngineRunner.h"
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <mutex>

namespace mfm1 {

void initTablesOnce()
{
    static std::once_flag once;
    std::call_once (once, [] { mfe_init_tables(); });
}

EngineRunner::EngineRunner()
{
    initTablesOnce();
    eng = static_cast<mfe_engine_t*> (std::calloc (1, mfe_engine_size()));
    mfe_engine_init (eng);
    mfe_patch_init (&enginePatch);
    for (auto& s : slots) mfe_patch_init (&s);
    pending.resize (4096);
    prepare (44100.0, 512);
}

EngineRunner::~EngineRunner() { std::free (eng); }

// Kaiser window, I0 by its series
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

void EngineRunner::prepare (double rate, int /*maxBlock*/)
{
    fsHost = rate > 1000.0 ? rate : 44100.0;
    fsInt = (int64_t) std::llround (fsHost);
    bypass = fsInt == engineRate;
    if (bypass)
    {
        halfWidth = 0;
        taps = 0;
        table.clear();
    }
    else
    {
        // windowed sinc: 64 taps when upsampling (20.3 kHz -6 dB point, about 90 dB stop band), widened
        // in proportion when the host runs below 44.1 kHz so the cut-off follows the host's Nyquist
        const double r = std::min (1.0, (double) fsInt / engineRate);
        halfWidth = (int) std::ceil (32.0 / r);
        taps = 2 * halfWidth;
        const double c = 0.92 * r;            // cut-off as a fraction of the engine's Nyquist
        const double beta = 9.0;
        const double i0b = besselI0 (beta);
        table.assign ((size_t) (phases + 1) * (size_t) taps, 0.0f);
        for (int p = 0; p <= phases; p++)
        {
            const double frac = (double) p / phases;
            double sum = 0.0;
            std::vector<double> h ((size_t) taps);
            for (int k = 0; k < taps; k++)
            {
                const double t = (double) (k - halfWidth + 1) - frac;
                const double u = t / halfWidth;
                double w = 0.0;
                if (std::fabs (u) < 1.0) w = besselI0 (beta * std::sqrt (1.0 - u * u)) / i0b;
                const double xx = 3.14159265358979323846 * c * t;
                const double s = std::fabs (xx) < 1e-9 ? 1.0 : std::sin (xx) / xx;
                h[(size_t) k] = c * s * w;
                sum += h[(size_t) k];
            }
            for (int k = 0; k < taps; k++) table[(size_t) p * (size_t) taps + (size_t) k] = (float) (h[(size_t) k] / sum);
        }
    }
    const int64_t L = lookAhead + halfWidth;                         // input samples
    latency = (int) std::llround ((double) L * (double) fsInt / engineRate);
    int64_t need = 4 * (taps + 2 * MFE_BLOCK + 64), size = 1024;
    while (size < need) size <<= 1;
    ring.assign ((size_t) size * 2, 0.0f);
    ringMask = size - 1;
    reset();
}

void EngineRunner::reset()
{
    // full engine re-init: no allocation (memset of the engine state), output depends only on what follows
    mfe_engine_init (eng);
    if (havePatch)
    {
        mfe_patch_t tmp = enginePatch;
        if (tmp.max_voices == 8) tmp.max_voices = MFE_MAX_VOICES;
        mfe_engine_set_patch (eng, &tmp);
    }
    appliedTempo = 0;
    std::fill (ring.begin(), ring.end(), 0.0f);
    rendered = 0;
    outPos = 0;
    pendHead = pendCount = 0;
    voices.store (0, std::memory_order_relaxed);
}

void EngineRunner::setTempo (double bpm)
{
    if (! (bpm > 1.0 && bpm < 1000.0)) return;
    tempoQ8.store ((uint32_t) std::lround (bpm * 256.0), std::memory_order_relaxed);
}

void EngineRunner::postPatch (const mfe_patch_t& p)
{
    slots[back] = p;
    back = middle.exchange (back | 4, std::memory_order_acq_rel) & 3;
}

void EngineRunner::postMidi (const uint8_t* m, int n)
{
    const uint32_t w = uiW.load (std::memory_order_relaxed);
    const uint32_t r = uiR.load (std::memory_order_acquire);
    if (w - r >= uiRingSize) { dropped.fetch_add (1, std::memory_order_relaxed); return; }
    Msg& s = uiRing[w % uiRingSize];
    s.n = (uint8_t) (n > 3 ? 3 : n);
    std::memcpy (s.b, m, s.n);
    uiW.store (w + 1, std::memory_order_release);
}

void EngineRunner::applyDueEvents (int64_t blockStart)
{
    // due: engine time at * 44100 / fs is nearer to this block's start than to the next one's
    const int64_t limit = (blockStart + lookAhead) * fsInt;
    while (pendCount > 0)
    {
        const Pending& e = pending[pendHead];
        if (e.at * (int64_t) engineRate >= limit) break;
        mfe_midi (eng, e.b, e.n);
        pendHead = (pendHead + 1) % pending.size();
        pendCount--;
    }
}

void EngineRunner::renderEngineBlock()
{
    // patch: a new one from the editor/state, then the host parameters on top
    bool isNew = false;
    if (middle.load (std::memory_order_acquire) & 4)
    {
        front = middle.exchange (front, std::memory_order_acq_rel) & 3;
        enginePatch = slots[front];
        isNew = true;
        havePatch = true;
    }
    bool changed = isNew;
    if (patchHook != nullptr && havePatch) changed = patchHook (hookUser, enginePatch, isNew) || changed;
    if (changed)
    {
        mfe_patch_t tmp = enginePatch;
        // the desktop has 16 voices; presets made for the FM1 carry its 8, meaning "all of them" (as the tablet)
        if (tmp.max_voices == 8) tmp.max_voices = MFE_MAX_VOICES;
        mfe_engine_set_patch (eng, &tmp);
    }
    const uint32_t t = tempoQ8.load (std::memory_order_relaxed);
    if (t != appliedTempo) { mfe_set_tempo (eng, t); appliedTempo = t; }

    // panel notes: now; host MIDI: at their block
    uint32_t r = uiR.load (std::memory_order_relaxed);
    const uint32_t w = uiW.load (std::memory_order_acquire);
    while (r != w)
    {
        const Msg& s = uiRing[r % uiRingSize];
        mfe_midi (eng, s.b, s.n);
        r++;
    }
    uiR.store (r, std::memory_order_release);
    applyDueEvents (rendered);

    mfe_render (eng, blockBuf);
    constexpr float k = 1.0f / 32768.0f;
    for (int i = 0; i < MFE_BLOCK; i++)
    {
        const size_t at = (size_t) ((rendered + i) & ringMask) * 2;
        ring[at] = (float) blockBuf[2 * i] * k;
        ring[at + 1] = (float) blockBuf[2 * i + 1] * k;
    }
    rendered += MFE_BLOCK;
    voices.store (mfe_active_voices (eng), std::memory_order_relaxed);
}

static inline int64_t floorDiv (int64_t a, int64_t b)
{
    int64_t q = a / b;
    if ((a % b != 0) && ((a < 0) != (b < 0))) q--;
    return q;
}

void EngineRunner::process (float* left, float* right, int n, const MidiEvent* events, int numEvents)
{
    for (int i = 0; i < numEvents; i++)
    {
        const MidiEvent& ev = events[i];
        if (ev.size == 0) continue;
        const uint8_t st = ev.bytes[0];
        if (st < 0x80 || st >= 0xF0) continue;       // system / real-time / running status: not for the engine
        midiIn.fetch_add (1, std::memory_order_relaxed);
        if (pendCount == pending.size()) { dropped.fetch_add (1, std::memory_order_relaxed); continue; }
        Pending& p = pending[(pendHead + pendCount) % pending.size()];
        p.at = outPos + std::max (0, std::min (ev.offset, n - 1));
        std::memcpy (p.b, ev.bytes, 3);
        p.n = ev.size > 3 ? 3 : ev.size;
        pendCount++;
    }

    const int64_t L = lookAhead + halfWidth;
    for (int s = 0; s < n; s++, outPos++)
    {
        const int64_t num = outPos * (int64_t) engineRate - L * fsInt;  // input position, in 1/fs units
        const int64_t idx = floorDiv (num, fsInt);
        const int64_t needed = idx + halfWidth;
        while (rendered <= needed) renderEngineBlock();
        float l = 0.0f, r = 0.0f;
        if (bypass)
        {
            if (idx >= 0) { l = tapAt (idx, 0); r = tapAt (idx, 1); }
        }
        else
        {
            const double pf = (double) (num - idx * fsInt) / (double) fsInt * phases;
            int p = (int) pf;
            if (p >= phases) p = phases - 1;
            const float a = (float) (pf - p);
            const float* c0 = &table[(size_t) p * (size_t) taps];
            const float* c1 = c0 + taps;
            const int64_t j0 = idx - halfWidth + 1;
            for (int k = 0; k < taps; k++)
            {
                const int64_t j = j0 + k;
                if (j < 0) continue;
                const float c = c0[k] + a * (c1[k] - c0[k]);
                l += c * tapAt (j, 0);
                r += c * tapAt (j, 1);
            }
        }
        if (right != nullptr) { right[s] = r; if (left != nullptr) left[s] = l; }
        else if (left != nullptr) left[s] = 0.5f * (l + r);     // mono output
    }
}

}
