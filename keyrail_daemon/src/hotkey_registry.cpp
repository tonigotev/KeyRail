#include "hotkey_registry.h"

#include "action_factory.h"
#include "audio.h"
#include "clipboard_history.h"
#include "display_state.h"
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

static const wchar_t* kPushToTalkOverlayClass = L"KeyRailPushToTalkOverlay";
static constexpr DWORD kDuplicateBackendWindowMs = 120;

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

static std::wstring pointerText(HANDLE handle) {
    wchar_t buffer[32]{};
    swprintf_s(buffer, L"%p", handle);
    return buffer;
}

static std::wstring vkText(UINT vk) {
    wchar_t buffer[16]{};
    swprintf_s(buffer, L"0x%02X", vk);
    return buffer;
}

void HotkeyRegistry::clearRegisteredIds(std::vector<int>& ids) {
    for (int id : ids) {
        UnregisterHotKey(nullptr, id);
    }
    ids.clear();
}

void HotkeyRegistry::clear() {
    resetPushToTalkState(true);

    if (commandTimerId_ != 0) {
        KillTimer(nullptr, commandTimerId_);
        commandTimerId_ = 0;
    }

    clearRegisteredIds(temporaryIds_);
    clearRegisteredIds(registeredIds_);
    dispatch_.clear();
    hotkeyVk_.clear();
    hotkeyComboKeys_.clear();
    hotkeyLabels_.clear();
    hotkeyPretty_.clear();
    commandActivationId_ = 0;
    actions_.clear();
    preparedBindings_.clear();
    pushToTalkBindings_.clear();
    rawDownVks_.clear();
    rawPressedKeys_.clear();
    lastDispatchTick_.clear();
    rawEventCounter_ = 0;
    rawDownCounter_ = 0;
    rawUpCounter_ = 0;
    rawMatchCounter_ = 0;
    rawDispatchCounter_ = 0;
    rawDuplicateCounter_ = 0;
    rawRepeatCounter_ = 0;
    rawModifierMissCounter_ = 0;
    rawInjectedCounter_ = 0;
    lastRawEvent_.clear();
    lastRawResult_.clear();
    pushToTalkOverlayEnabled_ = false;
    commandMode_ = false;
    commandActive_ = false;
    commandActivationComboReady_ = false;
    commandActivationCombo_ = {};
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
        if (pushToTalkOverlayEnabled_ && !ensurePushToTalkOverlay()) {
            status << L"push-to-talk overlay disabled: window failed\n";
        }
        setPushToTalkMute(pushToTalkIdleMuted_, L"held mic mode idle");
        status << L"held mic bindings armed: " << pushToTalkBindings_.size()
               << L" (raw input observes the key, it is not swallowed)\n";
    }

    int nextId = 1;

    if (commandMode_) {
        HotkeyCombo activation = parseHotkeyCombo(config.settings.commandHotkey);
        if (!activation.ok) {
            status << L"command mode disabled: launcher hotkey " << activation.error << L"\n";
            return status.str();
        }

        commandActivationCombo_ = activation;
        commandActivationComboReady_ = true;
        commandActivationId_ = nextId++;

        // Only the launcher is claimed up front in command mode. The bindings
        // themselves are registered on demand once the launcher fires.
        if (RegisterHotKey(nullptr, commandActivationId_, activation.mods | MOD_NOREPEAT, activation.vk)) {
            registeredIds_.push_back(commandActivationId_);
            hotkeyVk_[commandActivationId_] = activation.vk;
            hotkeyComboKeys_[commandActivationId_] = activation.key();
            hotkeyLabels_[commandActivationId_] = L"command mode launcher";
            hotkeyPretty_[commandActivationId_] = activation.pretty;
            status << L"command mode launcher " << activation.pretty << L" (claimed)\n";
        } else {
            DWORD error = GetLastError();
            commandActivationId_ = 0;
            status << L"command mode launcher " << activation.pretty << L" (raw input only, RegisterHotKey "
                   << (error == ERROR_HOTKEY_ALREADY_REGISTERED ? L"already in use" : L"failed") << L")\n";
        }

        status << L"ready bindings: " << preparedBindings_.size() << L"\n";
        status << L"timeout: " << commandTimeoutMs_ << L"ms\n";
        status << L"raw input fallback armed: command launcher + " << preparedBindings_.size() << L" bindings\n";
        return status.str();
    }

    int claimed = 0;
    for (const PreparedBinding& prepared : preparedBindings_) {
        int id = nextId++;

        // MOD_NOREPEAT so holding the combo fires once rather than repeating.
        if (RegisterHotKey(nullptr, id, prepared.combo.mods | MOD_NOREPEAT, prepared.combo.vk)) {
            dispatch_[id] = prepared.action;
            hotkeyVk_[id] = prepared.combo.vk;
            hotkeyComboKeys_[id] = prepared.combo.key();
            hotkeyLabels_[id] = prepared.label;
            hotkeyPretty_[id] = prepared.combo.pretty;
            registeredIds_.push_back(id);
            ++claimed;
            status << L"armed " << prepared.combo.pretty << L" -> " << prepared.label << L" (claimed)\n";
        } else {
            // Not fatal. Raw Input still sees the key, the focused app just
            // receives it too.
            DWORD error = GetLastError();
            status << L"armed " << prepared.combo.pretty << L" -> " << prepared.label
                   << L" (raw input only, RegisterHotKey "
                   << (error == ERROR_HOTKEY_ALREADY_REGISTERED ? L"already in use" : L"failed") << L")\n";
        }
    }

    if (preparedBindings_.empty()) status << L"no hotkeys armed\n";
    status << L"claimed by RegisterHotKey: " << claimed << L" of " << preparedBindings_.size() << L"\n";
    status << L"raw input fallback armed: " << preparedBindings_.size() << L" bindings\n";
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

bool HotkeyRegistry::shouldDispatchCombo(const HotkeyCombo& combo) {
    const unsigned long long key = combo.key();
    const DWORD now = GetTickCount();
    auto previous = lastDispatchTick_.find(key);
    if (previous != lastDispatchTick_.end()
        && static_cast<DWORD>(now - previous->second) < kDuplicateBackendWindowMs) {
        noteDuplicateSuppressed(combo);
        return false;
    }

    lastDispatchTick_[key] = now;
    return true;
}

void HotkeyRegistry::noteRawEvent(UINT vk, bool pressed, HANDLE device) {
    std::lock_guard<std::mutex> lock(eventMutex_);
    ++rawEventCounter_;
    if (pressed) ++rawDownCounter_;
    else ++rawUpCounter_;
    lastRawEvent_ = L"raw input: vk=" + vkText(vk)
        + (pressed ? L" down" : L" up")
        + L" device=" + pointerText(device);
}

void HotkeyRegistry::noteRawResult(const std::wstring& result) {
    std::lock_guard<std::mutex> lock(eventMutex_);
    lastRawResult_ = result;
}

void HotkeyRegistry::noteDuplicateSuppressed(const HotkeyCombo& combo) {
    std::lock_guard<std::mutex> lock(eventMutex_);
    ++rawDuplicateCounter_;
    lastRawResult_ = L"duplicate suppressed for " + combo.pretty
        + L" within " + std::to_wstring(kDuplicateBackendWindowMs) + L"ms";
}

bool HotkeyRegistry::rawAnyVkDown(std::initializer_list<UINT> keys) const {
    for (UINT key : keys) {
        if (rawDownVks_.count(key)) return true;
    }
    return false;
}

bool HotkeyRegistry::rawComboModifiersDown(UINT mods) const {
    if ((mods & MOD_CONTROL) && !rawAnyVkDown({VK_CONTROL, VK_LCONTROL, VK_RCONTROL})) return false;
    if ((mods & MOD_ALT) && !rawAnyVkDown({VK_MENU, VK_LMENU, VK_RMENU})) return false;
    if ((mods & MOD_SHIFT) && !rawAnyVkDown({VK_SHIFT, VK_LSHIFT, VK_RSHIFT})) return false;
    if ((mods & MOD_WIN) && !rawAnyVkDown({VK_LWIN, VK_RWIN})) return false;
    return true;
}

bool HotkeyRegistry::dispatchPrepared(const PreparedBinding& prepared, HANDLE device) {
    if (!shouldDispatchCombo(prepared.combo)) return true;

    if (commandMode_ && commandActive_) {
        resetCommandTimer();
    }

    {
        std::lock_guard<std::mutex> lock(eventMutex_);
        ++eventCounter_;
        ++rawDispatchCounter_;
        lastRawResult_ = L"dispatched " + prepared.combo.pretty + L" -> " + prepared.label;
        lastEvent_ = L"last hotkey: " + prepared.combo.pretty + L" -> " + prepared.label
            + L" #" + std::to_wstring(eventCounter_) + L" via raw input";
        if (device) {
            lastEvent_ += L" device=0x";
            lastEvent_ += pointerText(device);
        }
    }

    return runAction(prepared.action);
}

// Escape closes the media and clipboard pickers without needing a binding. Raw
// Input cannot swallow the key, so Escape also reaches the focused window.
bool HotkeyRegistry::handlePickerDismiss(UINT vk) {
    if (vk != VK_ESCAPE) return false;

    if (clipboardHistoryPickerIsOpen()) {
        cancelClipboardHistoryPicker(nullptr);
        noteRawResult(L"escape closed the clipboard history picker");
        return true;
    }

    if (mediaPickerIsOpen()) {
        cancelMediaPicker(nullptr);
        noteRawResult(L"escape closed the media picker");
        return true;
    }

    return false;
}

// Held-mic bindings mute or unmute for as long as the key is down. Raw Input
// repeats key-down while a key is held, so the active flag guards the edges.
// Synthesized keys arrive with a null device handle and are ignored here, the
// same way the mic used to ignore LLKHF_INJECTED events.
void HotkeyRegistry::handlePushToTalkRawKey(UINT vk, bool pressed, HANDLE device) {
    if (pushToTalkBindings_.empty() || !device) return;

    bool anyActive = false;
    bool activeMuted = pushToTalkIdleMuted_;
    bool changedActiveState = false;

    for (auto& binding : pushToTalkBindings_) {
        if (binding.combo.vk == vk) {
            if (pressed && !binding.active && rawComboModifiersDown(binding.combo.mods)) {
                binding.active = true;
                changedActiveState = true;
                std::lock_guard<std::mutex> lock(eventMutex_);
                ++eventCounter_;
                lastEvent_ = L"last hotkey: " + binding.combo.pretty + L" -> " + binding.label
                    + L" pressed #" + std::to_wstring(eventCounter_);
            } else if (!pressed && binding.active) {
                binding.active = false;
                changedActiveState = true;
                std::lock_guard<std::mutex> lock(eventMutex_);
                ++eventCounter_;
                lastEvent_ = L"last hotkey: " + binding.combo.pretty + L" -> " + binding.label
                    + L" released #" + std::to_wstring(eventCounter_);
            }
        }

        if (binding.active) {
            anyActive = true;
            activeMuted = binding.muteWhileHeld;
        }
    }

    if (!changedActiveState) return;

    const bool targetMuted = anyActive ? activeMuted : pushToTalkIdleMuted_;
    if (targetMuted != pushToTalkMuted_) {
        setPushToTalkMute(targetMuted, anyActive ? L"held mic key pressed" : L"held mic key released");
    }
}

bool HotkeyRegistry::dispatchRawKey(UINT vk, bool pressed, HANDLE device) {
    noteRawEvent(vk, pressed, device);
    if (pressed) rawDownVks_.insert(vk);
    else rawDownVks_.erase(vk);

    handlePushToTalkRawKey(vk, pressed, device);

    auto clearPressed = [&](const HotkeyCombo& combo) {
        if (combo.vk == vk) rawPressedKeys_.erase(combo.key());
    };

    if (!pressed) {
        if (commandActivationComboReady_) clearPressed(commandActivationCombo_);
        for (const auto& prepared : preparedBindings_) clearPressed(prepared.combo);
        noteRawResult(L"key released");
        return false;
    }

    // Raw Input also reports keys the daemon itself synthesized with SendInput
    // (media keys, the clipboard paste). Drop those so an action cannot
    // retrigger its own binding.
    if (consumeSuppressedSyntheticHotkey(vk)) {
        std::lock_guard<std::mutex> lock(eventMutex_);
        ++rawInjectedCounter_;
        lastRawResult_ = L"ignored synthetic key vk=" + vkText(vk);
        return true;
    }

    // Ctrl+Alt+Q stays available as the debug quit for the console daemon.
    if (vk == 'Q' && rawComboModifiersDown(MOD_CONTROL | MOD_ALT)) {
        quitRequested_ = true;
        noteRawResult(L"quit requested");
        return true;
    }

    if (handlePickerDismiss(vk)) return true;

    if (commandMode_ && commandActivationComboReady_
        && commandActivationCombo_.vk == vk) {
        if (!rawComboModifiersDown(commandActivationCombo_.mods)) {
            {
                std::lock_guard<std::mutex> lock(eventMutex_);
                ++rawModifierMissCounter_;
            }
            noteRawResult(L"command launcher key seen but modifiers missing: " + commandActivationCombo_.pretty);
            return false;
        }

        {
            std::lock_guard<std::mutex> lock(eventMutex_);
            ++rawMatchCounter_;
        }
        const unsigned long long key = commandActivationCombo_.key();
        if (rawPressedKeys_.insert(key).second) {
            if (!shouldDispatchCombo(commandActivationCombo_)) return true;
            {
                std::lock_guard<std::mutex> lock(eventMutex_);
                ++eventCounter_;
                ++rawDispatchCounter_;
                lastRawResult_ = L"dispatched command mode launcher " + commandActivationCombo_.pretty;
                lastEvent_ = L"last hotkey: " + commandActivationCombo_.pretty
                    + L" -> command mode launcher #" + std::to_wstring(eventCounter_)
                    + L" via raw input";
            }
            return activateCommandMode();
        }
        {
            std::lock_guard<std::mutex> lock(eventMutex_);
            ++rawRepeatCounter_;
            lastRawResult_ = L"held repeat ignored for command launcher " + commandActivationCombo_.pretty;
        }
        return true;
    }

    if (commandMode_ && !commandActive_) {
        noteRawResult(L"ignored because command mode is not active");
        return false;
    }

    bool sawSameKey = false;
    for (const auto& prepared : preparedBindings_) {
        if (prepared.combo.vk != vk) continue;
        sawSameKey = true;
        if (!rawComboModifiersDown(prepared.combo.mods)) {
            std::lock_guard<std::mutex> lock(eventMutex_);
            ++rawModifierMissCounter_;
            lastRawResult_ = L"key matched " + prepared.combo.pretty + L" but modifiers missing";
            continue;
        }

        {
            std::lock_guard<std::mutex> lock(eventMutex_);
            ++rawMatchCounter_;
            lastRawResult_ = L"matched " + prepared.combo.pretty + L" -> " + prepared.label;
        }
        const unsigned long long key = prepared.combo.key();
        if (!rawPressedKeys_.insert(key).second) {
            std::lock_guard<std::mutex> lock(eventMutex_);
            ++rawRepeatCounter_;
            lastRawResult_ = L"held repeat ignored for " + prepared.combo.pretty;
            return true;
        }
        return dispatchPrepared(prepared, device);
    }

    if (!sawSameKey) noteRawResult(L"no binding uses vk=" + vkText(vk));
    return false;
}

bool HotkeyRegistry::activateCommandMode() {
    if (commandActive_) {
        resetCommandTimer();
        wprintf(L"command mode refreshed for %dms\n", commandTimeoutMs_);
        return true;
    }

    // Claim the bindings for as long as command mode is awake, so they behave
    // like real hotkeys during the window instead of leaking into the focused
    // app. Raw Input still covers anything RegisterHotKey cannot claim.
    int nextId = 10000;
    int claimed = 0;
    for (const PreparedBinding& prepared : preparedBindings_) {
        int id = nextId++;
        if (!RegisterHotKey(nullptr, id, prepared.combo.mods | MOD_NOREPEAT, prepared.combo.vk)) {
            continue;
        }
        temporaryIds_.push_back(id);
        dispatch_[id] = prepared.action;
        hotkeyVk_[id] = prepared.combo.vk;
        hotkeyComboKeys_[id] = prepared.combo.key();
        hotkeyLabels_[id] = prepared.label;
        hotkeyPretty_[id] = prepared.combo.pretty;
        ++claimed;
    }

    commandActive_ = !preparedBindings_.empty();
    if (commandActive_) {
        resetCommandTimer();
        wprintf(L"command mode active: %d of %zu bindings claimed, rest via raw input; expires %dms after the last command\n",
                claimed,
                preparedBindings_.size(),
                commandTimeoutMs_);
    } else {
        wprintf(L"command mode activation failed: no bindings ready\n");
    }
    return commandActive_;
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
    // Release the on-demand registrations and their bookkeeping. Ids at or above
    // 10000 are the temporary command-mode ones; lower ids are the permanent
    // launcher registration and must survive.
    clearRegisteredIds(temporaryIds_);
    for (auto* map : {&hotkeyVk_}) {
        for (auto it = map->begin(); it != map->end();) {
            it = (it->first >= 10000) ? map->erase(it) : std::next(it);
        }
    }
    for (auto it = dispatch_.begin(); it != dispatch_.end();) {
        it = (it->first >= 10000) ? dispatch_.erase(it) : std::next(it);
    }
    for (auto it = hotkeyComboKeys_.begin(); it != hotkeyComboKeys_.end();) {
        it = (it->first >= 10000) ? hotkeyComboKeys_.erase(it) : std::next(it);
    }
    for (auto it = hotkeyLabels_.begin(); it != hotkeyLabels_.end();) {
        it = (it->first >= 10000) ? hotkeyLabels_.erase(it) : std::next(it);
    }
    for (auto it = hotkeyPretty_.begin(); it != hotkeyPretty_.end();) {
        it = (it->first >= 10000) ? hotkeyPretty_.erase(it) : std::next(it);
    }

    commandActive_ = false;
}

void HotkeyRegistry::resetPushToTalkState(bool restorePreviousMute) {
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

    pushToTalkPreviousMute_ = false;
    pushToTalkHadPreviousMute_ = false;
    pushToTalkMuted_ = false;
    pushToTalkIdleMuted_ = true;
    pushToTalkOverlayEnabled_ = false;
    destroyPushToTalkOverlay();
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
    // No BringWindowToTop: activating this dot would pull a fullscreen game out
    // of exclusive mode. SWP_NOACTIVATE keeps it a pure z-order change.
    SetWindowPos(pushToTalkOverlayWindow_, HWND_TOPMOST, x, y, width, height,
        SWP_NOACTIVATE | SWP_SHOWWINDOW | SWP_NOOWNERZORDER | SWP_NOSENDCHANGING);
    UpdateWindow(pushToTalkOverlayWindow_);
    noteOverlayShown(L"held mic indicator");
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

bool HotkeyRegistry::handleTimer(WPARAM timerId) {
    if (!commandMode_ || commandTimerId_ == 0 || timerId != commandTimerId_) return false;
    wprintf(L"command mode timed out\n");
    deactivateCommandMode();
    return true;
}

// WM_HOTKEY from the primary backend. Raw Input will usually have seen the same
// key press, so every path here runs through shouldDispatchCombo, which drops
// whichever backend arrives second inside the debounce window.
bool HotkeyRegistry::dispatch(WPARAM hotkeyId) {
    const int id = static_cast<int>(hotkeyId);

    auto vk = hotkeyVk_.find(id);
    if (vk != hotkeyVk_.end() && consumeSuppressedSyntheticHotkey(vk->second)) {
        return true;
    }

    if (commandMode_ && id == commandActivationId_) {
        if (hotkeyComboKeys_.count(id)) {
            HotkeyCombo combo = commandActivationCombo_;
            combo.vk = hotkeyVk_[id];
            if (!shouldDispatchCombo(combo)) return true;
        }
        {
            std::lock_guard<std::mutex> lock(eventMutex_);
            ++eventCounter_;
            lastEvent_ = L"last hotkey: " + hotkeyPretty_[id] + L" -> command mode launcher #"
                + std::to_wstring(eventCounter_) + L" via RegisterHotKey";
        }
        return activateCommandMode();
    }

    auto it = dispatch_.find(id);
    if (it == dispatch_.end()) return false;

    Runnable* action = it->second;
    HotkeyCombo combo;
    combo.ok = true;
    combo.vk = hotkeyVk_.count(id) ? hotkeyVk_[id] : 0;
    combo.pretty = hotkeyPretty_.count(id) ? hotkeyPretty_[id] : L"unknown";
    if (hotkeyComboKeys_.count(id)) {
        combo.mods = static_cast<UINT>(hotkeyComboKeys_[id] >> 32);
        if (!shouldDispatchCombo(combo)) return true;
    }

    if (commandMode_ && commandActive_) resetCommandTimer();

    {
        std::lock_guard<std::mutex> lock(eventMutex_);
        ++eventCounter_;
        std::wstring pretty = hotkeyPretty_.count(id) ? hotkeyPretty_[id] : L"unknown";
        std::wstring label = hotkeyLabels_.count(id) ? hotkeyLabels_[id] : L"binding";
        lastEvent_ = L"last hotkey: " + pretty + L" -> " + label + L" #"
            + std::to_wstring(eventCounter_) + L" via RegisterHotKey";
    }
    return runAction(action);
}

bool HotkeyRegistry::consumeQuitRequest() {
    if (!quitRequested_) return false;
    quitRequested_ = false;
    return true;
}

std::wstring HotkeyRegistry::lastEvent() const {
    std::lock_guard<std::mutex> lock(eventMutex_);
    return lastEvent_;
}

std::wstring HotkeyRegistry::rawInputDebugStatus() const {
    std::lock_guard<std::mutex> lock(eventMutex_);
    std::wstring status;
    status += L"raw input debug:\n";
    status += L"  events: " + std::to_wstring(rawEventCounter_)
        + L" (down " + std::to_wstring(rawDownCounter_)
        + L", up " + std::to_wstring(rawUpCounter_) + L")\n";
    status += L"  matches: " + std::to_wstring(rawMatchCounter_)
        + L", dispatches: " + std::to_wstring(rawDispatchCounter_)
        + L", duplicate suppressions: " + std::to_wstring(rawDuplicateCounter_) + L"\n";
    status += L"  held repeats ignored: " + std::to_wstring(rawRepeatCounter_)
        + L", modifier misses: " + std::to_wstring(rawModifierMissCounter_)
        + L", synthetic ignored: " + std::to_wstring(rawInjectedCounter_)
        + L", pressed combos: " + std::to_wstring(rawPressedKeys_.size()) + L"\n";
    if (!lastRawEvent_.empty()) status += L"  last event: " + lastRawEvent_ + L"\n";
    if (!lastRawResult_.empty()) status += L"  last result: " + lastRawResult_ + L"\n";
    return status;
}
