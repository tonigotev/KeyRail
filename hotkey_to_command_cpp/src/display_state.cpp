#include "display_state.h"

#include <windows.h>
#include <shlobj.h>

#include <mutex>

namespace {

std::mutex g_mutex;
bool g_lastShowSuppressed = false;
std::wstring g_lastSuppressedWhat;
unsigned long long g_suppressedCount = 0;

} // namespace

bool exclusiveFullscreenActive() {
    QUERY_USER_NOTIFICATION_STATE state{};
    if (FAILED(SHQueryUserNotificationState(&state))) return false;
    return state == QUNS_RUNNING_D3D_FULL_SCREEN;
}

void noteOverlayShown(const std::wstring& what) {
    const bool suppressed = exclusiveFullscreenActive();

    std::lock_guard<std::mutex> lock(g_mutex);
    g_lastShowSuppressed = suppressed;
    if (!suppressed) return;

    ++g_suppressedCount;
    g_lastSuppressedWhat = what;
}

std::wstring describeOverlayVisibility() {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_suppressedCount == 0) return L"";

    std::wstring status = L"overlay visibility:\n";
    status += L"  hidden by exclusive fullscreen: " + std::to_wstring(g_suppressedCount) + L"\n";
    if (!g_lastSuppressedWhat.empty()) {
        status += L"  last suppressed: " + g_lastSuppressedWhat + L"\n";
    }
    if (g_lastShowSuppressed) {
        status += L"  a game is in exclusive fullscreen right now; switch it to"
                  L" borderless so overlays can draw over it\n";
    }
    return status;
}
