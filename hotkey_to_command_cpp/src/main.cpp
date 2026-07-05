// main.cpp -- configurable hotkey daemon.
// The main thread owns RegisterHotKey/GetMessage. The pipe thread only posts
// control messages back here so reloads happen on the same thread as hotkeys.

#include <windows.h>
#include <cstdio>
#include <mutex>
#include <string>

#include "browser_bridge.h"
#include "clipboard_history.h"
#include "config.h"
#include "control_pipe.h"
#include "discord_bridge.h"
#include "hotkey_registry.h"

static constexpr UINT WM_HOTKEYD_RELOAD = WM_APP + 1;
static constexpr UINT WM_HOTKEYD_QUIT = WM_APP + 2;
static constexpr UINT WM_HOTKEYD_SUSPEND = WM_APP + 3;
static constexpr UINT WM_HOTKEYD_RESUME = WM_APP + 4;
static constexpr int QUIT_HOTKEY_ID = 9000;
static constexpr wchar_t kSingleInstanceMutex[] = L"Local\\HotkeyToCommandHotkeyd";

struct DaemonState {
    HotkeyRegistry registry;
    AppConfig activeConfig;
    std::wstring status;
    std::mutex statusMutex;
};

static void setStatus(DaemonState& state, const std::wstring& status) {
    std::lock_guard<std::mutex> lock(state.statusMutex);
    state.status = status;
}

static std::wstring getStatus(DaemonState& state) {
    std::lock_guard<std::mutex> lock(state.statusMutex);
    std::wstring status = state.status;
    HANDLE token = nullptr;
    TOKEN_ELEVATION elevation{};
    DWORD returned = 0;
    bool elevated = false;
    if (OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) {
        if (GetTokenInformation(token, TokenElevation, &elevation, sizeof(elevation), &returned)) {
            elevated = elevation.TokenIsElevated != 0;
        }
        CloseHandle(token);
    }
    status += std::wstring(L"daemon elevated: ") + (elevated ? L"yes\n" : L"no\n");
    std::wstring event = state.registry.lastEvent();
    if (!event.empty()) status += event + L"\n";
    return status;
}

static void loadAndApply(DaemonState& state, bool keepPreviousOnFailure) {
    ConfigLoadResult loaded = loadConfig();
    if (!loaded.ok) {
        std::wstring message = L"config load failed: " + loaded.message + L"\npath: " + loaded.path + L"\n";
        if (keepPreviousOnFailure) {
            message += L"kept previous working bindings\n";
        } else {
            state.registry.clear();
        }
        setStatus(state, message);
        wprintf(L"%s", message.c_str());
        return;
    }

    state.activeConfig = std::move(loaded.config);
    std::wstring status = state.registry.apply(state.activeConfig);
    status += L"path: " + loaded.path + L"\n";
    setStatus(state, status);
    wprintf(L"%s", status.c_str());
}

int main() {
    HANDLE singleInstance = CreateMutexW(nullptr, TRUE, kSingleInstanceMutex);
    if (!singleInstance) {
        return 1;
    }
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        CloseHandle(singleInstance);
        return 0;
    }

    DWORD mainThreadId = GetCurrentThreadId();
    MSG bootstrapMsg;
    PeekMessageW(&bootstrapMsg, nullptr, WM_USER, WM_USER, PM_NOREMOVE);

    DaemonState state;

    RegisterHotKey(nullptr, QUIT_HOTKEY_ID, MOD_CONTROL | MOD_ALT | MOD_NOREPEAT, 'Q');
    wprintf(L"hotkeyd starting\n");
    startDiscordBridge();
    startBrowserMediaBridge();
    startClipboardHistory();

    loadAndApply(state, false);

    ControlPipe pipe(
        mainThreadId,
        WM_HOTKEYD_RELOAD,
        WM_HOTKEYD_QUIT,
        WM_HOTKEYD_SUSPEND,
        WM_HOTKEYD_RESUME,
        [&state] { return getStatus(state); });
    pipe.start();

    wprintf(L"control pipe: \\\\.\\pipe\\hotkeyd-control\n");
    wprintf(L"Ctrl+Alt+Q quits this debug console daemon\n\n");

    MSG msg;
    BOOL r;
    while ((r = GetMessageW(&msg, nullptr, 0, 0)) != 0) {
        if (r == -1) break;

        if (msg.message == WM_HOTKEYD_RELOAD) {
            wprintf(L"\nreload requested\n");
            loadAndApply(state, true);
            continue;
        }

        if (msg.message == WM_HOTKEYD_QUIT) {
            break;
        }

        if (msg.message == WM_HOTKEYD_SUSPEND) {
            // Temporarily release all global hotkeys so the UI can record a combo
            // that is currently in use (RegisterHotKey otherwise swallows it).
            state.registry.clear();
            setStatus(state, L"hotkeys suspended for rebinding\n");
            continue;
        }

        if (msg.message == WM_HOTKEYD_RESUME) {
            std::wstring status = state.registry.apply(state.activeConfig);
            setStatus(state, status);
            continue;
        }

        if (msg.message == WM_HOTKEY) {
            if (static_cast<int>(msg.wParam) == QUIT_HOTKEY_ID) break;
            state.registry.dispatch(msg.wParam);
            continue;
        }

        if (msg.message == WM_TIMER) {
            state.registry.handleTimer(msg.wParam);
            continue;
        }

        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    pipe.stop();
    stopClipboardHistory();
    stopBrowserMediaBridge();
    stopDiscordBridge();
    state.registry.clear();
    UnregisterHotKey(nullptr, QUIT_HOTKEY_ID);
    ReleaseMutex(singleInstance);
    CloseHandle(singleInstance);
    wprintf(L"bye\n");
    return 0;
}
