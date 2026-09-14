#include "rescue.h"

#include "rescue_kill.h"
#include "rescue_log.h"
#include "rescue_meta.h"
#include "rescue_overlay.h"
#include "rescue_residency.h"
#include "rescue_sampler.h"
#include "rescue_trigger.h"

namespace rescue {

LogRing& logRing() {
    static LogRing ring;
    return ring;
}

std::wstring describeRescueLog(uint32_t count) {
    LogRing& ring = logRing();
    const uint32_t head = ring.head.load(std::memory_order_acquire);
    const uint32_t available = head < kLogSlots ? head : kLogSlots;
    if (count > available) count = available;

    std::wstring out;
    for (uint32_t i = count; i > 0; --i) {
        const uint32_t index = (head - i) % kLogSlots;
        out += L"  ";
        out += ring.slots[index];
        out += L"\n";
    }
    return out;
}

} // namespace rescue

static bool g_started = false;

bool startRescue(const RescueSettings& settings, std::wstring* report) {
    std::wstring out;
    if (!settings.enabled) {
        out = L"rescue menu disabled in config\n";
        if (report) *report = out;
        return false;
    }
    if (g_started) return true;

    std::wstring part;
    if (!rescue::samplerStart(settings, &part)) {
        if (report) *report = part;
        return false;
    }
    out += part;

    if (!rescue::overlayStart(settings, &part)) {
        rescue::samplerStop();
        if (report) *report = out + part;
        return false;
    }
    out += part;

    rescue::killStart(&part);
    out += part;

    rescue::metaStart(&part);
    out += part;

    rescue::triggerStart(settings.hotkey, rescue::overlayTriggerEvent(), &part);
    out += part;

    g_started = true;
    if (report) *report = out;
    return true;
}

void stopRescue() {
    if (!g_started) return;
    rescue::triggerStop();
    rescue::overlayStop();
    rescue::metaStop();
    rescue::killStop();
    rescue::samplerStop();
    g_started = false;
}

void applyRescueConfig(const RescueSettings& settings) {
    if (!g_started) return;
    rescue::samplerApply(settings);
    rescue::overlayApply(settings);
    rescue::triggerApply(settings.hotkey);
}

void suspendRescueTrigger() {
    if (g_started) rescue::triggerSuspend();
}

void resumeRescueTrigger() {
    if (g_started) rescue::triggerResume();
}

bool rescueObserveRawKey(UINT vk, bool pressed) {
    if (!g_started) return false;
    return rescue::observeRawKey(vk, pressed);
}

bool triggerRescue(std::wstring* report) {
    if (!g_started) {
        if (report) *report = L"rescue menu is not running";
        return false;
    }
    rescue::fireTrigger();
    if (report) *report = L"rescue menu requested";
    return true;
}

bool dismissRescue(std::wstring* report) {
    const bool ok = g_started && rescue::overlayDismiss();
    if (report) *report = ok ? L"rescue menu closing" : L"rescue menu is not open";
    return ok;
}

bool dumpRescueBitmap(std::wstring* report) {
    wchar_t path[MAX_PATH];
    const DWORD chars = GetTempPathW(MAX_PATH, path);
    if (chars == 0 || chars > MAX_PATH - 24) {
        if (report) *report = L"no temp path";
        return false;
    }
    wcscat_s(path, L"keyrail-rescue.bmp");
    const bool ok = g_started && rescue::overlayDumpBitmap(path);
    if (report) *report = ok ? std::wstring(L"wrote ") + path : L"could not render the rescue surface";
    return ok;
}

std::wstring describeRescueStatus() {
    if (!g_started) return L"rescue: not running\n";

    const rescue::SamplerStatus sampler = rescue::samplerStatus();
    const rescue::OverlayStatus overlay = rescue::overlayStatus();
    const rescue::KillStatus kill = rescue::killStatus();
    const HotkeyCombo combo = rescue::triggerCombo();

    std::wstring out = L"rescue:\n";
    out += L"  hotkey " + (combo.ok ? combo.pretty : L"invalid")
        + (rescue::triggerClaimed() ? L" (claimed + raw input)\n" : L" (raw input only)\n");
    out += L"  sampler: " + std::to_wstring(sampler.ticks) + L" ticks, last " + std::to_wstring(sampler.lastTickMicros / 1000)
        + L"." + std::to_wstring((sampler.lastTickMicros / 100) % 10) + L" ms, missed " + std::to_wstring(sampler.missedTicks)
        + (sampler.memoryLocked ? L", locked" : L", NOT locked") + (sampler.workingSetPinned ? L", pinned\n" : L", NOT pinned\n");
    out += std::wstring(L"  overlay: ") + (overlay.privateDesktop ? L"private desktop + fallback" : L"fallback only")
        + L", shown " + std::to_wstring(overlay.shows) + L"x";
    if (overlay.shows) {
        out += L", last on " + std::wstring(overlay.lastTier) + L", trigger->pixel "
            + std::to_wstring(overlay.lastTriggerToPixelMicros / 1000) + L"." + std::to_wstring((overlay.lastTriggerToPixelMicros / 100) % 10) + L" ms";
    }
    out += L"\n";
    out += std::wstring(L"  kill: ") + (kill.elevated ? L"elevated" : L"not elevated")
        + (kill.debugPrivilege ? L", SeDebugPrivilege on\n" : L", SeDebugPrivilege off\n");
    const rescue::ResidencyStatus residency = rescue::residencyStatus();
    if (residency.attempted) out += std::wstring(L"  ") + residency.detail + L"\n";
    const rescue::MetaStatus meta = rescue::metaStatus();
    out += L"  names: " + std::to_wstring(meta.resolved) + L" resolved, " + std::to_wstring(meta.misses)
        + L" without a description, " + std::to_wstring(meta.evicted) + L" evicted\n";
    if (sampler.lastError[0]) out += std::wstring(L"  last error: ") + sampler.lastError + L"\n";
    out += rescue::describeRescueLog(8);
    return out;
}
