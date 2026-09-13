#include "rescue_trigger.h"

#include "rescue_log.h"

#include <atomic>

namespace rescue {
namespace {

constexpr int kHotkeyId = 0x5245;   // 'RE'; the registry's ids start at 1 and 10000
constexpr UINT WM_TRIGGER_APPLY = WM_APP + 60;
constexpr UINT WM_TRIGGER_SUSPEND = WM_APP + 61;
constexpr UINT WM_TRIGGER_RESUME = WM_APP + 62;

HANDLE g_thread = nullptr;
DWORD g_threadId = 0;
HANDLE g_ready = nullptr;
HANDLE g_trigger = nullptr;

SRWLOCK g_comboLock = SRWLOCK_INIT;
HotkeyCombo g_combo;                 // written under g_comboLock
std::atomic<UINT> g_comboVk{0};      // mirrored for the raw path, no lock needed there
std::atomic<UINT> g_comboMods{0};
std::atomic<bool> g_claimed{false};
std::atomic<bool> g_suspended{false};

// Modifier state as seen by the raw stream. Bits match MOD_CONTROL/ALT/SHIFT/WIN.
std::atomic<UINT> g_rawMods{0};
std::atomic<int64_t> g_lastTriggerQpc{0};
std::atomic<PressHook> g_pressHook{nullptr};

void fire() {
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    g_lastTriggerQpc.store(now.QuadPart);
    if (g_trigger) SetEvent(g_trigger);
    PressHook hook = g_pressHook.load();
    if (hook) hook();
}

UINT modifierBit(UINT vk) {
    switch (vk) {
    case VK_CONTROL: case VK_LCONTROL: case VK_RCONTROL: return MOD_CONTROL;
    case VK_MENU: case VK_LMENU: case VK_RMENU: return MOD_ALT;
    case VK_SHIFT: case VK_LSHIFT: case VK_RSHIFT: return MOD_SHIFT;
    case VK_LWIN: case VK_RWIN: return MOD_WIN;
    default: return 0;
    }
}

bool registerCombo() {
    HotkeyCombo combo;
    AcquireSRWLockShared(&g_comboLock);
    combo = g_combo;
    ReleaseSRWLockShared(&g_comboLock);
    if (!combo.ok) {
        g_claimed.store(false);
        return false;
    }
    const bool ok = RegisterHotKey(nullptr, kHotkeyId, combo.mods | MOD_NOREPEAT, combo.vk) != 0;
    g_claimed.store(ok);
    if (ok) {
        log(L"rescue hotkey claimed: %ls", combo.pretty.c_str());
    } else {
        const DWORD error = GetLastError();
        log(L"rescue hotkey %ls not claimed (RegisterHotKey %ls); raw input only",
            combo.pretty.c_str(),
            error == ERROR_HOTKEY_ALREADY_REGISTERED ? L"already in use" : L"failed");
    }
    return ok;
}

void unregisterCombo() {
    if (g_claimed.exchange(false)) UnregisterHotKey(nullptr, kHotkeyId);
}

DWORD WINAPI triggerThread(LPVOID) {
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);
    SetThreadDescription(GetCurrentThread(), L"keyrail-rescue-trigger");

    // Force the queue into existence before anyone PostThreadMessages to us.
    MSG bootstrap;
    PeekMessageW(&bootstrap, nullptr, WM_USER, WM_USER, PM_NOREMOVE);

    registerCombo();
    SetEvent(g_ready);

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        switch (msg.message) {
        case WM_HOTKEY:
            // The whole job. Anything slower risks queueing behind ourselves.
            if (msg.wParam == static_cast<WPARAM>(kHotkeyId)) fire();
            break;
        case WM_TRIGGER_APPLY:
            unregisterCombo();
            if (!g_suspended.load()) registerCombo();
            break;
        case WM_TRIGGER_SUSPEND:
            unregisterCombo();
            break;
        case WM_TRIGGER_RESUME:
            registerCombo();
            break;
        default:
            break;
        }
    }
    unregisterCombo();
    return 0;
}

void setCombo(const std::wstring& hotkey) {
    HotkeyCombo combo = parseHotkeyCombo(hotkey);
    AcquireSRWLockExclusive(&g_comboLock);
    g_combo = combo;
    ReleaseSRWLockExclusive(&g_comboLock);
    g_comboVk.store(combo.ok ? combo.vk : 0);
    g_comboMods.store(combo.ok ? combo.mods : 0);
    if (!combo.ok) log(L"rescue hotkey '%ls' invalid: %ls", hotkey.c_str(), combo.error.c_str());
}

} // namespace

bool triggerStart(const std::wstring& hotkey, HANDLE trigger, std::wstring* report) {
    if (g_thread) return true;
    g_trigger = trigger;
    setCombo(hotkey);

    g_ready = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    g_thread = CreateThread(nullptr, 0, triggerThread, nullptr, 0, &g_threadId);
    if (!g_thread) {
        if (report) *report = L"rescue trigger unavailable: could not start thread\n";
        return false;
    }
    WaitForSingleObject(g_ready, 2000);

    if (report) {
        HotkeyCombo combo = triggerCombo();
        if (!combo.ok) {
            *report = L"rescue hotkey invalid: " + combo.error + L"\n";
        } else {
            *report = L"rescue hotkey " + combo.pretty
                + (g_claimed.load() ? L" (claimed + raw input)\n" : L" (raw input only, RegisterHotKey refused)\n");
        }
    }
    return true;
}

void triggerStop() {
    if (!g_thread) return;
    PostThreadMessageW(g_threadId, WM_QUIT, 0, 0);
    WaitForSingleObject(g_thread, 3000);
    CloseHandle(g_thread);
    g_thread = nullptr;
    g_threadId = 0;
    if (g_ready) {
        CloseHandle(g_ready);
        g_ready = nullptr;
    }
}

void triggerApply(const std::wstring& hotkey) {
    setCombo(hotkey);
    if (g_threadId) PostThreadMessageW(g_threadId, WM_TRIGGER_APPLY, 0, 0);
}

void triggerSuspend() {
    g_suspended.store(true);
    if (g_threadId) PostThreadMessageW(g_threadId, WM_TRIGGER_SUSPEND, 0, 0);
}

void triggerResume() {
    g_suspended.store(false);
    if (g_threadId) PostThreadMessageW(g_threadId, WM_TRIGGER_RESUME, 0, 0);
}

bool observeRawKey(UINT vk, bool pressed) {
    const UINT bit = modifierBit(vk);
    if (bit) {
        if (pressed) g_rawMods.fetch_or(bit);
        else g_rawMods.fetch_and(~bit);
        return false;
    }

    const UINT chordVk = g_comboVk.load();
    if (chordVk == 0 || vk != chordVk) return false;
    const UINT needed = g_comboMods.load();
    if ((g_rawMods.load() & needed) != needed) return false;

    if (pressed && !g_suspended.load()) fire();
    return true;
}

void fireTrigger() {
    fire();
}

void triggerSetPressHook(PressHook hook) {
    g_pressHook.store(hook);
}

int64_t lastTriggerQpc() {
    return g_lastTriggerQpc.load();
}

bool triggerClaimed() {
    return g_claimed.load();
}

HotkeyCombo triggerCombo() {
    AcquireSRWLockShared(&g_comboLock);
    HotkeyCombo combo = g_combo;
    ReleaseSRWLockShared(&g_comboLock);
    return combo;
}

} // namespace rescue
