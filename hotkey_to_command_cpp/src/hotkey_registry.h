#pragma once

#include "config.h"
#include "hotkey.h"
#include "runners.h"

#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>
#include <windows.h>

class HotkeyRegistry {
public:
    struct PreparedBinding {
        HotkeyCombo combo;
        std::wstring label;
        Runnable* action = nullptr;
    };

    ~HotkeyRegistry();

    std::wstring apply(const AppConfig& config);
    bool dispatch(WPARAM hotkeyId);
    bool handleTimer(WPARAM timerId);
    std::wstring lastEvent() const;
    void clear();

private:
    void clearRegisteredIds(std::vector<int>& ids);
    bool runAction(Runnable* action) const;
    bool activateCommandMode();
    void resetCommandTimer();
    void deactivateCommandMode();

    std::vector<int> registeredIds_;
    std::vector<int> temporaryIds_;
    std::vector<std::unique_ptr<Runnable>> actions_;
    std::unordered_map<int, Runnable*> dispatch_;
    std::unordered_map<int, UINT> hotkeyVk_;
    std::unordered_map<int, std::wstring> hotkeyLabels_;
    std::unordered_map<int, std::wstring> hotkeyPretty_;
    std::vector<PreparedBinding> preparedBindings_;
    mutable std::mutex eventMutex_;
    std::wstring lastEvent_;
    unsigned long long eventCounter_ = 0;
    bool commandMode_ = false;
    bool commandActive_ = false;
    int commandActivationId_ = 0;
    UINT_PTR commandTimerId_ = 0;
    int commandTimeoutMs_ = 4000;
};
