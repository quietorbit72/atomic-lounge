// SPDX-License-Identifier: AGPL-3.0-only
// Vintage Chonk plugin: a lock-free triple buffer (one writer thread, one reader thread), the pattern of
// EngineRunner's patch hand-over. The reader always gets the newest complete value; nothing allocates.
#pragma once
#include <atomic>

namespace mfm1 {

template <typename T>
class TripleBuffer
{
public:
    void write (const T& v)              // writer thread
    {
        slots[back] = v;
        back = middle.exchange (back | 4, std::memory_order_acq_rel) & 3;
    }
    bool read (T& out)                   // reader thread: true when a new value arrived
    {
        if (! (middle.load (std::memory_order_acquire) & 4)) return false;
        front = middle.exchange (front, std::memory_order_acq_rel) & 3;
        out = slots[front];
        return true;
    }

private:
    T slots[3] {};
    int back = 0, front = 1;
    std::atomic<int> middle { 2 };
};

}
