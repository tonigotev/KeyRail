// main.cpp -- configurable hotkey daemon.
// The main thread owns the Raw Input sink window and GetMessage. The pipe
// thread only posts control messages back here so reloads happen on the same
// thread as hotkeys.
//
// Key detection is Raw Input only (WM_INPUT with RIDEV_INPUTSINK). The daemon
// never calls RegisterHotKey or SetWindowsHookEx, so it stays a passive
// observer of the keyboard instead of an input path anti-cheat drivers block.

#include <windows.h>
#include <cstdio>
#include <mutex>
#include <string>
#include <vector>

#include "browser_bridge.h"
#include "clipboard_history.h"
#include "config.h"
#include "control_pipe.h"
#include "discord_bridge.h"
#include "display_state.h"
#include "hotkey_registry.h"
#include "media_sessions.h"

static constexpr UINT WM_HOTKEYD_RELOAD = WM_APP + 1;
static constexpr UINT WM_HOTKEYD_QUIT = WM_APP + 2;
static constexpr UINT WM_HOTKEYD_SUSPEND = WM_APP + 3;
static constexpr UINT WM_HOTKEYD_RESUME = WM_APP + 4;
static constexpr wchar_t kSingleInstanceMutex[] = L"Local\\HotkeyToCommandHotkeyd";
static constexpr wchar_t kRawInputWindowClass[] = L"HotkeyToCommandRawInputSink";

struct DaemonState {
    HotkeyRegistry registry;
    AppConfig activeConfig;
    std::wstring status;
    std::wstring rawInputStatus;
    std::mutex statusMutex;
};

static void handleRawInput(HotkeyRegistry& registry, HRAWINPUT input) {
    UINT size = 0;
    if (GetRawInputData(input, RID_INPUT, nullptr, &size, sizeof(RAWINPUTHEADER)) != 0 || size == 0) {
        return;
    }

    std::vector<BYTE> buffer(size);
    UINT copied = GetRawInputData(input, RID_INPUT, buffer.data(), &size, sizeof(RAWINPUTHEADER));
    if (copied == static_cast<UINT>(-1) || copied != size) return;

    RAWINPUT* raw = reinterpret_cast<RAWINPUT*>(buffer.data());
    if (raw->header.dwType != RIM_TYPEKEYBOARD) return;

    const RAWKEYBOARD& keyboard = raw->data.keyboard;
    if (keyboard.VKey == 0 || keyboard.VKey == 0xff) return;

    const bool pressed = (keyboard.Flags & RI_KEY_BREAK) == 0;
    registry.dispatchRawKey(keyboard.VKey, pressed, raw->header.hDevice);

    if (registry.consumeQuitRequest()) PostQuitMessage(0);
}

static LRESULT CALLBACK rawInputWindowProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
    if (message == WM_NCCREATE) {
        auto* create = reinterpret_cast<CREATESTRUCTW*>(lParam);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(create->lpCreateParams));
    }

    auto* registry = reinterpret_cast<HotkeyRegistry*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (message == WM_INPUT && registry) {
        (void)wParam;
        handleRawInput(*registry, reinterpret_cast<HRAWINPUT>(lParam));
        return 0;
    }

    return DefWindowProcW(hwnd, message, wParam, lParam);
}

static HWND createRawInputWindow(HotkeyRegistry& registry, std::wstring& status) {
    HINSTANCE instance = GetModuleHandleW(nullptr);
    WNDCLASSW wc{};
    wc.lpfnWndProc = rawInputWindowProc;
    wc.hInstance = instance;
    wc.lpszClassName = kRawInputWindowClass;

    if (!RegisterClassW(&wc) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
        status = L"raw input unavailable: RegisterClass failed\n";
        return nullptr;
    }

    HWND hwnd = CreateWindowExW(
        0,
        kRawInputWindowClass,
        L"",
        0,
        0,
        0,
        0,
        0,
        HWND_MESSAGE,
        nullptr,
        instance,
        &registry);

    if (!hwnd) {
        status = L"raw input unavailable: message window failed\n";
        return nullptr;
    }

    // usUsagePage 0x01 / usUsage 0x06 is the HID generic-desktop keyboard page.
    // RIDEV_INPUTSINK keeps WM_INPUT flowing to this message-only window even
    // while a game owns the foreground.
    RAWINPUTDEVICE device{};
    device.usUsagePage = 0x01;
    device.usUsage = 0x06;
    device.dwFlags = RIDEV_INPUTSINK;
    device.hwndTarget = hwnd;

    if (!RegisterRawInputDevices(&device, 1, sizeof(device))) {
        DestroyWindow(hwnd);
        status = L"raw input unavailable: RegisterRawInputDevices failed\n";
        return nullptr;
    }

    status = L"raw input armed: keyboard RIDEV_INPUTSINK\n";
    return hwnd;
}

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
    status += state.registry.rawInputDebugStatus();
    status += describeOverlayVisibility();
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
    status += state.rawInputStatus;
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

    wprintf(L"hotkeyd starting\n");
    HWND rawInputWindow = createRawInputWindow(state.registry, state.rawInputStatus);
    if (!rawInputWindow) {
        wprintf(L"%s", state.rawInputStatus.c_str());
        wprintf(L"no key detection backend available, exiting\n");
        ReleaseMutex(singleInstance);
        CloseHandle(singleInstance);
        return 1;
    }
    startDiscordBridge();
    startBrowserMediaBridge();
    startClipboardHistory();
    warmMediaOverlay();
    startMediaTargetPolling();

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
            // Stop matching bindings so the UI can record a combo that is
            // already bound without firing its action.
            state.registry.clear();
            setStatus(state, L"hotkeys suspended for rebinding\n");
            continue;
        }

        if (msg.message == WM_HOTKEYD_RESUME) {
            std::wstring status = state.registry.apply(state.activeConfig);
            status += state.rawInputStatus;
            setStatus(state, status);
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
    stopMediaTargetPolling();
    stopClipboardHistory();
    stopBrowserMediaBridge();
    stopDiscordBridge();
    state.registry.clear();
    if (rawInputWindow) DestroyWindow(rawInputWindow);
    ReleaseMutex(singleInstance);
    CloseHandle(singleInstance);
    wprintf(L"bye\n");
    return 0;
}
