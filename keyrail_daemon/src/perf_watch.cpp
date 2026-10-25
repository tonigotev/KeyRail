#include "perf_watch.h"

#include <atomic>
#include <cstdio>
#include <mutex>

namespace perf {
namespace {

constexpr uint32_t kSlots = 48;
constexpr uint32_t kTextChars = 160;
constexpr DWORD kHeartbeatMs = 250;
// A wake-up this late means the machine itself stopped scheduling us.
constexpr uint32_t kStallMs = 1200;

struct Entry {
    wchar_t text[kTextChars];
};

constexpr uint32_t kTallySlots = 16;

// String literals, compared by pointer: the call sites pass the same literal
// every time, so this needs no hashing and no allocation.
struct Tally {
    const wchar_t* what = nullptr;
    uint64_t calls = 0;
    uint64_t totalMs = 0;
    uint32_t maxMs = 0;
};

Tally g_tally[kTallySlots];
ULONGLONG g_tallySince = 0;

std::mutex g_mutex;
Entry g_entries[kSlots];
uint32_t g_head = 0;
uint32_t g_count = 0;

HANDLE g_heartbeatThread = nullptr;
HANDLE g_heartbeatStop = nullptr;
LARGE_INTEGER g_frequency{};
std::atomic<uint32_t> g_busyUntilTick{0};   // set while a KeyRail operation is running

std::wstring logPath() {
    wchar_t appData[MAX_PATH];
    const DWORD chars = GetEnvironmentVariableW(L"APPDATA", appData, MAX_PATH);
    if (chars == 0 || chars >= MAX_PATH) return L"";
    return std::wstring(appData) + L"\\KeyRail\\perf.log";
}

void appendToFile(const wchar_t* line) {
    static std::wstring path = logPath();
    if (path.empty()) return;

    // Keep the file from growing without bound; these lines are rare.
    HANDLE file = CreateFileW(path.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ, nullptr,
        OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return;
    LARGE_INTEGER size{};
    if (GetFileSizeEx(file, &size) && size.QuadPart > (1 << 20)) {
        CloseHandle(file);
        DeleteFileW(path.c_str());
        file = CreateFileW(path.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ, nullptr,
            OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file == INVALID_HANDLE_VALUE) return;
    }

    char utf8[kTextChars * 3];
    const int bytes = WideCharToMultiByte(CP_UTF8, 0, line, -1, utf8, sizeof(utf8), nullptr, nullptr);
    if (bytes > 1) {
        DWORD written = 0;
        WriteFile(file, utf8, static_cast<DWORD>(bytes - 1), &written, nullptr);
        WriteFile(file, "\r\n", 2, &written, nullptr);
    }
    CloseHandle(file);
}

int64_t now() {
    if (g_frequency.QuadPart == 0) QueryPerformanceFrequency(&g_frequency);
    LARGE_INTEGER value;
    QueryPerformanceCounter(&value);
    return value.QuadPart;
}

uint32_t millisSince(int64_t start) {
    if (g_frequency.QuadPart == 0) return 0;
    return static_cast<uint32_t>((now() - start) * 1000 / g_frequency.QuadPart);
}

DWORD WINAPI heartbeatThread(LPVOID) {
    SetThreadDescription(GetCurrentThread(), L"keyrail-perf-heartbeat");
    ULONGLONG previous = GetTickCount64();
    for (;;) {
        if (WaitForSingleObject(g_heartbeatStop, kHeartbeatMs) == WAIT_OBJECT_0) break;
        const ULONGLONG current = GetTickCount64();
        const ULONGLONG elapsed = current - previous;
        previous = current;
        if (elapsed >= kStallMs) {
            // Was KeyRail in the middle of something when the machine stopped?
            const uint32_t busyAt = g_busyUntilTick.load();
            const bool ours = busyAt != 0 && static_cast<ULONGLONG>(busyAt) + elapsed >= current;
            wchar_t text[kTextChars];
            swprintf_s(text, L"the whole machine stalled for %llu ms (%ls)", elapsed,
                ours ? L"KeyRail was mid-operation; see the line above"
                     : L"nothing in KeyRail was running - the stall came from elsewhere");
            note(text, 0);
        }
    }
    return 0;
}

} // namespace

void note(const wchar_t* what, uint32_t millis) {
    SYSTEMTIME time{};
    GetLocalTime(&time);
    wchar_t line[kTextChars];
    if (millis > 0) {
        swprintf_s(line, L"%02u:%02u:%02u  %ls took %u ms", time.wHour, time.wMinute, time.wSecond, what, millis);
    } else {
        swprintf_s(line, L"%02u:%02u:%02u  %ls", time.wHour, time.wMinute, time.wSecond, what);
    }

    {
        std::lock_guard<std::mutex> lock(g_mutex);
        wcscpy_s(g_entries[g_head].text, line);
        g_head = (g_head + 1) % kSlots;
        if (g_count < kSlots) ++g_count;
    }
    wprintf(L"slow: %ls\n", line);
    appendToFile(line);
}

Scope::Scope(const wchar_t* what, uint32_t thresholdMs)
    : what_(what), thresholdMs_(thresholdMs), start_(now()) {
    g_busyUntilTick.store(static_cast<uint32_t>(GetTickCount64()));
}

Scope::~Scope() {
    const uint32_t elapsed = millisSince(start_);
    g_busyUntilTick.store(0);
    tally(what_, elapsed);
    if (elapsed >= thresholdMs_) note(what_, elapsed);
}

void tally(const wchar_t* what, uint32_t millis) {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_tallySince == 0) g_tallySince = GetTickCount64();
    for (uint32_t i = 0; i < kTallySlots; ++i) {
        Tally& entry = g_tally[i];
        if (entry.what == nullptr) {
            entry.what = what;
        } else if (entry.what != what) {
            continue;
        }
        ++entry.calls;
        entry.totalMs += millis;
        if (millis > entry.maxMs) entry.maxMs = millis;
        return;
    }
}

void startHeartbeat() {
    if (g_heartbeatThread) return;
    QueryPerformanceFrequency(&g_frequency);
    g_heartbeatStop = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    g_heartbeatThread = CreateThread(nullptr, 0, heartbeatThread, nullptr, 0, nullptr);
}

void stopHeartbeat() {
    if (!g_heartbeatThread) return;
    SetEvent(g_heartbeatStop);
    WaitForSingleObject(g_heartbeatThread, 2000);
    CloseHandle(g_heartbeatThread);
    CloseHandle(g_heartbeatStop);
    g_heartbeatThread = nullptr;
    g_heartbeatStop = nullptr;
}

std::wstring describe() {
    std::lock_guard<std::mutex> lock(g_mutex);
    std::wstring out;

    // Where the steady cost goes. A spike log alone cannot explain an
    // operation that costs 100 ms every second and never looks slow.
    if (g_tallySince) {
        const ULONGLONG windowMs = GetTickCount64() - g_tallySince;
        out += L"where the daemon's time goes (last " + std::to_wstring(windowMs / 1000) + L" s):\n";
        for (uint32_t i = 0; i < kTallySlots; ++i) {
            const Tally& entry = g_tally[i];
            if (!entry.what || entry.calls == 0) continue;
            const double share = windowMs ? (100.0 * static_cast<double>(entry.totalMs) / static_cast<double>(windowMs)) : 0.0;
            wchar_t line[240];
            swprintf_s(line, L"  %-52ls %6llu calls  avg %4llu ms  max %5u ms  %5.1f%% of one core\n",
                entry.what, entry.calls, entry.totalMs / entry.calls, entry.maxMs, share);
            out += line;
        }
    }

    if (g_count == 0) return out;
    out += L"slow operations (newest last, also in %APPDATA%\\KeyRail\\perf.log):\n";
    for (uint32_t i = 0; i < g_count; ++i) {
        const uint32_t index = (g_head + kSlots - g_count + i) % kSlots;
        out += L"  ";
        out += g_entries[index].text;
        out += L"\n";
    }
    return out;
}

} // namespace perf
