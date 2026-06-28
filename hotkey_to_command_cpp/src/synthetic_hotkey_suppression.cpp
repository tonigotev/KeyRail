#include "synthetic_hotkey_suppression.h"

#include <mutex>
#include <vector>

namespace {

struct SuppressedHotkey {
    UINT vk = 0;
    ULONGLONG expiresAt = 0;
};

std::mutex g_suppressedMutex;
std::vector<SuppressedHotkey> g_suppressedHotkeys;

} // namespace

void suppressNextSyntheticHotkey(UINT vk, DWORD milliseconds) {
    std::lock_guard<std::mutex> lock(g_suppressedMutex);
    g_suppressedHotkeys.push_back({vk, GetTickCount64() + milliseconds});
}

bool consumeSuppressedSyntheticHotkey(UINT vk) {
    const ULONGLONG now = GetTickCount64();
    std::lock_guard<std::mutex> lock(g_suppressedMutex);

    for (auto it = g_suppressedHotkeys.begin(); it != g_suppressedHotkeys.end();) {
        if (it->expiresAt < now) {
            it = g_suppressedHotkeys.erase(it);
            continue;
        }

        if (it->vk == vk) {
            g_suppressedHotkeys.erase(it);
            return true;
        }

        ++it;
    }

    return false;
}
