#include "rescue_sampler.h"

#include "rescue_nt.h"

#include <algorithm>
#include <atomic>
#include <cstring>
#include <new>

namespace rescue {
namespace {

constexpr int kSnapshotCount = 3;
constexpr uint32_t kInitialBufferBytes = 1u << 20;
constexpr uint32_t kMaxBufferBytes = 64u << 20;
constexpr uint32_t kWindowPassDeadlineMicros = 40 * 1000;
constexpr uint16_t kHungByStateTicks = 3;
constexpr SIZE_T kWorkingSetHeadroom = 48u << 20;

// Below these a term contributes nothing. Normalising against the sample max
// alone would crown a random process #1 on an idle machine.
constexpr float kHardFaultFloor = 100.0f;          // faults/s
constexpr float kSoftFaultFloor = 20000.0f;        // faults/s; busy apps sit in the low thousands
constexpr float kPrivGrowthFloor = 4.0f * 1048576; // bytes/s
constexpr float kThreadGrowthFloor = 2.0f;         // threads/s
constexpr float kCpuFloor = 0.02f;                 // cores
constexpr float kIoFloor = 20.0f * 1048576;        // bytes/s
constexpr float kSpawnStormPerSec = 10.0f;
constexpr float kIoStormBytesPerSec = 150.0f * 1048576;

uint64_t g_physicalBytes = 0;

NtApi g_api;
BYTE* g_buffer = nullptr;
uint32_t g_bufferBytes = 0;
bool g_bufferLocked = false;
uint32_t g_wantBufferBytes = 0;   // set when a tick came back short

Snapshot* g_snapshots[kSnapshotCount] = {};
bool g_snapshotsLocked = false;
std::atomic<int> g_published{-1};
std::atomic<int> g_readers[kSnapshotCount] = {};

HANDLE g_thread = nullptr;
HANDLE g_stopEvent = nullptr;
HANDLE g_timer = nullptr;
std::atomic<bool> g_busy{false};

SRWLOCK g_hotLock = SRWLOCK_INIT;
RescueWeights g_weights;
int g_intervalMs = 1000;

LARGE_INTEGER g_qpcFrequency{};
uint32_t g_cores = 1;
uint32_t g_selfPid = 0;

SRWLOCK g_statusLock = SRWLOCK_INIT;
SamplerStatus g_status;

void setLastError(const wchar_t* text) {
    AcquireSRWLockExclusive(&g_statusLock);
    wcsncpy_s(g_status.lastError, text, _TRUNCATE);
    ReleaseSRWLockExclusive(&g_statusLock);
}

int64_t qpcNow() {
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    return now.QuadPart;
}

uint32_t micros(int64_t from, int64_t to) {
    return static_cast<uint32_t>((to - from) * 1000000 / g_qpcFrequency.QuadPart);
}

// ---- memory ------------------------------------------------------------------

void* allocLocked(SIZE_T bytes, bool* locked) {
    void* memory = VirtualAlloc(nullptr, bytes, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!memory) return nullptr;
    *locked = VirtualLock(memory, bytes) != 0;
    return memory;
}

bool pinWorkingSet(SIZE_T lockedBytes) {
    // VirtualLock is bounded by the minimum working set, so the floor has to be
    // raised first. The hard minimum is what keeps the daemon resident when
    // the memory manager starts trimming everything else.
    SIZE_T minimum = lockedBytes + kWorkingSetHeadroom;
    SIZE_T maximum = minimum + (64u << 20);
    return SetProcessWorkingSetSizeEx(
               GetCurrentProcess(), minimum, maximum,
               QUOTA_LIMITS_HARDWS_MIN_ENABLE | QUOTA_LIMITS_HARDWS_MAX_DISABLE)
        != 0;
}

// Finds how much NtQuerySystemInformation needs right now. Returns 0 when the
// call itself is unavailable.
uint32_t measureNeededBytes() {
    uint32_t size = kInitialBufferBytes;
    while (size <= kMaxBufferBytes) {
        void* probe = VirtualAlloc(nullptr, size, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
        if (!probe) return 0;
        ULONG returned = 0;
        NTSTATUS status = g_api.querySystemInformation(kSystemProcessInformation, probe, size, &returned);
        VirtualFree(probe, 0, MEM_RELEASE);
        if (status == kStatusInfoLengthMismatch) {
            size = (returned > size) ? (returned + (returned / 2)) : size * 2;
            continue;
        }
        if (status < 0) return 0;
        return returned ? returned : size;
    }
    return 0;
}

bool replaceBuffer(uint32_t bytes) {
    bool locked = false;
    void* fresh = allocLocked(bytes, &locked);
    if (!fresh) return false;
    if (g_buffer) VirtualFree(g_buffer, 0, MEM_RELEASE);
    g_buffer = static_cast<BYTE*>(fresh);
    g_bufferBytes = bytes;
    g_bufferLocked = locked;
    return true;
}

// ---- pid index -------------------------------------------------------------------

uint32_t pidHash(uint32_t pid) {
    return ((pid >> 2) * 2654435761u) & (kPidIndexSize - 1);
}

void indexInsert(Snapshot& snapshot, uint32_t pid, uint32_t recordIndex) {
    uint32_t slot = pidHash(pid);
    while (snapshot.pidIndex[slot] != 0) slot = (slot + 1) & (kPidIndexSize - 1);
    snapshot.pidIndex[slot] = static_cast<uint16_t>(recordIndex + 1);
}

// ---- snapshot selection ---------------------------------------------------------

Snapshot* pickWritable() {
    const int published = g_published.load(std::memory_order_acquire);
    for (int i = 0; i < kSnapshotCount; ++i) {
        if (i == published) continue;
        if (g_readers[i].load(std::memory_order_acquire) == 0) return g_snapshots[i];
    }
    return nullptr;
}

int indexOf(const Snapshot* snapshot) {
    for (int i = 0; i < kSnapshotCount; ++i) {
        if (g_snapshots[i] == snapshot) return i;
    }
    return -1;
}

// ---- window ownership pass --------------------------------------------------------

struct WindowPass {
    Snapshot* snapshot;
    int64_t deadline;
    uint32_t seen;
    bool truncated;
};

BOOL CALLBACK windowPassCallback(HWND hwnd, LPARAM lParam) {
    auto* pass = reinterpret_cast<WindowPass*>(lParam);
    if ((++pass->seen & 31) == 0 && qpcNow() > pass->deadline) {
        pass->truncated = true;
        return FALSE;
    }

    // Nothing here sends a message to the window's thread: visibility, owner
    // and the hung flag are all answered from the kernel's own bookkeeping.
    if (!IsWindowVisible(hwnd)) return TRUE;

    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    if (pid == 0) return TRUE;

    auto* record = const_cast<ProcRecord*>(findRecord(*pass->snapshot, pid));
    if (!record) return TRUE;

    record->flags |= kFlagOwnsWindow;
    if (IsHungAppWindow(hwnd)) record->flags |= kFlagHungWindow;

    // InternalGetWindowText returns the title the kernel already holds and
    // never messages the owning thread. GetWindowText would send WM_GETTEXT
    // and block on exactly the hung pump we are trying to report. Windows
    // whose title lives only in a custom WM_GETTEXT handler come back empty
    // and are shown by image name, which is the intended degradation.
    if (!record->window || !record->title[0]) {
        wchar_t title[kTitleChars];
        const int chars = InternalGetWindowText(hwnd, title, kTitleChars);
        if (chars > 0) {
            wcscpy_s(record->title, title);
            record->window = hwnd;
        } else if (!record->window) {
            record->window = hwnd;
        }
    }
    return TRUE;
}

// ---- one tick ----------------------------------------------------------------------

void fillFromEntry(ProcRecord& record, const ProcessInfo& entry) {
    record.pid = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(entry.UniqueProcessId));
    record.parentPid = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(entry.InheritedFromUniqueProcessId));
    record.createTime = entry.CreateTime.QuadPart;
    record.cpuTime = static_cast<uint64_t>(entry.KernelTime.QuadPart + entry.UserTime.QuadPart);
    if (record.cpuTime) {
        record.kernelFrac = static_cast<float>(static_cast<double>(entry.KernelTime.QuadPart) / static_cast<double>(record.cpuTime));
    }
    record.privateBytes = entry.PrivatePageCount;
    record.workingSetBytes = entry.WorkingSetSize;
    record.pageFaults = entry.PageFaultCount;
    record.hardFaults = entry.HardFaultCount;
    record.threadCount = entry.NumberOfThreads;
    record.handleCount = entry.HandleCount;
    record.sessionId = entry.SessionId;
    record.basePriority = entry.BasePriority;
    record.ioBytes = static_cast<uint64_t>(entry.ReadTransferCount.QuadPart + entry.WriteTransferCount.QuadPart);
    if (record.basePriority >= 24) record.flags |= kFlagRealtime;
    if (record.pid == g_selfPid) record.flags |= kFlagSelf;

    if (entry.ImageName.Buffer && entry.ImageName.Length) {
        size_t chars = entry.ImageName.Length / sizeof(wchar_t);
        if (chars > kImageChars - 1) chars = kImageChars - 1;
        memcpy(record.image, entry.ImageName.Buffer, chars * sizeof(wchar_t));
        record.image[chars] = L'\0';
    } else if (record.pid == 4) {
        wcscpy_s(record.image, L"System");
    } else {
        wcscpy_s(record.image, L"?");
    }

    for (ULONG i = 0; i < entry.NumberOfThreads; ++i) {
        const ThreadInfo& thread = entry.Threads[i];
        switch (thread.ThreadState) {
        case kThreadReady:
        case kThreadRunning:
        case kThreadStandby:
        case kThreadDeferredReady:
            ++record.runnable;
            break;
        case kThreadWaiting:
            ++record.waiting;
            switch (thread.WaitReason) {
            case kWaitWrUserRequest:
                ++record.pumping;
                break;
            case kWaitSuspended:
            case kWaitWrSuspended:
                ++record.suspended;
                break;
            case kWaitFreePage:
            case kWaitPageIn:
            case kWaitWrFreePage:
            case kWaitWrPageIn:
            case kWaitWrVirtualMemory:
            case kWaitWrPageOut:
                ++record.paging;
                break;
            default:
                break;
            }
            break;
        default:
            break;
        }
    }
    if (record.paging) record.flags |= kFlagPaging;
}

void computeDeltas(ProcRecord& record, const ProcRecord& previous, double elapsed) {
    const double cpuDelta100ns = static_cast<double>(record.cpuTime - previous.cpuTime);
    record.cpuCores = static_cast<float>(cpuDelta100ns / (elapsed * 1e7));
    record.ioBytesPerSec = static_cast<float>(static_cast<double>(record.ioBytes - previous.ioBytes) / elapsed);

    const uint32_t faultDelta = record.pageFaults - previous.pageFaults;
    const uint32_t hardDelta = record.hardFaults - previous.hardFaults;
    const uint32_t softDelta = faultDelta >= hardDelta ? faultDelta - hardDelta : 0;
    record.hardFaultsPerSec = static_cast<float>(hardDelta / elapsed);
    record.softFaultsPerSec = static_cast<float>(softDelta / elapsed);

    record.privGrowthPerSec = static_cast<float>(
        (static_cast<double>(record.privateBytes) - static_cast<double>(previous.privateBytes)) / elapsed);
    record.threadGrowthPerSec = static_cast<float>(
        (static_cast<double>(record.threadCount) - static_cast<double>(previous.threadCount)) / elapsed);

    memcpy(record.cpuHistory, previous.cpuHistory, sizeof(record.cpuHistory));
    memcpy(record.hardFaultHistory, previous.hardFaultHistory, sizeof(record.hardFaultHistory));
    record.historyHead = previous.historyHead;
    record.historyLen = previous.historyLen;
    record.cpuHistory[record.historyHead] = record.cpuCores;
    record.hardFaultHistory[record.historyHead] = record.hardFaultsPerSec;
    record.historyHead = static_cast<uint8_t>((record.historyHead + 1) % kHistory);
    if (record.historyLen < kHistory) ++record.historyLen;
    record.hungTicks = previous.hungTicks;
}

float normalised(float value, float base, float floor) {
    if (value < floor || base < floor) return 0.0f;
    const float ratio = value / base;
    return ratio > 1.0f ? 1.0f : ratio;
}

void scoreSnapshot(Snapshot& snapshot, const RescueWeights& weights) {
    snapshot.maxHardFaults = 0.0f;
    snapshot.maxSoftFaults = 0.0f;
    snapshot.maxPrivGrowth = 0.0f;
    snapshot.maxThreadGrowth = 0.0f;
    snapshot.maxIoBytes = 0.0f;
    snapshot.totalHardFaults = 0.0f;
    snapshot.totalIoBytes = 0.0f;
    snapshot.totalCpuCores = 0.0f;
    snapshot.physicalBytes = g_physicalBytes;

    // Spawn storms: credit each new child to its parent. A fork bomb is the
    // parent, not the children the menu would otherwise show.
    for (uint32_t i = 0; i < snapshot.count; ++i) {
        const ProcRecord& child = snapshot.records[i];
        if (!(child.flags & kFlagNew)) continue;
        auto* parent = const_cast<ProcRecord*>(findRecord(snapshot, child.parentPid));
        if (parent && parent->createTime <= child.createTime) parent->childSpawnsPerSec += 1.0f;
    }
    if (snapshot.elapsedSec > 0.0) {
        for (uint32_t i = 0; i < snapshot.count; ++i) {
            ProcRecord& record = snapshot.records[i];
            record.childSpawnsPerSec = static_cast<float>(record.childSpawnsPerSec / snapshot.elapsedSec);
            if (record.childSpawnsPerSec >= kSpawnStormPerSec) record.flags |= kFlagSpawnStorm;
        }
    }

    for (uint32_t i = 0; i < snapshot.count; ++i) {
        const ProcRecord& record = snapshot.records[i];
        snapshot.totalHardFaults += record.hardFaultsPerSec;
        snapshot.totalIoBytes += record.ioBytesPerSec;
        snapshot.totalCpuCores += record.cpuCores;
        snapshot.maxIoBytes = (std::max)(snapshot.maxIoBytes, record.ioBytesPerSec);
        snapshot.maxHardFaults = (std::max)(snapshot.maxHardFaults, record.hardFaultsPerSec);
        snapshot.maxSoftFaults = (std::max)(snapshot.maxSoftFaults, record.softFaultsPerSec);
        snapshot.maxPrivGrowth = (std::max)(snapshot.maxPrivGrowth, record.privGrowthPerSec);
        snapshot.maxThreadGrowth = (std::max)(snapshot.maxThreadGrowth, record.threadGrowthPerSec);
    }

    for (uint32_t i = 0; i < snapshot.count; ++i) {
        ProcRecord& record = snapshot.records[i];

        float hung = 0.0f;
        if (record.flags & kFlagHungWindow) hung = 1.0f;
        else if (record.flags & kFlagHungByState) hung = 0.7f;

        const float cpu = record.cpuCores < kCpuFloor ? 0.0f : (std::min)(record.cpuCores, 1.0f);

        // Hard faults are the disk-hitting ones that freeze everyone else and
        // get the full weight. A soft-fault storm is still a process churning
        // memory at an absurd rate, so it can earn up to half.
        const float faults = (std::max)(
            normalised(record.hardFaultsPerSec, snapshot.maxHardFaults, kHardFaultFloor),
            0.5f * normalised(record.softFaultsPerSec, snapshot.maxSoftFaults, kSoftFaultFloor));

        if (record.ioBytesPerSec >= kIoStormBytesPerSec && record.ioBytesPerSec >= 0.7f * snapshot.totalIoBytes) {
            record.flags |= kFlagIoStorm;
        }

        record.score = weights.hung * hung
            + weights.faults * faults
            + weights.io * normalised(record.ioBytesPerSec, snapshot.maxIoBytes, kIoFloor)
            + (record.flags & kFlagSpawnStorm ? weights.threads * 2.0f : 0.0f)
            + (record.flags & kFlagRealtime ? weights.cpu * (std::min)(record.cpuCores, 1.0f) : 0.0f)
            + weights.cpu * cpu
            + weights.priv * normalised(record.privGrowthPerSec, snapshot.maxPrivGrowth, kPrivGrowthFloor)
            + weights.threads * normalised(record.threadGrowthPerSec, snapshot.maxThreadGrowth, kThreadGrowthFloor);
    }

    for (uint32_t i = 0; i < snapshot.count; ++i) snapshot.ranked[i] = static_cast<uint16_t>(i);
    std::sort(snapshot.ranked, snapshot.ranked + snapshot.count, [&](uint16_t a, uint16_t b) {
        const ProcRecord& ra = snapshot.records[a];
        const ProcRecord& rb = snapshot.records[b];
        if (ra.score != rb.score) return ra.score > rb.score;
        if (ra.cpuCores != rb.cpuCores) return ra.cpuCores > rb.cpuCores;
        return ra.privateBytes > rb.privateBytes;
    });
}

void tick() {
    const int64_t started = qpcNow();

    ULONG returned = 0;
    NTSTATUS status = g_api.querySystemInformation(kSystemProcessInformation, g_buffer, g_bufferBytes, &returned);
    if (status == kStatusInfoLengthMismatch) {
        // The buffer contents are undefined on this status, so the tick is
        // lost. The previous snapshot stays published; the buffer grows on the
        // next quiet tick rather than here.
        AcquireSRWLockExclusive(&g_statusLock);
        ++g_status.missedTicks;
        ++g_status.bufferTooSmall;
        ReleaseSRWLockExclusive(&g_statusLock);
        g_wantBufferBytes = returned > g_bufferBytes ? returned * 2 : g_bufferBytes * 2;
        return;
    }
    if (status < 0) {
        AcquireSRWLockExclusive(&g_statusLock);
        ++g_status.missedTicks;
        ReleaseSRWLockExclusive(&g_statusLock);
        setLastError(L"NtQuerySystemInformation failed");
        return;
    }

    Snapshot* target = pickWritable();
    if (!target) {
        AcquireSRWLockExclusive(&g_statusLock);
        ++g_status.skippedFlips;
        ReleaseSRWLockExclusive(&g_statusLock);
        return;
    }

    const int publishedIndex = g_published.load(std::memory_order_acquire);
    const Snapshot* previous = publishedIndex >= 0 ? g_snapshots[publishedIndex] : nullptr;

    RescueWeights weights;
    AcquireSRWLockShared(&g_hotLock);
    weights = g_weights;
    ReleaseSRWLockShared(&g_hotLock);

    double elapsed = 0.0;
    if (previous) {
        elapsed = static_cast<double>(started - previous->qpc) / static_cast<double>(g_qpcFrequency.QuadPart);
        if (elapsed < 0.05) elapsed = 0.05;
    }

    memset(target->pidIndex, 0, sizeof(target->pidIndex));
    target->count = 0;
    target->threadCount = 0;

    const BYTE* cursor = g_buffer;
    const BYTE* end = g_buffer + returned;
    for (;;) {
        const auto* entry = reinterpret_cast<const ProcessInfo*>(cursor);
        const uint32_t pid = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(entry->UniqueProcessId));

        // Pid 0 is the idle process: its "CPU" is every idle core.
        if (pid != 0 && target->count < kMaxRecords) {
            ProcRecord& record = target->records[target->count];
            record = ProcRecord{};
            fillFromEntry(record, *entry);
            target->threadCount += entry->NumberOfThreads;

            const ProcRecord* before = previous ? findRecord(*previous, pid) : nullptr;
            if (before && before->createTime == record.createTime && elapsed > 0.0) {
                computeDeltas(record, *before, elapsed);
            } else {
                record.flags |= kFlagNew;
            }

            indexInsert(*target, pid, target->count);
            ++target->count;
        }

        if (entry->NextEntryOffset == 0) break;
        cursor += entry->NextEntryOffset;
        if (cursor + sizeof(ProcessInfo) > end) break;
    }

    const int64_t windowStart = qpcNow();
    WindowPass pass{target, windowStart + (static_cast<int64_t>(kWindowPassDeadlineMicros) * g_qpcFrequency.QuadPart) / 1000000, 0, false};
    EnumWindows(windowPassCallback, reinterpret_cast<LPARAM>(&pass));
    target->windowPassMicros = micros(windowStart, qpcNow());
    target->windowPassTruncated = pass.truncated;

    // "Not responding" is IsHungAppWindow, set in the window pass. A thread-
    // state guess (owns a window, nothing runnable, nobody in GetMessage) was
    // tried and dropped: Qt, Electron and Chromium wait for messages through
    // MsgWaitForMultipleObjectsEx, which reports a different wait reason, so
    // every idle browser looked hung.

    scoreSnapshot(*target, weights);

    target->qpc = started;
    target->elapsedSec = elapsed;
    target->cores = g_cores;
    target->sequence = previous ? previous->sequence + 1 : 1;
    target->tickMicros = micros(started, qpcNow());

    g_published.store(indexOf(target), std::memory_order_release);

    AcquireSRWLockExclusive(&g_statusLock);
    ++g_status.ticks;
    g_status.lastTickMicros = target->tickMicros;
    ReleaseSRWLockExclusive(&g_statusLock);
}

void growIfWanted() {
    if (g_wantBufferBytes == 0 || g_busy.load()) return;
    const uint32_t wanted = (std::min)(g_wantBufferBytes, kMaxBufferBytes);
    g_wantBufferBytes = 0;
    if (wanted <= g_bufferBytes) return;
    if (!replaceBuffer(wanted)) {
        setLastError(L"could not grow the snapshot buffer");
        return;
    }
    SIZE_T locked = g_bufferBytes + static_cast<SIZE_T>(sizeof(Snapshot)) * kSnapshotCount;
    pinWorkingSet(locked);
    AcquireSRWLockExclusive(&g_statusLock);
    g_status.bufferBytes = g_bufferBytes;
    g_status.memoryLocked = g_bufferLocked && g_snapshotsLocked;
    ReleaseSRWLockExclusive(&g_statusLock);
}

DWORD WINAPI samplerThread(LPVOID) {
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);
    SetThreadDescription(GetCurrentThread(), L"keyrail-rescue-sampler");

    tick();

    HANDLE waits[2] = {g_stopEvent, g_timer};
    for (;;) {
        DWORD woke = WaitForMultipleObjects(2, waits, FALSE, INFINITE);
        if (woke == WAIT_OBJECT_0) break;
        if (woke != WAIT_OBJECT_0 + 1) break;
        tick();
        growIfWanted();
    }
    return 0;
}

void armTimer() {
    AcquireSRWLockShared(&g_hotLock);
    const int interval = g_intervalMs;
    ReleaseSRWLockShared(&g_hotLock);
    LARGE_INTEGER due;
    due.QuadPart = -static_cast<LONGLONG>(interval) * 10000;
    SetWaitableTimer(g_timer, &due, interval, nullptr, nullptr, FALSE);
}

} // namespace

bool samplerStart(const RescueSettings& settings, std::wstring* report) {
    if (g_thread) return true;

    std::wstring out;
    g_api = ntApi();
    if (!g_api.querySystemInformation) {
        if (report) *report = L"rescue sampler unavailable: NtQuerySystemInformation not found\n";
        setLastError(L"NtQuerySystemInformation not found");
        return false;
    }

    QueryPerformanceFrequency(&g_qpcFrequency);
    SYSTEM_INFO info{};
    GetSystemInfo(&info);
    g_cores = info.dwNumberOfProcessors ? info.dwNumberOfProcessors : 1;
    g_selfPid = GetCurrentProcessId();
    MEMORYSTATUSEX memoryStatus{};
    memoryStatus.dwLength = sizeof(memoryStatus);
    if (GlobalMemoryStatusEx(&memoryStatus)) g_physicalBytes = memoryStatus.ullTotalPhys;

    {
        AcquireSRWLockExclusive(&g_hotLock);
        g_weights = settings.weights;
        g_intervalMs = settings.sampleIntervalMs;
        ReleaseSRWLockExclusive(&g_hotLock);
    }

    // Everything the tick touches is allocated once here and locked so a
    // thrashing machine cannot page the sampler out. VirtualLock needs the
    // working-set floor raised first; both are reported, neither is fatal.
    const uint32_t needed = measureNeededBytes();
    if (needed == 0) {
        if (report) *report = L"rescue sampler unavailable: could not size the process snapshot\n";
        setLastError(L"could not size the process snapshot");
        return false;
    }
    const uint32_t bufferBytes = (std::min)(needed * 2, kMaxBufferBytes);
    const SIZE_T lockedBytes = bufferBytes + static_cast<SIZE_T>(sizeof(Snapshot)) * kSnapshotCount;
    const bool pinned = pinWorkingSet(lockedBytes);

    if (!replaceBuffer(bufferBytes)) {
        if (report) *report = L"rescue sampler unavailable: could not allocate the snapshot buffer\n";
        setLastError(L"could not allocate the snapshot buffer");
        return false;
    }

    g_snapshotsLocked = true;
    for (int i = 0; i < kSnapshotCount; ++i) {
        bool locked = false;
        void* memory = allocLocked(sizeof(Snapshot), &locked);
        if (!memory) {
            if (report) *report = L"rescue sampler unavailable: could not allocate snapshot tables\n";
            setLastError(L"could not allocate snapshot tables");
            return false;
        }
        g_snapshots[i] = new (memory) Snapshot();
        g_snapshotsLocked = g_snapshotsLocked && locked;
    }

    g_stopEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    g_timer = CreateWaitableTimerW(nullptr, FALSE, nullptr);
    if (!g_stopEvent || !g_timer) {
        if (report) *report = L"rescue sampler unavailable: could not create timer\n";
        setLastError(L"could not create timer");
        return false;
    }
    armTimer();

    {
        AcquireSRWLockExclusive(&g_statusLock);
        g_status.running = true;
        g_status.memoryLocked = g_bufferLocked && g_snapshotsLocked;
        g_status.workingSetPinned = pinned;
        g_status.bufferBytes = g_bufferBytes;
        g_status.lockedBytes = static_cast<uint32_t>(lockedBytes);
        ReleaseSRWLockExclusive(&g_statusLock);
    }

    g_thread = CreateThread(nullptr, 0, samplerThread, nullptr, 0, nullptr);
    if (!g_thread) {
        if (report) *report = L"rescue sampler unavailable: could not start thread\n";
        setLastError(L"could not start thread");
        return false;
    }

    out += L"rescue sampler armed: " + std::to_wstring(g_bufferBytes / 1024) + L" KB snapshot buffer, "
        + std::to_wstring(lockedBytes / 1024) + L" KB "
        + (g_bufferLocked && g_snapshotsLocked ? L"locked" : L"NOT locked") + L", working set "
        + (pinned ? L"pinned" : L"NOT pinned") + L", " + std::to_wstring(settings.sampleIntervalMs) + L"ms\n";
    if (report) *report = out;
    return true;
}

void samplerStop() {
    if (g_thread) {
        SetEvent(g_stopEvent);
        WaitForSingleObject(g_thread, 5000);
        CloseHandle(g_thread);
        g_thread = nullptr;
    }
    if (g_timer) {
        CloseHandle(g_timer);
        g_timer = nullptr;
    }
    if (g_stopEvent) {
        CloseHandle(g_stopEvent);
        g_stopEvent = nullptr;
    }
    g_published.store(-1);
    for (int i = 0; i < kSnapshotCount; ++i) {
        if (g_snapshots[i]) {
            VirtualFree(g_snapshots[i], 0, MEM_RELEASE);
            g_snapshots[i] = nullptr;
        }
    }
    if (g_buffer) {
        VirtualFree(g_buffer, 0, MEM_RELEASE);
        g_buffer = nullptr;
        g_bufferBytes = 0;
    }
    AcquireSRWLockExclusive(&g_statusLock);
    g_status.running = false;
    ReleaseSRWLockExclusive(&g_statusLock);
}

void samplerApply(const RescueSettings& settings) {
    bool intervalChanged = false;
    {
        AcquireSRWLockExclusive(&g_hotLock);
        g_weights = settings.weights;
        intervalChanged = g_intervalMs != settings.sampleIntervalMs;
        g_intervalMs = settings.sampleIntervalMs;
        ReleaseSRWLockExclusive(&g_hotLock);
    }
    if (intervalChanged && g_timer) armTimer();
}

void samplerSetBusy(bool busy) {
    g_busy.store(busy);
}

const Snapshot* snapshotAcquire() {
    for (;;) {
        const int index = g_published.load(std::memory_order_acquire);
        if (index < 0) return nullptr;
        g_readers[index].fetch_add(1, std::memory_order_acq_rel);
        if (g_published.load(std::memory_order_acquire) == index) return g_snapshots[index];
        // Flipped underneath us; the writer may already be reusing this one.
        g_readers[index].fetch_sub(1, std::memory_order_acq_rel);
    }
}

void snapshotRelease(const Snapshot* snapshot) {
    const int index = indexOf(snapshot);
    if (index >= 0) g_readers[index].fetch_sub(1, std::memory_order_acq_rel);
}

const ProcRecord* findRecord(const Snapshot& snapshot, uint32_t pid) {
    uint32_t slot = pidHash(pid);
    for (uint32_t probe = 0; probe < kPidIndexSize; ++probe) {
        const uint16_t entry = snapshot.pidIndex[slot];
        if (entry == 0) return nullptr;
        const ProcRecord& record = snapshot.records[entry - 1];
        if (record.pid == pid) return &record;
        slot = (slot + 1) & (kPidIndexSize - 1);
    }
    return nullptr;
}

const wchar_t* culpritName(Culprit culprit) {
    switch (culprit) {
    case Culprit::MemoryHog: return L"memory hog (causing the page-fault storm)";
    case Culprit::RealtimeSpinner: return L"realtime-priority spinner";
    case Culprit::IoStorm: return L"disk storm (everyone else is waiting on it)";
    case Culprit::SpawnStorm: return L"spawn storm (fork bomb)";
    default: return L"";
    }
}

// Rules are absolute thresholds plus a dominance test: the suspect must own
// most of the effect and the runner-up must be far behind. Anything less and
// the answer is "show the menu, let the human decide".
const ProcRecord* findCulprit(const Snapshot& snapshot, Culprit* why) {
    if (why) *why = Culprit::None;
    if (snapshot.elapsedSec <= 0.0 || snapshot.count == 0) return nullptr;

    const ProcRecord* best = nullptr;
    Culprit reason = Culprit::None;

    auto eligible = [&](const ProcRecord& record) {
        if (record.flags & (kFlagSelf | kFlagNew)) return false;
        if (record.pid <= 4) return false;
        return true;
    };

    // 1. Memory hog. Measured, not guessed: the hog itself barely hard-faults
    //    (its evicted pages are still being written out, so it takes soft
    //    faults), while every other process hard-faults to get its own pages
    //    back. So: the system is hard-faulting heavily, and one process holds
    //    a dominant slice of RAM and is still churning it.
    if (snapshot.totalHardFaults >= 1000.0f && snapshot.physicalBytes) {
        const ProcRecord* top = nullptr;
        const ProcRecord* second = nullptr;
        auto footprint = [](const ProcRecord& record) {
            return (std::max)(record.privateBytes, record.workingSetBytes);
        };
        for (uint32_t i = 0; i < snapshot.count; ++i) {
            const ProcRecord& record = snapshot.records[i];
            if (!eligible(record)) continue;
            if (!top || footprint(record) > footprint(*top)) {
                second = top;
                top = &record;
            } else if (!second || footprint(record) > footprint(*second)) {
                second = &record;
            }
        }
        if (top) {
            const double ramShare = static_cast<double>(footprint(*top)) / static_cast<double>(snapshot.physicalBytes);
            const bool dominant = !second || footprint(*top) >= footprint(*second) + footprint(*second) / 2;
            const bool churning = top->softFaultsPerSec >= 20000.0f
                || top->privGrowthPerSec >= 50.0f * 1048576
                || top->hardFaultsPerSec >= 0.3f * snapshot.totalHardFaults
                || (top->flags & kFlagPaging);
            if (ramShare >= 0.25 && dominant && churning) {
                best = top;
                reason = Culprit::MemoryHog;
            }
        }
    }

    // 2. Realtime spinner: realtime class and at least half the cores busy.
    if (!best) {
        for (uint32_t i = 0; i < snapshot.count; ++i) {
            const ProcRecord& record = snapshot.records[i];
            if (!eligible(record) || !(record.flags & kFlagRealtime)) continue;
            if (record.cpuCores >= 0.5f * static_cast<float>(snapshot.cores) && record.cpuCores >= 1.0f) {
                best = &record;
                reason = Culprit::RealtimeSpinner;
                break;
            }
        }
    }

    // 3. Disk storm: one process is >= 70% of all disk traffic at storm rates
    //    and others are hard-faulting because of it (>= 500/s not theirs).
    if (!best && snapshot.totalIoBytes >= kIoStormBytesPerSec) {
        for (uint32_t i = 0; i < snapshot.count; ++i) {
            const ProcRecord& record = snapshot.records[i];
            if (!eligible(record) || !(record.flags & kFlagIoStorm)) continue;
            const float othersFaults = snapshot.totalHardFaults - record.hardFaultsPerSec;
            if (othersFaults >= 500.0f) {
                best = &record;
                reason = Culprit::IoStorm;
                break;
            }
        }
    }

    // 4. Spawn storm: the parent creating children by the dozen.
    if (!best) {
        for (uint32_t i = 0; i < snapshot.count; ++i) {
            const ProcRecord& record = snapshot.records[i];
            if (!eligible(record) || !(record.flags & kFlagSpawnStorm)) continue;
            if (record.childSpawnsPerSec >= 2.0f * kSpawnStormPerSec) {
                best = &record;
                reason = Culprit::SpawnStorm;
                break;
            }
        }
    }

    if (why) *why = reason;
    return best;
}

SamplerStatus samplerStatus() {
    AcquireSRWLockShared(&g_statusLock);
    SamplerStatus copy = g_status;
    ReleaseSRWLockShared(&g_statusLock);
    return copy;
}

} // namespace rescue
