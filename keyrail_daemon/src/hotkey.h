#pragma once

#include <windows.h>
#include <string>

struct HotkeyCombo {
    bool ok = false;
    UINT mods = 0;
    UINT vk = 0;
    std::wstring pretty;
    std::wstring error;

    unsigned long long key() const {
        return (static_cast<unsigned long long>(mods) << 32) | vk;
    }
};

HotkeyCombo parseHotkeyCombo(const std::wstring& text);
