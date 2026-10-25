#pragma once
// perf_watch.h -- find out what stalls, after the fact.
//
// A hitch that happens once every twenty minutes, mid-game, cannot be caught
// by watching a terminal. So the daemon times its own periodic work and keeps
// the slow ones: in memory for the status pipe, and appended to
// %APPDATA%\KeyRail\perf.log so the evidence survives a restart.
//
// The heartbeat is the other half. A thread sleeps in a tight cadence and
// records when it is woken late: if the machine freezes for five seconds and
// no KeyRail operation was slow, the freeze was not ours, and the log says so
// instead of leaving it to guesswork.

#include <windows.h>

#include <cstdint>
#include <string>

namespace perf {

// Records an operation that took longer than its threshold.
void note(const wchar_t* what, uint32_t millis);

// Times a scope; logs on destruction when it ran over `thresholdMs`.
class Scope {
public:
    Scope(const wchar_t* what, uint32_t thresholdMs = 250);
    ~Scope();
    Scope(const Scope&) = delete;
    Scope& operator=(const Scope&) = delete;

private:
    const wchar_t* what_;
    uint32_t thresholdMs_;
    int64_t start_;
};

void startHeartbeat();
void stopHeartbeat();

// Every timed scope is also counted, so the status can say where the daemon's
// steady CPU goes - a spike log alone cannot explain an operation that costs
// 100 ms every second and never crosses the slow threshold.
void tally(const wchar_t* what, uint32_t millis);

// Recent slow events for the daemon status, newest last. Empty when nothing
// has been slow since startup.
std::wstring describe();

} // namespace perf
