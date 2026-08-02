#include "hotkey.h"

#include <cwctype>
#include <unordered_map>

HotkeyCombo parseHotkeyCombo(const std::wstring& text) {
    static const std::unordered_map<std::wstring, UINT> kMods = {
        {L"ctrl", MOD_CONTROL}, {L"control", MOD_CONTROL}, {L"alt", MOD_ALT},
        {L"shift", MOD_SHIFT}, {L"win", MOD_WIN}, {L"super", MOD_WIN}};
    static const std::unordered_map<std::wstring, UINT> kNamed = {
        {L"space", VK_SPACE}, {L"enter", VK_RETURN}, {L"return", VK_RETURN},
        {L"tab", VK_TAB}, {L"esc", VK_ESCAPE}, {L"escape", VK_ESCAPE},
        {L"backspace", VK_BACK}, {L"delete", VK_DELETE}, {L"insert", VK_INSERT},
        {L"home", VK_HOME}, {L"end", VK_END}, {L"pageup", VK_PRIOR},
        {L"pagedown", VK_NEXT}, {L"up", VK_UP}, {L"down", VK_DOWN},
        {L"left", VK_LEFT}, {L"right", VK_RIGHT},
        {L"media_next", VK_MEDIA_NEXT_TRACK}, {L"medianext", VK_MEDIA_NEXT_TRACK},
        {L"nexttrack", VK_MEDIA_NEXT_TRACK}, {L"next_track", VK_MEDIA_NEXT_TRACK},
        {L"media_previous", VK_MEDIA_PREV_TRACK}, {L"media_prev", VK_MEDIA_PREV_TRACK},
        {L"previous_track", VK_MEDIA_PREV_TRACK}, {L"prev_track", VK_MEDIA_PREV_TRACK},
        {L"media_play_pause", VK_MEDIA_PLAY_PAUSE}, {L"play_pause", VK_MEDIA_PLAY_PAUSE},
        {L"playpause", VK_MEDIA_PLAY_PAUSE},
        {L"media_stop", VK_MEDIA_STOP}};

    UINT mods = 0;
    std::wstring key;
    std::wstring lower;
    for (wchar_t c : text) {
        if (c != L' ') lower += static_cast<wchar_t>(towlower(c));
    }

    auto handle = [&](const std::wstring& token) -> bool {
        if (token.empty()) return true;
        auto mod = kMods.find(token);
        if (mod != kMods.end()) {
            mods |= mod->second;
            return true;
        }
        if (!key.empty()) return false;
        key = token;
        return true;
    };

    size_t start = 0;
    for (size_t i = 0; i <= lower.size(); ++i) {
        if (i == lower.size() || lower[i] == L'+') {
            if (!handle(lower.substr(start, i - start))) {
                return {false, 0, 0, L"", L"only one non-modifier key is allowed"};
            }
            start = i + 1;
        }
    }

    if (key.empty()) return {false, 0, 0, L"", L"missing non-modifier key"};

    UINT vk = 0;
    auto named = kNamed.find(key);
    if (named != kNamed.end()) {
        vk = named->second;
    } else if (key.size() >= 2 && key[0] == L'f') {
        int number = 0;
        for (size_t i = 1; i < key.size(); ++i) {
            if (!iswdigit(key[i])) return {false, 0, 0, L"", L"unknown key"};
            number = number * 10 + (key[i] - L'0');
        }
        if (number < 1 || number > 24) return {false, 0, 0, L"", L"function key must be f1-f24"};
        vk = VK_F1 + static_cast<UINT>(number - 1);
    } else if (key.size() == 1 && iswalnum(key[0])) {
        vk = static_cast<UINT>(towupper(key[0]));
    } else {
        return {false, 0, 0, L"", L"unknown key"};
    }

    std::wstring pretty;
    if (mods & MOD_CONTROL) pretty += L"Ctrl+";
    if (mods & MOD_ALT) pretty += L"Alt+";
    if (mods & MOD_SHIFT) pretty += L"Shift+";
    if (mods & MOD_WIN) pretty += L"Win+";
    pretty += key;

    return {true, mods, vk, pretty, L""};
}
