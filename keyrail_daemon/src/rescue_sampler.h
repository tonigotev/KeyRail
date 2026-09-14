#pragma once
// rescue_sampler.h -- Tier 0 of the rescue menu: everything that can be known
// about every process from one NtQuerySystemInformation call, kept warm on a
// dedicated thread so the panic path only ever reads a finished table.
//
// Memory is allocated and VirtualLock'd once at start. After that the sampler
// never allocates: each tick rewrites one of three fixed snapshots and flips
// an atomic index to publish it. Readers pin a snapshot with snapshotAcquire
// and get a table that is already scored and ranked.

#include "config.h"

#include <windows.h>

#include <cstdint>
#include <string>

namespace rescue {

constexpr uint32_t kMaxRecords = 4096;
constexpr uint32_t kPidIndexSize = 8192;   // power of two, >= 2x kMaxRecords
constexpr uint32_t kImageChars = 24;
constexpr uint32_t kTitleChars = 48;
constexpr uint32_t kHistory = 8;

enum RecordFlags : uint16_t {
    kFlagOwnsWindow = 1 << 0,    // has a visible top-level window
    kFlagHungWindow = 1 << 1,    // IsHungAppWindow said so (the authoritative signal)
    kFlagHungByState = 1 << 2,   // retired: never set (see the sampler for why); kept so the bit stays reserved
    kFlagPaging = 1 << 3,        // threads blocked on page-in: a thrash victim
    kFlagNew = 1 << 4,           // first tick seen, deltas not yet meaningful
    kFlagSelf = 1 << 5,          // this daemon
    kFlagRealtime = 1 << 6,      // REALTIME_PRIORITY_CLASS: can starve everything
    kFlagIoStorm = 1 << 7,       // disk traffic far above everyone else
    kFlagSpawnStorm = 1 << 8,    // creating children by the dozen per second
};

// Why the sampler is sure a process is freezing the machine, if it is.
enum class Culprit : uint8_t {
    None,
    MemoryHog,       // owns the hard-fault storm and a large slice of RAM
    RealtimeSpinner, // realtime priority with cores pegged
    IoStorm,         // saturating the disk while others hard-fault
    SpawnStorm,      // fork bomb
};

const wchar_t* culpritName(Culprit culprit);

struct ProcRecord {
    uint32_t pid = 0;
    uint32_t parentPid = 0;
    int64_t createTime = 0;      // with pid, the identity of a process
    uint64_t cpuTime = 0;        // kernel + user, 100ns units, cumulative
    uint64_t privateBytes = 0;
    uint64_t workingSetBytes = 0;
    uint32_t pageFaults = 0;     // cumulative, soft + hard
    uint32_t hardFaults = 0;     // cumulative, the disk-hitting subset
    uint32_t threadCount = 0;
    uint32_t handleCount = 0;
    uint32_t sessionId = 0;
    int32_t basePriority = 0;    // 24+ is REALTIME_PRIORITY_CLASS
    uint64_t ioBytes = 0;        // read + write transfer, cumulative
    uint16_t runnable = 0;       // Ready/Running/Standby/DeferredReady
    uint16_t waiting = 0;
    uint16_t paging = 0;         // waiting on page-in / free page / virtual memory
    uint16_t pumping = 0;        // WrUserRequest: sitting in GetMessage
    uint16_t suspended = 0;
    uint16_t flags = 0;
    uint16_t hungTicks = 0;      // consecutive ticks the by-state heuristic held
    HWND window = nullptr;       // first visible top-level window, for Close and titles

    // Per-second rates for the interval ending at this snapshot.
    float cpuCores = 0.0f;       // 1.0 = one full core
    float hardFaultsPerSec = 0.0f;
    float softFaultsPerSec = 0.0f;
    float privGrowthPerSec = 0.0f;   // bytes/s, negative when shrinking
    float threadGrowthPerSec = 0.0f;
    float ioBytesPerSec = 0.0f;
    float kernelFrac = 0.0f;         // share of this process's CPU spent in kernel
    float childSpawnsPerSec = 0.0f;  // new direct children this tick
    float score = 0.0f;

    // Recent history so a single quiet tick does not hide a culprit.
    float cpuHistory[kHistory] = {};
    float hardFaultHistory[kHistory] = {};
    uint8_t historyLen = 0;
    uint8_t historyHead = 0;

    wchar_t image[kImageChars] = {};  // "chrome.exe"; truncated, never resolved from disk
    wchar_t title[kTitleChars] = {};  // first titled visible window, read from the kernel's cache
};

struct Snapshot {
    int64_t qpc = 0;
    double elapsedSec = 0.0;     // since the previous snapshot; 0 on the first
    uint32_t sequence = 0;
    uint32_t count = 0;          // records in use
    uint32_t threadCount = 0;
    uint32_t cores = 1;
    uint32_t tickMicros = 0;     // how long the sample itself took
    uint32_t windowPassMicros = 0;
    bool windowPassTruncated = false;   // deadline hit; some ownsWindow flags missing
    // System-wide totals for this interval; the culprit rules compare a
    // process's share of these, not absolute numbers.
    float totalHardFaults = 0.0f;
    float totalIoBytes = 0.0f;
    float totalCpuCores = 0.0f;
    uint64_t physicalBytes = 0;
    float maxIoBytes = 0.0f;
    float maxHardFaults = 0.0f;  // normalisation bases used for the scores
    float maxSoftFaults = 0.0f;
    float maxPrivGrowth = 0.0f;
    float maxThreadGrowth = 0.0f;
    uint16_t ranked[kMaxRecords] = {};        // record indices, score descending
    uint16_t pidIndex[kPidIndexSize] = {};    // pid -> record index + 1; 0 = empty
    ProcRecord records[kMaxRecords];
};

struct SamplerStatus {
    bool running = false;
    bool memoryLocked = false;
    bool workingSetPinned = false;
    uint32_t bufferBytes = 0;
    uint32_t lockedBytes = 0;
    uint32_t ticks = 0;
    uint32_t missedTicks = 0;      // NtQuerySystemInformation failed or buffer too small
    uint32_t bufferTooSmall = 0;   // ticks lost to STATUS_INFO_LENGTH_MISMATCH
    uint32_t skippedFlips = 0;     // no free snapshot (a reader held it)
    uint32_t lastTickMicros = 0;
    wchar_t lastError[128] = {};
};

// Allocates, locks, resolves ntdll exports and starts the thread. Everything
// that may touch the disk or the heap happens here. `report` gets a human
// readable summary of what was and was not achieved.
bool samplerStart(const RescueSettings& settings, std::wstring* report);
void samplerStop();

// Hot-swaps weights and interval without reallocating.
void samplerApply(const RescueSettings& settings);

// While busy (the overlay is showing) the sampler avoids anything optional,
// including growing a buffer that came back too small.
void samplerSetBusy(bool busy);

// Pins the latest finished snapshot. Never blocks, never allocates. Returns
// null only before the first tick completes. Release exactly once.
const Snapshot* snapshotAcquire();
void snapshotRelease(const Snapshot* snapshot);

const ProcRecord* findRecord(const Snapshot& snapshot, uint32_t pid);

// The one process the evidence says is freezing the machine right now, or
// null. Deliberately strict: a wrong answer here pauses someone's app. Never
// returns the daemon, a deny-listed process, or anything ambiguous.
const ProcRecord* findCulprit(const Snapshot& snapshot, Culprit* why);

SamplerStatus samplerStatus();

} // namespace rescue
