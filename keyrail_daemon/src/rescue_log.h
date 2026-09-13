#pragma once
// rescue_log.h -- a log the panic path is allowed to call.
//
// wprintf takes the CRT stdio lock and the daemon has no console anyway; the
// status strings the UI polls are std::wstring under a mutex. Neither belongs
// on a TIME_CRITICAL thread. This is a fixed ring of fixed-size slots written
// with swprintf_s: no heap, no disk, one atomic. describeRescueLog renders it
// for the pipe thread, where allocation is fine.

#include <windows.h>

#include <atomic>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <string>

namespace rescue {

constexpr uint32_t kLogSlots = 64;
constexpr uint32_t kLogSlotChars = 160;

struct LogRing {
    wchar_t slots[kLogSlots][kLogSlotChars] = {};
    uint32_t stamps[kLogSlots] = {};      // GetTickCount at write time
    std::atomic<uint32_t> head{0};        // next slot to write; total writes so far
};

LogRing& logRing();

inline void log(const wchar_t* format, ...) {
    LogRing& ring = logRing();
    const uint32_t index = ring.head.fetch_add(1, std::memory_order_acq_rel) % kLogSlots;
    va_list args;
    va_start(args, format);
    _vsnwprintf_s(ring.slots[index], kLogSlotChars, _TRUNCATE, format, args);
    va_end(args);
    ring.stamps[index] = GetTickCount();
}

// Newest last, at most `count` lines. Pipe-thread only.
std::wstring describeRescueLog(uint32_t count = 12);

} // namespace rescue
