#include "hotkey_registry.h"

#include "action_factory.h"
#include "hotkey.h"
#include "synthetic_hotkey_suppression.h"

#include <windows.h>
#include <cstdio>
#include <sstream>
#include <exception>
#include <thread>
#include <unordered_set>

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

    if (!buildPreparedBindings(config, actions_, preparedBindings_, status)) {
        status << L"no bindings ready\n";
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
