#include "rescue_kill.h"

#include "rescue_log.h"
#include "rescue_nt.h"
#include "rescue_overlay.h"
#include "rescue_sampler.h"

#include <atomic>

namespace rescue {
namespace {

// Terminating any of these bugchecks the machine on the spot. explorer.exe and
// dwm.exe are deliberately absent: both restart on their own, and a hung shell
// or compositor is one of the main reasons to reach for this menu.
constexpr const wchar_t* kDenied[] = {
    L"csrss.exe", L"smss.exe", L"wininit.exe", L"winlogon.exe",
    L"services.exe", L"lsass.exe", L"system", L"secure system", L"registry",
};

constexpr DWORD kVerifyWaitMs = 3000;
constexpr uint32_t kWindowDeadlineMicros = 50 * 1000;

enum class Op { None, Close, Kill };

struct Request {
    Op op = Op::None;
    uint32_t pid = 0;
    int64_t createTime = 0;
};

HANDLE g_thread = nullptr;
HANDLE g_wake = nullptr;
HANDLE g_stop = nullptr;
SRWLOCK g_requestLock = SRWLOCK_INIT;
Request g_pending;
uint32_t g_selfPid = 0;
LARGE_INTEGER g_qpcFrequency{};
std::atomic<bool> g_elevated{false};
std::atomic<bool> g_debugPrivilege{false};

SRWLOCK g_pauseLock = SRWLOCK_INIT;
PauseInfo g_pause;
HANDLE g_pauseHandle = nullptr;

void clearPauseLocked() {
    if (g_pauseHandle) CloseHandle(g_pauseHandle);
    g_pauseHandle = nullptr;
    g_pause = PauseInfo{};
}

bool tokenIsElevated() {
    HANDLE token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) return false;
    TOKEN_ELEVATION elevation{};
    DWORD returned = 0;
    const bool ok = GetTokenInformation(token, TokenElevation, &elevation, sizeof(elevation), &returned) != 0;
    CloseHandle(token);
    return ok && elevation.TokenIsElevated != 0;
}

// SeDebugPrivilege is present-but-disabled in an elevated token and absent
// from a filtered one, so this only ever succeeds when the daemon was started
// as administrator. The daemon has no privilege code of its own to reuse:
// elevation is arranged by the settings UI through a scheduled task.
bool enableDebugPrivilege() {
    HANDLE token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &token)) return false;
    TOKEN_PRIVILEGES privileges{};
    privileges.PrivilegeCount = 1;
    privileges.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
    bool ok = LookupPrivilegeValueW(nullptr, SE_DEBUG_NAME, &privileges.Privileges[0].Luid) != 0;
    if (ok) {
        AdjustTokenPrivileges(token, FALSE, &privileges, sizeof(privileges), nullptr, nullptr);
        ok = GetLastError() == ERROR_SUCCESS;
    }
    CloseHandle(token);
    return ok;
}

void report(const wchar_t* text) {
    log(L"%ls", text);
    overlaySetResult(text);
}

// Looks the process up in the latest snapshot; the create time guards against
// a reused pid. Copies the image name out so the snapshot is released quickly.
bool resolve(uint32_t pid, int64_t createTime, wchar_t (&image)[kImageChars]) {
    const Snapshot* snapshot = snapshotAcquire();
    if (!snapshot) return false;
    const ProcRecord* record = findRecord(*snapshot, pid);
    const bool ok = record && record->createTime == createTime;
    if (ok) wcscpy_s(image, record->image);
    snapshotRelease(snapshot);
    return ok;
}

struct CloseWindows {
    uint32_t pid;
    int64_t deadline;
    uint32_t seen;
    uint32_t posted;
};

BOOL CALLBACK closeWindowsCallback(HWND hwnd, LPARAM lParam) {
    auto* pass = reinterpret_cast<CloseWindows*>(lParam);
    if ((++pass->seen & 31) == 0) {
        LARGE_INTEGER now;
        QueryPerformanceCounter(&now);
        if (now.QuadPart > pass->deadline) return FALSE;
    }
    if (!IsWindowVisible(hwnd)) return TRUE;
    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    if (pid != pass->pid) return TRUE;
    if (PostMessageW(hwnd, WM_CLOSE, 0, 0)) ++pass->posted;
    return TRUE;
}

void doClose(const Request& request) {
    wchar_t image[kImageChars];
    if (!resolve(request.pid, request.createTime, image)) {
        report(L"close: that process is already gone");
        return;
    }

    // EnumWindows runs on the caller's desktop, which is the normal one for
    // this thread, so this works while the menu sits on the private desktop.
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    CloseWindows pass{request.pid, now.QuadPart + (static_cast<int64_t>(kWindowDeadlineMicros) * g_qpcFrequency.QuadPart) / 1000000, 0, 0};
    EnumWindows(closeWindowsCallback, reinterpret_cast<LPARAM>(&pass));

    wchar_t text[200];
    if (pass.posted == 0) {
        swprintf_s(text, L"close %u %ls: no visible window to close; use Enter to kill", request.pid, image);
    } else {
        swprintf_s(text, L"close %u %ls: WM_CLOSE posted to %u window(s); a hung app will not read it",
            request.pid, image, pass.posted);
    }
    report(text);
}

bool queryProtection(HANDLE process, ProtectionInfo* protection) {
    const NtApi& api = ntApi();
    if (!api.queryInformationProcess) return false;
    ULONG returned = 0;
    return api.queryInformationProcess(process, kProcessProtectionInformation, protection, sizeof(*protection), &returned) >= 0;
}

bool queryCritical(HANDLE process, bool* critical) {
    const NtApi& api = ntApi();
    if (!api.queryInformationProcess) return false;
    ULONG flag = 0;
    ULONG returned = 0;
    if (api.queryInformationProcess(process, kProcessBreakOnTermination, &flag, sizeof(flag), &returned) < 0) return false;
    *critical = flag != 0;
    return true;
}

void doKill(const Request& request) {
    wchar_t image[kImageChars];
    if (!resolve(request.pid, request.createTime, image)) {
        report(L"kill: that process is already gone");
        return;
    }
    wchar_t text[200];
    if (isDenied(request.pid, image)) {
        swprintf_s(text, L"kill %u %ls refused: terminating it would crash Windows", request.pid, image);
        report(text);
        return;
    }

    // Diagnose first with the access a protected process still grants, so the
    // answer is right even when the terminate handle is refused below.
    bool protectedProcess = false;
    bool critical = false;
    HANDLE probe = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, request.pid);
    if (probe) {
        ProtectionInfo protection{};
        if (queryProtection(probe, &protection)) protectedProcess = (protection.Level & 0x7) != 0;
        CloseHandle(probe);
    }
    HANDLE probeFull = OpenProcess(PROCESS_QUERY_INFORMATION, FALSE, request.pid);
    if (probeFull) {
        queryCritical(probeFull, &critical);
        CloseHandle(probeFull);
    }
    if (critical) {
        swprintf_s(text, L"kill %u %ls refused: marked critical, terminating it would crash Windows", request.pid, image);
        report(text);
        return;
    }

    HANDLE process = OpenProcess(PROCESS_TERMINATE | SYNCHRONIZE, FALSE, request.pid);
    if (!process) {
        const DWORD error = GetLastError();
        if (protectedProcess) {
            swprintf_s(text, L"kill %u %ls: protected process (anti-cheat / AV) - not terminable from user mode", request.pid, image);
        } else if (error == ERROR_ACCESS_DENIED && !g_elevated.load()) {
            swprintf_s(text, L"kill %u %ls: access denied - run the daemon as administrator (Settings > Administrator access)", request.pid, image);
        } else {
            swprintf_s(text, L"kill %u %ls: OpenProcess failed (error %lu)", request.pid, image, error);
        }
        report(text);
        return;
    }

    LARGE_INTEGER started;
    QueryPerformanceCounter(&started);
    if (!TerminateProcess(process, 1)) {
        swprintf_s(text, L"kill %u %ls: TerminateProcess failed (error %lu)", request.pid, image, GetLastError());
        CloseHandle(process);
        report(text);
        return;
    }

    const DWORD waited = WaitForSingleObject(process, kVerifyWaitMs);
    LARGE_INTEGER ended;
    QueryPerformanceCounter(&ended);
    const uint32_t millis = static_cast<uint32_t>((ended.QuadPart - started.QuadPart) * 1000 / g_qpcFrequency.QuadPart);
    CloseHandle(process);

    if (waited == WAIT_OBJECT_0) {
        AcquireSRWLockExclusive(&g_pauseLock);
        if (g_pause.active && g_pause.pid == request.pid) clearPauseLocked();
        ReleaseSRWLockExclusive(&g_pauseLock);
        swprintf_s(text, L"killed %u %ls (exit confirmed after %u ms)", request.pid, image, millis);
    } else if (protectedProcess) {
        swprintf_s(text, L"kill %u %ls queued but it is still alive: protected process (anti-cheat / AV)", request.pid, image);
    } else {
        swprintf_s(text, L"kill %u %ls queued but still alive after %u ms: threads blocked in a kernel wait - needs a driver or a reboot",
            request.pid, image, millis);
    }
    report(text);
}

DWORD WINAPI killThread(LPVOID) {
    SetThreadDescription(GetCurrentThread(), L"keyrail-rescue-kill");
    HANDLE waits[2] = {g_stop, g_wake};
    for (;;) {
        const DWORD woke = WaitForMultipleObjects(2, waits, FALSE, INFINITE);
        if (woke != WAIT_OBJECT_0 + 1) break;

        Request request;
        AcquireSRWLockExclusive(&g_requestLock);
        request = g_pending;
        g_pending = Request{};
        ReleaseSRWLockExclusive(&g_requestLock);

        if (request.op == Op::Close) doClose(request);
        else if (request.op == Op::Kill) doKill(request);
    }
    return 0;
}

void submit(Op op, uint32_t pid, int64_t createTime) {
    if (!g_wake) return;
    AcquireSRWLockExclusive(&g_requestLock);
    g_pending = Request{op, pid, createTime};
    ReleaseSRWLockExclusive(&g_requestLock);
    SetEvent(g_wake);
}

} // namespace

bool killStart(std::wstring* report) {
    if (g_thread) return true;
    QueryPerformanceFrequency(&g_qpcFrequency);
    g_selfPid = GetCurrentProcessId();
    g_elevated.store(tokenIsElevated());
    g_debugPrivilege.store(enableDebugPrivilege());

    g_wake = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    g_stop = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    g_thread = CreateThread(nullptr, 0, killThread, nullptr, 0, nullptr);
    if (!g_thread) {
        if (report) *report = L"rescue kill worker unavailable: could not start thread\n";
        return false;
    }
    if (report) {
        *report = std::wstring(L"rescue kill worker ready: ")
            + (g_elevated.load() ? L"elevated, " : L"not elevated, ")
            + (g_debugPrivilege.load() ? L"SeDebugPrivilege on\n" : L"SeDebugPrivilege off (same-user processes only)\n");
    }
    return true;
}

void killStop() {
    resumePaused();
    if (!g_thread) return;
    SetEvent(g_stop);
    WaitForSingleObject(g_thread, kVerifyWaitMs + 1000);
    CloseHandle(g_thread);
    CloseHandle(g_wake);
    CloseHandle(g_stop);
    g_thread = nullptr;
    g_wake = nullptr;
    g_stop = nullptr;
}

void requestClose(uint32_t pid, int64_t createTime) {
    submit(Op::Close, pid, createTime);
}

void requestKill(uint32_t pid, int64_t createTime) {
    submit(Op::Kill, pid, createTime);
}

bool pauseProcess(const ProcRecord& record, Culprit why) {
    if (isDenied(record.pid, record.image)) return false;
    const NtApi& api = ntApi();
    if (!api.suspendProcess || !api.resumeProcess) return false;

    HANDLE process = OpenProcess(PROCESS_SUSPEND_RESUME | PROCESS_TERMINATE | SYNCHRONIZE, FALSE, record.pid);
    if (!process) {
        log(L"auto-pause %u %ls refused: OpenProcess error %lu", record.pid, record.image, GetLastError());
        return false;
    }
    const NTSTATUS status = api.suspendProcess(process);
    if (status < 0) {
        log(L"auto-pause %u %ls failed: NtSuspendProcess 0x%08lX", record.pid, record.image, static_cast<unsigned long>(status));
        CloseHandle(process);
        return false;
    }

    AcquireSRWLockExclusive(&g_pauseLock);
    clearPauseLocked();
    g_pauseHandle = process;
    g_pause.active = true;
    g_pause.pid = record.pid;
    g_pause.createTime = record.createTime;
    g_pause.why = why;
    wcscpy_s(g_pause.image, record.image);
    ReleaseSRWLockExclusive(&g_pauseLock);

    log(L"auto-paused %u %ls: %ls", record.pid, record.image, culpritName(why));
    return true;
}

void resumePaused() {
    AcquireSRWLockExclusive(&g_pauseLock);
    if (!g_pause.active) {
        ReleaseSRWLockExclusive(&g_pauseLock);
        return;
    }
    const NtApi& api = ntApi();
    const NTSTATUS status = api.resumeProcess ? api.resumeProcess(g_pauseHandle) : -1;
    wchar_t text[160];
    if (status >= 0) {
        swprintf_s(text, L"resumed %u %ls (was paused: %ls)", g_pause.pid, g_pause.image, culpritName(g_pause.why));
    } else {
        swprintf_s(text, L"resume of %u %ls failed: 0x%08lX (it may have exited)", g_pause.pid, g_pause.image, static_cast<unsigned long>(status));
    }
    clearPauseLocked();
    ReleaseSRWLockExclusive(&g_pauseLock);
    report(text);
}

bool killPausedNow() {
    AcquireSRWLockExclusive(&g_pauseLock);
    if (!g_pause.active || !g_pauseHandle) {
        ReleaseSRWLockExclusive(&g_pauseLock);
        return false;
    }
    const BOOL ok = TerminateProcess(g_pauseHandle, 1);
    const DWORD error = ok ? 0 : GetLastError();
    wchar_t text[160];
    if (ok) swprintf_s(text, L"second chord press: killed paused %u %ls outright", g_pause.pid, g_pause.image);
    else swprintf_s(text, L"second chord press: TerminateProcess on %u %ls failed (error %lu)", g_pause.pid, g_pause.image, error);
    if (ok) clearPauseLocked();
    ReleaseSRWLockExclusive(&g_pauseLock);
    log(L"%ls", text);
    overlaySetResult(text);
    return ok != 0;
}

PauseInfo pausedInfo() {
    AcquireSRWLockShared(&g_pauseLock);
    PauseInfo copy = g_pause;
    ReleaseSRWLockShared(&g_pauseLock);
    return copy;
}

bool isDenied(uint32_t pid, const wchar_t* image) {
    if (pid == 4 || pid == g_selfPid) return true;
    for (const wchar_t* name : kDenied) {
        if (_wcsicmp(name, image) == 0) return true;
    }
    return false;
}

KillStatus killStatus() {
    KillStatus status;
    status.started = g_thread != nullptr;
    status.elevated = g_elevated.load();
    status.debugPrivilege = g_debugPrivilege.load();
    return status;
}

} // namespace rescue
