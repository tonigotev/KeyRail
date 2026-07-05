#include "hotkey_registry.h"

#include "action_factory.h"
#include "audio.h"
#include "clipboard_history.h"
#include "hotkey.h"
#include "media_sessions.h"
#include "synthetic_hotkey_suppression.h"

#include <windows.h>
#include <cstdio>
#include <sstream>
#include <exception>
#include <initializer_list>
#include <thread>
#include <unordered_set>

static HotkeyRegistry* g_keyboardHookRegistry = nullptr;
static const wchar_t* kPushToTalkOverlayClass = L"HotkeyToCommandPushToTalkOverlay";

static LRESULT CALLBACK keyboardHookProc(int code, WPARAM wParam, LPARAM lParam) {
    if (code == HC_ACTION && g_keyboardHookRegistry && lParam) {
        const auto* event = reinterpret_cast<const KBDLLHOOKSTRUCT*>(lParam);
        if (g_keyboardHookRegistry->handleKeyboardEvent(wParam, *event)) {
            return 1;
        }
    }
    return CallNextHookEx(nullptr, code, wParam, lParam);
}

static LRESULT CALLBACK pushToTalkOverlayProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
    if (message == WM_NCCREATE) {
        auto* create = reinterpret_cast<CREATESTRUCTW*>(lParam);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(create->lpCreateParams));
    }

    auto* registry = reinterpret_cast<HotkeyRegistry*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (message == WM_PAINT && registry) {
        registry->paintPushToTalkOverlay();
        return 0;
    }

    return DefWindowProcW(hwnd, message, wParam, lParam);
}

static RECT foregroundOverlayBounds() {
    HWND foreground = GetForegroundWindow();
    HMONITOR monitor = foreground ? MonitorFromWindow(foreground, MONITOR_DEFAULTTONEAREST) : nullptr;
    if (!monitor) {
        POINT cursor{};
        GetCursorPos(&cursor);
        monitor = MonitorFromPoint(cursor, MONITOR_DEFAULTTONEAREST);
    }

    MONITORINFO info{};
    info.cbSize = sizeof(info);
    if (!monitor || !GetMonitorInfoW(monitor, &info)) {
        return RECT{0, 0, GetSystemMetrics(SM_CXSCREEN), GetSystemMetrics(SM_CYSCREEN)};
    }

    if (foreground) {
        RECT foregroundRect{};
        if (GetWindowRect(foreground, &foregroundRect)) {
            const bool coversMonitor =
                foregroundRect.left <= info.rcMonitor.left + 2
                && foregroundRect.top <= info.rcMonitor.top + 2
                && foregroundRect.right >= info.rcMonitor.right - 2
                && foregroundRect.bottom >= info.rcMonitor.bottom - 2;
            if (coversMonitor) return info.rcMonitor;
        }
    }
    return info.rcWork;
}

HotkeyRegistry::~HotkeyRegistry() {
    clear();
}

void HotkeyRegistry::clearRegisteredIds(std::vector<int>& ids) {
    for (int id : ids) {
        UnregisterHotKey(nullptr, id);
    }
    ids.clear();
}

void HotkeyRegistry::clear() {
    uninstallKeyboardHook(true);

    if (commandTimerId_ != 0) {
        KillTimer(nullptr, commandTimerId_);
        commandTimerId_ = 0;
    }

    clearRegisteredIds(temporaryIds_);
    clearRegisteredIds(registeredIds_);
    dispatch_.clear();
    actions_.clear();
    hotkeyVk_.clear();
    hotkeyLabels_.clear();
    hotkeyPretty_.clear();
    preparedBindings_.clear();
    pushToTalkBindings_.clear();
    pushToTalkOverlayEnabled_ = false;
    commandMode_ = false;
    commandActive_ = false;
    commandActivationId_ = 0;
}

static int clampTimeout(int timeoutMs) {
    if (timeoutMs < 1000) return 1000;
    if (timeoutMs > 15000) return 15000;
    return timeoutMs;
}

static bool buildPreparedBindings(
    const AppConfig& config,
    std::vector<std::unique_ptr<Runnable>>& actions,
    std::vector<HotkeyRegistry::PreparedBinding>& prepared,
    std::vector<HotkeyRegistry::PushToTalkBinding>& pushToTalk,
    std::wstringstream& status) {
    bool any = false;
    std::unordered_set<unsigned long long> seenHotkeys;

    for (const BindingSpec& binding : config.bindings) {
        std::wstring label = binding.id.empty() ? binding.hotkey : binding.id;

        if (!binding.enabled) {
            status << L"skip " << label << L": disabled\n";
            continue;
        }

        HotkeyCombo combo = parseHotkeyCombo(binding.hotkey);
        if (!combo.ok) {
            status << L"skip " << label << L": " << combo.error << L"\n";
            continue;
        }

        if (!seenHotkeys.insert(combo.key()).second) {
            status << L"skip " << label << L": duplicate hotkey\n";
            continue;
        }

        if (binding.action.type == L"builtin"
            && (binding.action.name == L"push_to_talk" || binding.action.name == L"push_to_mute")) {
            const bool muteWhileHeld = binding.action.name == L"push_to_mute";
            pushToTalk.push_back({combo, label, false, binding.action.pushToTalkOverlay, muteWhileHeld});
            status << (muteWhileHeld ? L"push-to-mute " : L"push-to-talk ")
                   << combo.pretty << L" -> " << label << L"\n";
            any = true;
            continue;
        }

        ActionBuildResult built = makeAction(binding.action);
        if (!built.action) {
            status << L"skip " << label << L": " << built.error << L"\n";
            continue;
        }

        Runnable* action = built.action.get();
        actions.push_back(std::move(built.action));
        prepared.push_back({combo, label, action});
        any = true;
    }

    return any;
}

std::wstring HotkeyRegistry::apply(const AppConfig& config) {
    clear();

    std::wstringstream status;
    status << L"config version " << config.version << L"\n";

    commandMode_ = config.settings.hotkeyMode == L"command";
    commandTimeoutMs_ = clampTimeout(config.settings.commandTimeoutMs);

    if (!buildPreparedBindings(config, actions_, preparedBindings_, pushToTalkBindings_, status)) {
        status << L"no bindings ready\n";
    }

    if (!pushToTalkBindings_.empty()) {
        pushToTalkIdleMuted_ = true;
        for (const auto& binding : pushToTalkBindings_) {
            pushToTalkOverlayEnabled_ = pushToTalkOverlayEnabled_ || binding.showOverlay;
            if (binding.muteWhileHeld) pushToTalkIdleMuted_ = false;
        }
        if (getDefaultMicrophoneMute(&pushToTalkPreviousMute_)) {
            pushToTalkHadPreviousMute_ = true;
        }
        if (installKeyboardHook()) {
            if (pushToTalkOverlayEnabled_ && !ensurePushToTalkOverlay()) {
                status << L"push-to-talk overlay disabled: window failed\n";
            }
            setPushToTalkMute(pushToTalkIdleMuted_, L"held mic mode idle");
            status << L"held mic bindings armed: " << pushToTalkBindings_.size() << L"\n";
        } else {
            status << L"held mic mode disabled: keyboard hook failed\n";
            pushToTalkBindings_.clear();
        }
    }

    // The low-level keyboard hook also lets Escape dismiss the media picker and
    // clipboard history picker without a dedicated binding. Ensure it is armed
    // even when there are no push-to-talk bindings.
    if (!keyboardHook_) {
        if (installKeyboardHook()) {
            status << L"esc-dismiss hook armed\n";
        } else {
            status << L"esc-dismiss hook unavailable\n";
        }
    }

    int nextId = 1;
    int armed = 0;

    if (commandMode_) {
        HotkeyCombo activation = parseHotkeyCombo(config.settings.commandHotkey);
        if (!activation.ok) {
            status << L"command mode disabled: launcher hotkey " << activation.error << L"\n";
            return status.str();
        }

        commandActivationId_ = nextId++;
        if (!RegisterHotKey(nullptr, commandActivationId_, activation.mods | MOD_NOREPEAT, activation.vk)) {
            DWORD error = GetLastError();
            status << L"command mode disabled: launcher "
                   << (error == ERROR_HOTKEY_ALREADY_REGISTERED ? L"already in use" : L"register failed")
                   << L"\n";
            commandActivationId_ = 0;
            return status.str();
        }

        registeredIds_.push_back(commandActivationId_);
        hotkeyVk_[commandActivationId_] = activation.vk;
        hotkeyLabels_[commandActivationId_] = L"command mode launcher";
        hotkeyPretty_[commandActivationId_] = activation.pretty;
        status << L"command mode launcher " << activation.pretty << L"\n";
        status << L"ready bindings: " << preparedBindings_.size() << L"\n";
        status << L"timeout: " << commandTimeoutMs_ << L"ms\n";
        return status.str();
    }

    for (const PreparedBinding& prepared : preparedBindings_) {
        int id = nextId++;
        if (!RegisterHotKey(nullptr, id, prepared.combo.mods | MOD_NOREPEAT, prepared.combo.vk)) {
            DWORD error = GetLastError();
            status << L"skip " << prepared.label << L": "
                   << (error == ERROR_HOTKEY_ALREADY_REGISTERED ? L"already in use" : L"register failed")
                   << L"\n";
            continue;
        }

        dispatch_[id] = prepared.action;
        hotkeyVk_[id] = prepared.combo.vk;
        hotkeyLabels_[id] = prepared.label;
        hotkeyPretty_[id] = prepared.combo.pretty;
        registeredIds_.push_back(id);
        ++armed;

        status << L"armed " << prepared.combo.pretty << L" -> " << prepared.label << L"\n";
    }

    if (armed == 0) status << L"no hotkeys armed\n";
    return status.str();
}

bool HotkeyRegistry::runAction(Runnable* action) const {
    if (!action) return false;
    std::thread([action] {
        try {
            action->run();
        } catch (const std::exception& ex) {
            wprintf(L"hotkey action failed: %S\n", ex.what());
        } catch (...) {
            wprintf(L"hotkey action failed: unknown exception\n");
        }
    }).detach();
    return true;
}

bool HotkeyRegistry::activateCommandMode() {
    if (commandActive_) {
        resetCommandTimer();
        wprintf(L"command mode refreshed for %dms\n", commandTimeoutMs_);
        return true;
    }

    int nextId = 10000;
    int armed = 0;
    for (const PreparedBinding& prepared : preparedBindings_) {
        int id = nextId++;
        if (!RegisterHotKey(nullptr, id, prepared.combo.mods | MOD_NOREPEAT, prepared.combo.vk)) {
            wprintf(L"command mode skip %s: temporary register failed\n", prepared.label.c_str());
            continue;
        }

        temporaryIds_.push_back(id);
        dispatch_[id] = prepared.action;
        hotkeyVk_[id] = prepared.combo.vk;
        hotkeyLabels_[id] = prepared.label;
        hotkeyPretty_[id] = prepared.combo.pretty;
        ++armed;
    }

    commandActive_ = armed > 0;
    if (commandActive_) {
        resetCommandTimer();
        wprintf(L"command mode active: %d bindings armed; expires %dms after the last command\n", armed, commandTimeoutMs_);
    } else {
        wprintf(L"command mode activation failed: no bindings could be armed\n");
    }
    return true;
}

void HotkeyRegistry::resetCommandTimer() {
    if (commandTimerId_ != 0) {
        KillTimer(nullptr, commandTimerId_);
        commandTimerId_ = 0;
    }
    commandTimerId_ = SetTimer(nullptr, 0, static_cast<UINT>(commandTimeoutMs_), nullptr);
}

void HotkeyRegistry::deactivateCommandMode() {
    if (commandTimerId_ != 0) {
        KillTimer(nullptr, commandTimerId_);
        commandTimerId_ = 0;
    }
    clearRegisteredIds(temporaryIds_);
    for (auto it = dispatch_.begin(); it != dispatch_.end();) {
        if (it->first >= 10000) {
            it = dispatch_.erase(it);
        } else {
            ++it;
        }
    }
    for (auto it = hotkeyVk_.begin(); it != hotkeyVk_.end();) {
        if (it->first >= 10000) {
            it = hotkeyVk_.erase(it);
        } else {
            ++it;
        }
    }
    for (auto it = hotkeyLabels_.begin(); it != hotkeyLabels_.end();) {
        if (it->first >= 10000) {
            it = hotkeyLabels_.erase(it);
        } else {
            ++it;
        }
    }
    for (auto it = hotkeyPretty_.begin(); it != hotkeyPretty_.end();) {
        if (it->first >= 10000) {
            it = hotkeyPretty_.erase(it);
        } else {
            ++it;
        }
    }
    commandActive_ = false;
}

bool HotkeyRegistry::installKeyboardHook() {
    if (keyboardHook_) return true;
    g_keyboardHookRegistry = this;
    keyboardHook_ = SetWindowsHookExW(WH_KEYBOARD_LL, keyboardHookProc, nullptr, 0);
    if (!keyboardHook_) {
        if (g_keyboardHookRegistry == this) g_keyboardHookRegistry = nullptr;
        return false;
    }
    return true;
}

void HotkeyRegistry::uninstallKeyboardHook(bool restorePreviousMute) {
    bool hadActivePushToTalk = false;
    for (const auto& binding : pushToTalkBindings_) {
        if (binding.active) {
            hadActivePushToTalk = true;
            break;
        }
    }

    if (!pushToTalkBindings_.empty() || pushToTalkMuted_) {
        if (hadActivePushToTalk) {
            setPushToTalkMute(true, L"push-to-talk cleared while active");
        } else if (restorePreviousMute && pushToTalkHadPreviousMute_) {
            setPushToTalkMute(pushToTalkPreviousMute_, L"push-to-talk restored previous mute state");
        }
    }

    if (keyboardHook_) {
        UnhookWindowsHookEx(keyboardHook_);
        keyboardHook_ = nullptr;
    }
    if (g_keyboardHookRegistry == this) g_keyboardHookRegistry = nullptr;
    pushToTalkPreviousMute_ = false;
    pushToTalkHadPreviousMute_ = false;
    pushToTalkMuted_ = false;
    pushToTalkIdleMuted_ = true;
    pushToTalkOverlayEnabled_ = false;
    destroyPushToTalkOverlay();
}

static bool anyVkDown(std::initializer_list<int> keys) {
    for (int key : keys) {
        if ((GetAsyncKeyState(key) & 0x8000) != 0) return true;
    }
    return false;
}

bool HotkeyRegistry::comboModifiersDown(UINT mods) const {
    if ((mods & MOD_CONTROL) && !anyVkDown({VK_CONTROL, VK_LCONTROL, VK_RCONTROL})) return false;
    if ((mods & MOD_ALT) && !anyVkDown({VK_MENU, VK_LMENU, VK_RMENU})) return false;
    if ((mods & MOD_SHIFT) && !anyVkDown({VK_SHIFT, VK_LSHIFT, VK_RSHIFT})) return false;
    if ((mods & MOD_WIN) && !anyVkDown({VK_LWIN, VK_RWIN})) return false;
    return true;
}

void HotkeyRegistry::setPushToTalkMute(bool muted, const std::wstring& reason) {
    std::wstring report;
    if (setDefaultMicrophoneMute(muted, &report)) {
        pushToTalkMuted_ = muted;
        updatePushToTalkOverlay(muted);
        wprintf(L"%s: %s\n", reason.c_str(), report.c_str());
    } else {
        wprintf(L"%s failed: %s\n", reason.c_str(), report.c_str());
    }
}

bool HotkeyRegistry::ensurePushToTalkOverlay() {
    if (pushToTalkOverlayWindow_) {
        updatePushToTalkOverlay(pushToTalkMuted_);
        return true;
    }

    HINSTANCE instance = GetModuleHandleW(nullptr);
    WNDCLASSW wc{};
    wc.lpfnWndProc = pushToTalkOverlayProc;
    wc.hInstance = instance;
    wc.lpszClassName = kPushToTalkOverlayClass;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);

    if (!RegisterClassW(&wc) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
        return false;
    }

    RECT work = foregroundOverlayBounds();

    const int width = 22;
    const int height = 22;
    const int x = work.right - width - 18;
    const int y = work.bottom - height - 18;

    pushToTalkOverlayWindow_ = CreateWindowExW(
        WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_NOACTIVATE,
        kPushToTalkOverlayClass,
        L"",
        WS_POPUP,
        x,
        y,
        width,
        height,
        nullptr,
        nullptr,
        instance,
        this);

    if (!pushToTalkOverlayWindow_) return false;

    SetLayeredWindowAttributes(pushToTalkOverlayWindow_, RGB(255, 0, 255), 255, LWA_COLORKEY | LWA_ALPHA);
    ShowWindow(pushToTalkOverlayWindow_, SW_SHOWNOACTIVATE);
    BringWindowToTop(pushToTalkOverlayWindow_);
    SetWindowPos(pushToTalkOverlayWindow_, HWND_TOPMOST, x, y, width, height,
        SWP_NOACTIVATE | SWP_SHOWWINDOW | SWP_NOOWNERZORDER | SWP_NOSENDCHANGING);
    UpdateWindow(pushToTalkOverlayWindow_);
    return true;
}

void HotkeyRegistry::destroyPushToTalkOverlay() {
    if (!pushToTalkOverlayWindow_) return;
    DestroyWindow(pushToTalkOverlayWindow_);
    pushToTalkOverlayWindow_ = nullptr;
}

void HotkeyRegistry::updatePushToTalkOverlay(bool muted) {
    (void)muted;
    if (!pushToTalkOverlayWindow_) return;
    RECT work = foregroundOverlayBounds();
    const int width = 22;
    const int height = 22;
    const int x = work.right - width - 18;
    const int y = work.bottom - height - 18;
    SetWindowPos(pushToTalkOverlayWindow_, HWND_TOPMOST, x, y, width, height,
        SWP_NOACTIVATE | SWP_SHOWWINDOW | SWP_NOOWNERZORDER | SWP_NOSENDCHANGING);
    InvalidateRect(pushToTalkOverlayWindow_, nullptr, TRUE);
    UpdateWindow(pushToTalkOverlayWindow_);
}

void HotkeyRegistry::paintPushToTalkOverlay() {
    if (!pushToTalkOverlayWindow_) return;

    PAINTSTRUCT ps;
    HDC hdc = BeginPaint(pushToTalkOverlayWindow_, &ps);
    RECT rect{};
    GetClientRect(pushToTalkOverlayWindow_, &rect);

    HBRUSH transparentBrush = CreateSolidBrush(RGB(255, 0, 255));
    FillRect(hdc, &rect, transparentBrush);
    DeleteObject(transparentBrush);

    const bool muted = pushToTalkMuted_;
    const COLORREF accent = muted ? RGB(222, 72, 84) : RGB(35, 210, 189);
    HBRUSH glowBrush = CreateSolidBrush(muted ? RGB(122, 38, 47) : RGB(20, 101, 94));
    HBRUSH oldGlowBrush = static_cast<HBRUSH>(SelectObject(hdc, glowBrush));
    HPEN glowPen = CreatePen(PS_SOLID, 1, muted ? RGB(122, 38, 47) : RGB(20, 101, 94));
    HPEN oldGlowPen = static_cast<HPEN>(SelectObject(hdc, glowPen));
    Ellipse(hdc, 1, 1, rect.right - 1, rect.bottom - 1);
    SelectObject(hdc, oldGlowPen);
    DeleteObject(glowPen);
    SelectObject(hdc, oldGlowBrush);
    DeleteObject(glowBrush);

    HBRUSH dotBrush = CreateSolidBrush(accent);
    HBRUSH oldBrush = static_cast<HBRUSH>(SelectObject(hdc, dotBrush));
    HPEN dotPen = CreatePen(PS_SOLID, 1, accent);
    HPEN oldPen = static_cast<HPEN>(SelectObject(hdc, dotPen));
    Ellipse(hdc, 5, 5, rect.right - 5, rect.bottom - 5);
    SelectObject(hdc, oldPen);
    DeleteObject(dotPen);
    SelectObject(hdc, oldBrush);
    DeleteObject(dotBrush);

    EndPaint(pushToTalkOverlayWindow_, &ps);
}

bool HotkeyRegistry::handleKeyboardEvent(WPARAM message, const KBDLLHOOKSTRUCT& event) {
    // Picker overlays are non-activating, so Escape is caught globally. Movement
    // and paste stay configurable through normal clipboard history bindings.
    if ((message == WM_KEYDOWN || message == WM_SYSKEYDOWN)
        && (event.flags & LLKHF_INJECTED) == 0) {
        if (clipboardHistoryPickerIsOpen() && event.vkCode == VK_ESCAPE) {
            cancelClipboardHistoryPicker(nullptr);
            return true;
        }

        if (mediaPickerIsOpen() && event.vkCode == VK_ESCAPE) {
            cancelMediaPicker(nullptr);
            return true;
        }
    }

    if (pushToTalkBindings_.empty()) return false;
    if ((event.flags & LLKHF_INJECTED) != 0) return false;

    const bool down = message == WM_KEYDOWN || message == WM_SYSKEYDOWN;
    const bool up = message == WM_KEYUP || message == WM_SYSKEYUP;
    if (!down && !up) return false;

    bool swallow = false;
    bool anyActive = false;
    bool activeMuted = pushToTalkIdleMuted_;
    bool changedActiveState = false;

    for (auto& binding : pushToTalkBindings_) {
        if (event.vkCode != binding.combo.vk) {
            if (binding.active) {
                anyActive = true;
                activeMuted = binding.muteWhileHeld;
            }
            continue;
        }

        if (down && comboModifiersDown(binding.combo.mods)) {
            swallow = true;
            if (!binding.active) {
                binding.active = true;
                changedActiveState = true;
                std::lock_guard<std::mutex> lock(eventMutex_);
                ++eventCounter_;
                lastEvent_ = L"last hotkey: " + binding.combo.pretty + L" -> " + binding.label
                    + L" pressed #" + std::to_wstring(eventCounter_);
            }
        } else if (up && binding.active) {
            swallow = true;
            binding.active = false;
            changedActiveState = true;
            std::lock_guard<std::mutex> lock(eventMutex_);
            ++eventCounter_;
            lastEvent_ = L"last hotkey: " + binding.combo.pretty + L" -> " + binding.label
                + L" released #" + std::to_wstring(eventCounter_);
        }

        if (binding.active) {
            anyActive = true;
            activeMuted = binding.muteWhileHeld;
        }
    }

    if (changedActiveState) {
        const bool targetMuted = anyActive ? activeMuted : pushToTalkIdleMuted_;
        if (targetMuted != pushToTalkMuted_) {
            setPushToTalkMute(targetMuted, anyActive ? L"held mic key pressed" : L"held mic key released");
        }
    }

    return swallow;
}

bool HotkeyRegistry::handleTimer(WPARAM timerId) {
    if (!commandMode_ || commandTimerId_ == 0 || timerId != commandTimerId_) return false;
    wprintf(L"command mode timed out\n");
    deactivateCommandMode();
    return true;
}

bool HotkeyRegistry::dispatch(WPARAM hotkeyId) {
    const int id = static_cast<int>(hotkeyId);
    auto vk = hotkeyVk_.find(id);
    if (vk != hotkeyVk_.end() && consumeSuppressedSyntheticHotkey(vk->second)) {
        return true;
    }

    if (commandMode_ && id == commandActivationId_) {
        {
            std::lock_guard<std::mutex> lock(eventMutex_);
            ++eventCounter_;
            lastEvent_ = L"last hotkey: " + hotkeyPretty_[id] + L" -> command mode launcher #" + std::to_wstring(eventCounter_);
        }
        return activateCommandMode();
    }

    auto it = dispatch_.find(id);
    if (it == dispatch_.end()) return false;

    Runnable* action = it->second;
    if (commandMode_ && commandActive_) {
        resetCommandTimer();
    }
    {
        std::lock_guard<std::mutex> lock(eventMutex_);
        ++eventCounter_;
        std::wstring pretty = hotkeyPretty_.count(id) ? hotkeyPretty_[id] : L"unknown";
        std::wstring label = hotkeyLabels_.count(id) ? hotkeyLabels_[id] : L"binding";
        lastEvent_ = L"last hotkey: " + pretty + L" -> " + label + L" #" + std::to_wstring(eventCounter_);
    }
    return runAction(action);
}

std::wstring HotkeyRegistry::lastEvent() const {
    std::lock_guard<std::mutex> lock(eventMutex_);
    return lastEvent_;
}
