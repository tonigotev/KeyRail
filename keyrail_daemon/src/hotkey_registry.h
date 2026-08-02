#pragma once

#include "config.h"
#include "hotkey.h"
#include "runners.h"

#include <initializer_list>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include <windows.h>

// Two detection backends run at once, because neither is sufficient alone.
//
//   RegisterHotKey (primary) actually claims the combo: Windows delivers it to
//   us and the focused app never sees it. That is what you want for a hotkey,
//   so nothing leaks a stray keypress into whatever has focus.
//
//   Raw Input (fallback) only observes. It cannot consume, but it keeps
//   working when a foreground app has taken the key first, which is the case
//   RegisterHotKey cannot cover.
//
// Both are armed for every binding. shouldDispatchCombo debounces per combo so
// a key seen by both backends fires the action exactly once.
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
    // WM_HOTKEY from the primary backend.
    bool dispatch(WPARAM hotkeyId);
    // WM_INPUT from the fallback backend.
    bool dispatchRawKey(UINT vk, bool pressed, HANDLE device);
    bool handleTimer(WPARAM timerId);
    bool consumeQuitRequest();
    std::wstring lastEvent() const;
    std::wstring rawInputDebugStatus() const;
    void paintPushToTalkOverlay();
    void clear();

private:
    void clearRegisteredIds(std::vector<int>& ids);
    bool runAction(Runnable* action) const;
    bool shouldDispatchCombo(const HotkeyCombo& combo);
    bool dispatchPrepared(const PreparedBinding& prepared, HANDLE device);
    void noteRawEvent(UINT vk, bool pressed, HANDLE device);
    void noteRawResult(const std::wstring& result);
    void noteDuplicateSuppressed(const HotkeyCombo& combo);
    bool rawAnyVkDown(std::initializer_list<UINT> keys) const;
    bool rawComboModifiersDown(UINT mods) const;
    bool handlePickerDismiss(UINT vk);
    void handlePushToTalkRawKey(UINT vk, bool pressed, HANDLE device);
    bool activateCommandMode();
    void resetCommandTimer();
    void deactivateCommandMode();
    void resetPushToTalkState(bool restorePreviousMute);
    void setPushToTalkMute(bool muted, const std::wstring& reason);
    bool ensurePushToTalkOverlay();
    void destroyPushToTalkOverlay();
    void updatePushToTalkOverlay(bool muted);

    // Primary backend bookkeeping, keyed by the id given to RegisterHotKey.
    std::vector<int> registeredIds_;
    std::vector<int> temporaryIds_;
    std::unordered_map<int, Runnable*> dispatch_;
    std::unordered_map<int, UINT> hotkeyVk_;
    std::unordered_map<int, unsigned long long> hotkeyComboKeys_;
    std::unordered_map<int, std::wstring> hotkeyLabels_;
    std::unordered_map<int, std::wstring> hotkeyPretty_;

    std::vector<std::unique_ptr<Runnable>> actions_;
    std::vector<PreparedBinding> preparedBindings_;
    std::vector<PushToTalkBinding> pushToTalkBindings_;
    bool pushToTalkPreviousMute_ = false;
    bool pushToTalkHadPreviousMute_ = false;
    bool pushToTalkMuted_ = false;
    bool pushToTalkIdleMuted_ = true;
    bool pushToTalkOverlayEnabled_ = false;
    HWND pushToTalkOverlayWindow_ = nullptr;
    mutable std::mutex eventMutex_;
    std::wstring lastEvent_;
    unsigned long long eventCounter_ = 0;
    unsigned long long rawEventCounter_ = 0;
    unsigned long long rawDownCounter_ = 0;
    unsigned long long rawUpCounter_ = 0;
    unsigned long long rawMatchCounter_ = 0;
    unsigned long long rawDispatchCounter_ = 0;
    unsigned long long rawDuplicateCounter_ = 0;
    unsigned long long rawRepeatCounter_ = 0;
    unsigned long long rawModifierMissCounter_ = 0;
    unsigned long long rawInjectedCounter_ = 0;
    std::wstring lastRawEvent_;
    std::wstring lastRawResult_;
    std::unordered_set<UINT> rawDownVks_;
    std::unordered_set<unsigned long long> rawPressedKeys_;
    std::unordered_map<unsigned long long, DWORD> lastDispatchTick_;
    bool quitRequested_ = false;
    bool commandMode_ = false;
    bool commandActive_ = false;
    bool commandActivationComboReady_ = false;
    HotkeyCombo commandActivationCombo_;
    int commandActivationId_ = 0;
    UINT_PTR commandTimerId_ = 0;
    int commandTimeoutMs_ = 4000;
};
