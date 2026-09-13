#include "rescue_residency.h"

#include "rescue_log.h"

#include <algorithm>
#include <atomic>

namespace rescue {
namespace {

constexpr uint64_t kCompositorHeadroom = 256ull << 20;  // above its current working set; 64 MB left the switch at 7.5 s
constexpr uint64_t kCompositorMaxExtra = 512ull << 20;

SRWLOCK g_lock = SRWLOCK_INIT;
ResidencyStatus g_status;
uint32_t g_lastSeenPid = 0;

void setDetail(const wchar_t* text) {
    wcsncpy_s(g_status.detail, text, _TRUNCATE);
}

// The compositor for our session. Other sessions have their own dwm.exe; the
// session id in the snapshot tells them apart.
const ProcRecord* findCompositor(const Snapshot& snapshot) {
    DWORD session = 0;
    ProcessIdToSessionId(GetCurrentProcessId(), &session);
    for (uint32_t i = 0; i < snapshot.count; ++i) {
        const ProcRecord& record = snapshot.records[i];
        if (record.sessionId == session && _wcsicmp(record.image, L"dwm.exe") == 0) return &record;
    }
    return nullptr;
}

void pin(const ProcRecord& compositor) {
    AcquireSRWLockExclusive(&g_lock);
    g_status.attempted = true;
    g_status.pinned = false;
    g_status.compositorPid = compositor.pid;
    g_status.pinnedBytes = 0;

    HANDLE process = OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_SET_QUOTA, FALSE, compositor.pid);
    if (!process) {
        const DWORD error = GetLastError();
        setDetail(error == ERROR_ACCESS_DENIED
            ? L"compositor not pinned: needs an elevated daemon"
            : L"compositor not pinned: OpenProcess failed");
        log(L"%ls (dwm.exe pid %u, error %lu)", g_status.detail, compositor.pid, error);
        ReleaseSRWLockExclusive(&g_lock);
        return;
    }

    SIZE_T currentMin = 0;
    SIZE_T currentMax = 0;
    DWORD flags = 0;
    GetProcessWorkingSetSizeEx(process, &currentMin, &currentMax, &flags);

    // Never lower an existing floor; otherwise floor = what it uses now plus
    // headroom, so pinning costs nothing beyond what DWM already holds.
    const uint64_t wanted = compositor.workingSetBytes + kCompositorHeadroom;
    const SIZE_T minimum = static_cast<SIZE_T>(wanted > currentMin ? wanted : currentMin);
    const SIZE_T maximum = static_cast<SIZE_T>((std::max)(static_cast<uint64_t>(currentMax), minimum + kCompositorMaxExtra));
    const bool ok = SetProcessWorkingSetSizeEx(process, minimum, maximum,
                        QUOTA_LIMITS_HARDWS_MIN_ENABLE | QUOTA_LIMITS_HARDWS_MAX_DISABLE) != 0;
    const DWORD error = ok ? 0 : GetLastError();
    CloseHandle(process);

    if (ok) {
        g_status.pinned = true;
        g_status.pinnedBytes = minimum;
        wchar_t text[120];
        swprintf_s(text, L"compositor dwm.exe pid %u pinned: working set floor %llu MB",
            compositor.pid, static_cast<unsigned long long>(minimum >> 20));
        setDetail(text);
    } else {
        wchar_t text[120];
        swprintf_s(text, L"compositor not pinned: SetProcessWorkingSetSizeEx error %lu", error);
        setDetail(text);
    }
    log(L"%ls", g_status.detail);
    ReleaseSRWLockExclusive(&g_lock);
}

} // namespace

void residencyTick(const Snapshot& snapshot) {
    const ProcRecord* compositor = findCompositor(snapshot);
    if (!compositor) return;
    if (compositor->pid == g_lastSeenPid) return;
    g_lastSeenPid = compositor->pid;
    pin(*compositor);
}

ResidencyStatus residencyStatus() {
    AcquireSRWLockShared(&g_lock);
    ResidencyStatus copy = g_status;
    ReleaseSRWLockShared(&g_lock);
    return copy;
}

} // namespace rescue
