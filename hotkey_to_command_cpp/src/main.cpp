// main.cpp -- configurable hotkey daemon.
// The main thread owns RegisterHotKey/GetMessage. The pipe thread only posts
// control messages back here so reloads happen on the same thread as hotkeys.

#include <windows.h>
#include <cstdio>
#include <mutex>
#include <string>

#include "browser_bridge.h"
#include "config.h"
#include "control_pipe.h"
#include "discord_bridge.h"
#include "hotkey_registry.h"

static constexpr UINT WM_HOTKEYD_RELOAD = WM_APP + 1;
static constexpr UINT WM_HOTKEYD_QUIT = WM_APP + 2;
static constexpr int QUIT_HOTKEY_ID = 9000;

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
    return state.status;
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
    DWORD mainThreadId = GetCurrentThreadId();
    MSG bootstrapMsg;
    PeekMessageW(&bootstrapMsg, nullptr, WM_USER, WM_USER, PM_NOREMOVE);

    DaemonState state;

    RegisterHotKey(nullptr, QUIT_HOTKEY_ID, MOD_CONTROL | MOD_ALT | MOD_NOREPEAT, 'Q');
    wprintf(L"hotkeyd starting\n");
    startDiscordBridge();
    startBrowserMediaBridge();

    loadAndApply(state, false);

    ControlPipe pipe(
        mainThreadId,
        WM_HOTKEYD_RELOAD,
        WM_HOTKEYD_QUIT,
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

        if (msg.message == WM_HOTKEY) {
            if (static_cast<int>(msg.wParam) == QUIT_HOTKEY_ID) break;
            state.registry.dispatch(msg.wParam);
        }
    }

    pipe.stop();
    stopBrowserMediaBridge();
    stopDiscordBridge();
    state.registry.clear();
    UnregisterHotKey(nullptr, QUIT_HOTKEY_ID);
    wprintf(L"bye\n");
    return 0;
}
