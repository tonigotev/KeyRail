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

    struct PushToTalkBinding {
        HotkeyCombo combo;
        std::wstring label;
        bool active = false;
        bool showOverlay = false;
        bool muteWhileHeld = false;
    };

    ~HotkeyRegistry();

    std::wstring apply(const AppConfig& config);
    bool dispatch(WPARAM hotkeyId);
    bool handleTimer(WPARAM timerId);
    bool handleKeyboardEvent(WPARAM message, const KBDLLHOOKSTRUCT& event);
    std::wstring lastEvent() const;
    void paintPushToTalkOverlay();
    void clear();

private:
    void clearRegisteredIds(std::vector<int>& ids);
    bool runAction(Runnable* action) const;
    bool activateCommandMode();
    void resetCommandTimer();
    void deactivateCommandMode();
    bool installKeyboardHook();
    void uninstallKeyboardHook(bool restorePreviousMute);
    bool comboModifiersDown(UINT mods) const;
    void setPushToTalkMute(bool muted, const std::wstring& reason);
    bool ensurePushToTalkOverlay();
    void destroyPushToTalkOverlay();
    void updatePushToTalkOverlay(bool muted);

    std::vector<int> registeredIds_;
    std::vector<int> temporaryIds_;
    std::vector<std::unique_ptr<Runnable>> actions_;
    std::unordered_map<int, Runnable*> dispatch_;
    std::unordered_map<int, UINT> hotkeyVk_;
    std::unordered_map<int, std::wstring> hotkeyLabels_;
    std::unordered_map<int, std::wstring> hotkeyPretty_;
    std::vector<PreparedBinding> preparedBindings_;
    std::vector<PushToTalkBinding> pushToTalkBindings_;
    HHOOK keyboardHook_ = nullptr;
    bool pushToTalkPreviousMute_ = false;
    bool pushToTalkHadPreviousMute_ = false;
    bool pushToTalkMuted_ = false;
    bool pushToTalkIdleMuted_ = true;
    bool pushToTalkOverlayEnabled_ = false;
    HWND pushToTalkOverlayWindow_ = nullptr;
    mutable std::mutex eventMutex_;
    std::wstring lastEvent_;
    unsigned long long eventCounter_ = 0;
    bool commandMode_ = false;
    bool commandActive_ = false;
    int commandActivationId_ = 0;
    UINT_PTR commandTimerId_ = 0;
    int commandTimeoutMs_ = 4000;
};
