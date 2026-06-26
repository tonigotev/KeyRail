#include "hotkey_registry.h"

#include "action_factory.h"
#include "hotkey.h"

#include <windows.h>
#include <sstream>
#include <thread>
#include <unordered_set>

HotkeyRegistry::~HotkeyRegistry() {
    clear();
}

void HotkeyRegistry::clear() {
    for (int id : registeredIds_) {
        UnregisterHotKey(nullptr, id);
    }
    registeredIds_.clear();
    dispatch_.clear();
    actions_.clear();
}

std::wstring HotkeyRegistry::apply(const AppConfig& config) {
    clear();

    std::wstringstream status;
    status << L"config version " << config.version << L"\n";

    int nextId = 1;
    int armed = 0;
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

        int id = nextId++;
        if (!RegisterHotKey(nullptr, id, combo.mods | MOD_NOREPEAT, combo.vk)) {
            DWORD error = GetLastError();
            status << L"skip " << label << L": "
                   << (error == ERROR_HOTKEY_ALREADY_REGISTERED ? L"already in use" : L"register failed")
                   << L"\n";
            continue;
        }

        Runnable* action = built.action.get();
        actions_.push_back(std::move(built.action));
        dispatch_[id] = action;
        registeredIds_.push_back(id);
        ++armed;

        status << L"armed " << combo.pretty << L" -> " << label << L"\n";
    }

    if (armed == 0) status << L"no hotkeys armed\n";
    return status.str();
}

bool HotkeyRegistry::dispatch(WPARAM hotkeyId) const {
    auto it = dispatch_.find(static_cast<int>(hotkeyId));
    if (it == dispatch_.end()) return false;

    Runnable* action = it->second;
    std::thread([action] { action->run(); }).detach();
    return true;
}
