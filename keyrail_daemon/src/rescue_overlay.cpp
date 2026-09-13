#include "rescue_overlay.h"

#include "rescue_kill.h"
#include "rescue_log.h"
#include "rescue_meta.h"
#include "rescue_sampler.h"
#include "rescue_trigger.h"

#include <uxtheme.h>

#include <algorithm>
#include <atomic>
#include <cstring>
#include <cwctype>

namespace rescue {
namespace {

constexpr wchar_t kDesktopName[] = L"KeyRailRescue";
constexpr wchar_t kWindowClass[] = L"KeyRailRescueOverlay";
constexpr UINT WM_RESCUE_SHOW = WM_APP + 70;
constexpr UINT WM_RESCUE_REFRESH = WM_APP + 71;
constexpr UINT WM_RESCUE_FOCUS = WM_APP + 72;

constexpr int kWidth = 1120;
constexpr int kHeight = 660;
constexpr int kMargin = 14;
constexpr int kRowHeight = 20;
constexpr int kFontHeight = 15;
constexpr uint32_t kFilterChars = 24;
constexpr uint32_t kResultChars = 200;
constexpr DWORD kVisibleRefreshMs = 500;
constexpr uint32_t kIdleDismissSeconds = 180;

constexpr COLORREF kBackground = RGB(0, 0, 0);
constexpr COLORREF kText = RGB(222, 222, 222);
constexpr COLORREF kDim = RGB(140, 140, 140);
constexpr COLORREF kHeader = RGB(120, 170, 255);
constexpr COLORREF kTitle = RGB(255, 200, 80);
constexpr COLORREF kHung = RGB(255, 96, 96);
constexpr COLORREF kPaging = RGB(255, 200, 80);
constexpr COLORREF kDenied = RGB(110, 110, 110);
constexpr COLORREF kSelectedBg = RGB(48, 84, 140);
constexpr COLORREF kSelectedText = RGB(255, 255, 255);
constexpr COLORREF kConfirm = RGB(255, 120, 120);

enum class Tier { None, Private, Fallback };
enum class SortMode { Score, Name, Pid };

// A row is a process, not a rank position. The order is fixed when the list
// is (re)built; between rebuilds the numbers update in place, processes that
// exit drop out, and new ones append at the bottom. Re-ranking under the
// cursor every second made selecting anything a chase.
struct RowKey {
    uint32_t pid;
    int64_t createTime;
};

struct Surface {
    HDESK desktop = nullptr;       // null for the fallback surface
    HANDLE thread = nullptr;
    DWORD threadId = 0;
    HANDLE ready = nullptr;
    HWND hwnd = nullptr;
    HDC memDc = nullptr;
    HBITMAP dib = nullptr;
    HBITMAP oldBitmap = nullptr;
    void* bits = nullptr;
    HFONT font = nullptr;
    HFONT oldFont = nullptr;
    int charWidth = 8;
    bool controller = false;       // waits on the trigger event
    bool ok = false;
};

// One set of UI state; only the surface that is on screen touches it.
struct UiState {
    Tier tier = Tier::None;
    Surface* surface = nullptr;
    HDESK returnDesktop = nullptr;
    uint32_t selectedPid = 0;
    int64_t selectedCreateTime = 0;
    uint32_t scroll = 0;
    bool showAll = false;
    bool filterMode = false;
    int confirm = 0;               // 0 none, 1 kill
    uint32_t confirmPid = 0;
    int64_t confirmCreateTime = 0;
    wchar_t confirmImage[kImageChars] = {};
    wchar_t filter[kFilterChars] = {};
    uint32_t filterLen = 0;
    uint32_t lastSequence = 0;
    int64_t triggerQpc = 0;
    int64_t lastKeyQpc = 0;
    // Stage stamps for the show path, so a slow appearance says which step
    // waited on the disk instead of leaving it to guesswork.
    int64_t wakeQpc = 0;
    int64_t switchQpc = 0;
    SortMode sort = SortMode::Score;
    bool rebuild = true;               // next render rebuilds the order
    RowKey rows[kMaxRecords] = {};
    uint32_t rowCount = 0;
};

Surface g_private;
Surface g_fallback;
UiState g_ui;
HDESK g_rescueDesktop = nullptr;
HDESK g_homeDesktop = nullptr;
HANDLE g_trigger = nullptr;
HANDLE g_stop = nullptr;
HANDLE g_showThread = nullptr;     // waits on g_trigger and nothing else
LARGE_INTEGER g_qpcFrequency{};

std::atomic<bool> g_visible{false};
std::atomic<bool> g_autoPause{true};
// Set when a trigger is being handled and cleared once pixels are up. A
// second press in that window means "it is not coming; just kill it".
std::atomic<int64_t> g_pendingSinceQpc{0};
constexpr uint32_t kEscalateAfterMicros = 1500 * 1000;
std::atomic<int> g_rowLimit{15};
std::atomic<int> g_titleDeadlineMs{50};
SRWLOCK g_resultLock = SRWLOCK_INIT;
wchar_t g_result[kResultChars] = {};
SRWLOCK g_statusLock = SRWLOCK_INIT;
OverlayStatus g_status;

int64_t qpcNow() {
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    return now.QuadPart;
}

uint32_t microsSince(int64_t from) {
    return static_cast<uint32_t>((qpcNow() - from) * 1000000 / g_qpcFrequency.QuadPart);
}

// ---- drawing --------------------------------------------------------------------

// Measured under a real thrash, GDI text drawing waited 740 ms on the font
// cache, which lives in pageable kernel memory. So every glyph the menu can
// show is rendered once at startup into this locked table, and drawing a row
// is a plain memory blend into the locked DIB. No GDI on the panic path
// except the final BitBlt.
constexpr int kFirstGlyph = 32;
constexpr int kGlyphCount = 95;   // printable ASCII; anything else draws as '?'

struct GlyphAtlas {
    int cellWidth = 8;
    uint8_t* alpha = nullptr;      // [glyph][row][column], locked
    bool ok = false;
};

GlyphAtlas g_atlas;

uint32_t dibColor(COLORREF color) {
    return (static_cast<uint32_t>(GetRValue(color)) << 16)
        | (static_cast<uint32_t>(GetGValue(color)) << 8)
        | static_cast<uint32_t>(GetBValue(color));
}

bool buildGlyphAtlas(HFONT font, int cellWidth) {
    if (g_atlas.ok) return true;
    const SIZE_T bytes = static_cast<SIZE_T>(kGlyphCount) * kRowHeight * cellWidth;
    g_atlas.alpha = static_cast<uint8_t*>(VirtualAlloc(nullptr, bytes, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
    if (!g_atlas.alpha) return false;
    VirtualLock(g_atlas.alpha, bytes);
    g_atlas.cellWidth = cellWidth;

    // Scratch DIB: all glyphs in a row, white on black, grayscale antialiasing
    // so the green channel is a clean coverage value.
    HDC dc = CreateCompatibleDC(nullptr);
    BITMAPINFO info{};
    info.bmiHeader.biSize = sizeof(info.bmiHeader);
    info.bmiHeader.biWidth = cellWidth * kGlyphCount;
    info.bmiHeader.biHeight = -kRowHeight;
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    info.bmiHeader.biCompression = BI_RGB;
    void* bits = nullptr;
    HBITMAP bitmap = CreateDIBSection(dc, &info, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (!dc || !bitmap) return false;
    HGDIOBJ oldBitmap = SelectObject(dc, bitmap);
    HGDIOBJ oldFont = SelectObject(dc, font);
    SetBkColor(dc, RGB(0, 0, 0));
    SetTextColor(dc, RGB(255, 255, 255));
    RECT all{0, 0, cellWidth * kGlyphCount, kRowHeight};
    ExtTextOutW(dc, 0, 0, ETO_OPAQUE, &all, L"", 0, nullptr);
    for (int i = 0; i < kGlyphCount; ++i) {
        const wchar_t ch = static_cast<wchar_t>(kFirstGlyph + i);
        ExtTextOutW(dc, i * cellWidth, 2, ETO_CLIPPED, &all, &ch, 1, nullptr);
    }
    GdiFlush();

    const auto* pixels = static_cast<const uint32_t*>(bits);
    for (int g = 0; g < kGlyphCount; ++g) {
        for (int row = 0; row < kRowHeight; ++row) {
            for (int col = 0; col < cellWidth; ++col) {
                const uint32_t pixel = pixels[row * (cellWidth * kGlyphCount) + g * cellWidth + col];
                g_atlas.alpha[(g * kRowHeight + row) * cellWidth + col] = static_cast<uint8_t>((pixel >> 8) & 0xff);
            }
        }
    }
    SelectObject(dc, oldFont);
    SelectObject(dc, oldBitmap);
    DeleteObject(bitmap);
    DeleteDC(dc);
    g_atlas.ok = true;
    return true;
}

void fillRect(const Surface& surface, int x, int y, int w, int h, COLORREF color) {
    auto* pixels = static_cast<uint32_t*>(surface.bits);
    const uint32_t value = dibColor(color);
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > kWidth) w = kWidth - x;
    if (y + h > kHeight) h = kHeight - y;
    for (int row = 0; row < h; ++row) {
        uint32_t* line = pixels + static_cast<size_t>(y + row) * kWidth + x;
        for (int col = 0; col < w; ++col) line[col] = value;
    }
}

void drawLine(const Surface& surface, int y, const wchar_t* text, COLORREF color, COLORREF background = kBackground) {
    if (y < 0 || y + kRowHeight > kHeight) return;
    fillRect(surface, kMargin, y, kWidth - kMargin * 2, kRowHeight, background);
    if (!g_atlas.ok) return;

    auto* pixels = static_cast<uint32_t*>(surface.bits);
    const int fr = GetRValue(color), fg = GetGValue(color), fb = GetBValue(color);
    const int br = GetRValue(background), bg = GetGValue(background), bb = GetBValue(background);
    const int cellWidth = g_atlas.cellWidth;
    int x = kMargin + 4;
    for (const wchar_t* ch = text; *ch; ++ch) {
        if (x + cellWidth > kWidth - kMargin) break;
        int glyph = static_cast<int>(*ch) - kFirstGlyph;
        if (glyph < 0 || glyph >= kGlyphCount) glyph = static_cast<int>(L'?') - kFirstGlyph;
        const uint8_t* alpha = g_atlas.alpha + static_cast<size_t>(glyph) * kRowHeight * cellWidth;
        for (int row = 0; row < kRowHeight; ++row) {
            uint32_t* line = pixels + static_cast<size_t>(y + row) * kWidth + x;
            const uint8_t* coverage = alpha + row * cellWidth;
            for (int col = 0; col < cellWidth; ++col) {
                const int a = coverage[col];
                if (a == 0) continue;
                const int r = br + ((fr - br) * a) / 255;
                const int g = bg + ((fg - bg) * a) / 255;
                const int b = bb + ((fb - bb) * a) / 255;
                line[col] = (static_cast<uint32_t>(r) << 16) | (static_cast<uint32_t>(g) << 8) | static_cast<uint32_t>(b);
            }
        }
        x += cellWidth;
    }
}

void formatBytes(uint64_t bytes, wchar_t* out, size_t chars) {
    if (bytes >= (1ull << 30)) swprintf_s(out, chars, L"%.1fG", bytes / 1073741824.0);
    else if (bytes >= (1ull << 20)) swprintf_s(out, chars, L"%lluM", bytes >> 20);
    else swprintf_s(out, chars, L"%lluK", bytes >> 10);
}

bool matchesFilter(const ProcRecord& record) {
    if (g_ui.filterLen == 0) return true;
    wchar_t lower[kImageChars];
    for (uint32_t i = 0; i < kImageChars; ++i) {
        lower[i] = static_cast<wchar_t>(towlower(record.image[i]));
        if (!record.image[i]) break;
    }
    return wcsstr(lower, g_ui.filter) != nullptr;
}

const wchar_t* sortName(SortMode mode) {
    switch (mode) {
    case SortMode::Name: return L"name";
    case SortMode::Pid: return L"pid";
    default: return L"score";
    }
}

// Full rebuild in the chosen order. Score order is the sampler's ranking;
// name and pid are sorted here on a scratch index array (no allocation).
void buildRows(const Snapshot& snapshot) {
    static uint16_t order[kMaxRecords];
    const uint32_t count = snapshot.count;
    for (uint32_t i = 0; i < count; ++i) order[i] = snapshot.ranked[i];

    if (g_ui.sort == SortMode::Name) {
        std::sort(order, order + count, [&](uint16_t a, uint16_t b) {
            const int cmp = _wcsicmp(snapshot.records[a].image, snapshot.records[b].image);
            if (cmp != 0) return cmp < 0;
            return snapshot.records[a].pid < snapshot.records[b].pid;
        });
    } else if (g_ui.sort == SortMode::Pid) {
        std::sort(order, order + count, [&](uint16_t a, uint16_t b) {
            return snapshot.records[a].pid < snapshot.records[b].pid;
        });
    }

    g_ui.rowCount = 0;
    for (uint32_t i = 0; i < count; ++i) {
        const ProcRecord& record = snapshot.records[order[i]];
        if (!matchesFilter(record)) continue;
        g_ui.rows[g_ui.rowCount++] = RowKey{record.pid, record.createTime};
    }
    g_ui.rebuild = false;
    g_ui.lastSequence = snapshot.sequence;
}

// Incremental update for a new snapshot: keep the order, drop rows whose
// process is gone, append processes that appeared since the last build.
void syncRows(const Snapshot& snapshot) {
    static bool present[kMaxRecords];
    memset(present, 0, sizeof(present));

    uint32_t kept = 0;
    for (uint32_t i = 0; i < g_ui.rowCount; ++i) {
        const ProcRecord* record = findRecord(snapshot, g_ui.rows[i].pid);
        if (!record || record->createTime != g_ui.rows[i].createTime) continue;
        present[record - snapshot.records] = true;
        g_ui.rows[kept++] = g_ui.rows[i];
    }
    g_ui.rowCount = kept;

    for (uint32_t i = 0; i < snapshot.count && g_ui.rowCount < kMaxRecords; ++i) {
        const uint16_t index = snapshot.ranked[i];
        if (present[index]) continue;
        const ProcRecord& record = snapshot.records[index];
        if (!matchesFilter(record)) continue;
        g_ui.rows[g_ui.rowCount++] = RowKey{record.pid, record.createTime};
    }
    g_ui.lastSequence = snapshot.sequence;
}

void refreshRows(const Snapshot& snapshot) {
    if (g_ui.rebuild || g_ui.rowCount == 0) buildRows(snapshot);
    else if (snapshot.sequence != g_ui.lastSequence) syncRows(snapshot);
}

const ProcRecord* rowRecord(const Snapshot& snapshot, uint32_t row) {
    const ProcRecord* record = findRecord(snapshot, g_ui.rows[row].pid);
    return (record && record->createTime == g_ui.rows[row].createTime) ? record : nullptr;
}

int selectedRow() {
    for (uint32_t i = 0; i < g_ui.rowCount; ++i) {
        if (g_ui.rows[i].pid == g_ui.selectedPid && g_ui.rows[i].createTime == g_ui.selectedCreateTime) return static_cast<int>(i);
    }
    return -1;
}

void selectRow(int row) {
    if (g_ui.rowCount == 0) {
        g_ui.selectedPid = 0;
        return;
    }
    if (row < 0) row = 0;
    if (row >= static_cast<int>(g_ui.rowCount)) row = static_cast<int>(g_ui.rowCount) - 1;
    g_ui.selectedPid = g_ui.rows[row].pid;
    g_ui.selectedCreateTime = g_ui.rows[row].createTime;
}

uint32_t rowsThatFit() {
    const int available = kHeight - kMargin * 2 - kRowHeight * 5;   // title, header, gap, two status lines
    return static_cast<uint32_t>(available / kRowHeight);
}

uint32_t rowsShown() {
    const uint32_t fit = rowsThatFit();
    const uint32_t limit = g_ui.showAll ? fit : (std::min)(fit, static_cast<uint32_t>(g_rowLimit.load()));
    return (std::min)(limit, g_ui.rowCount);
}

void render(Surface& surface) {
    const Snapshot* snapshot = snapshotAcquire();
    const SamplerStatus sampler = samplerStatus();

    fillRect(surface, 0, 0, kWidth, kHeight, kBackground);
    int y = kMargin;

    wchar_t line[256];
    swprintf_s(line, L"KeyRail rescue   %ls   sort: %ls (S changes, R re-sorts now)   %ls",
        g_ui.tier == Tier::Private ? L"[private desktop]" : L"[fallback overlay]",
        sortName(g_ui.sort),
        snapshot ? L"" : L"no sample yet");
    drawLine(surface, y, line, kTitle);
    y += kRowHeight;

    drawLine(surface, y, L"PID     IMAGE                     CPU%    PRIV   HFLT/s  SFLT/s  THR R/W    FLAGS      NAME", kHeader);
    y += kRowHeight;

    if (snapshot) {
        refreshRows(*snapshot);

        int selected = selectedRow();
        if (selected < 0) {
            selectRow(0);
            selected = g_ui.rowCount ? 0 : -1;
        }

        const uint32_t shown = rowsShown();
        if (selected >= 0) {
            if (static_cast<uint32_t>(selected) < g_ui.scroll) g_ui.scroll = static_cast<uint32_t>(selected);
            if (shown && static_cast<uint32_t>(selected) >= g_ui.scroll + shown) g_ui.scroll = static_cast<uint32_t>(selected) - shown + 1;
        }
        if (g_ui.scroll + shown > g_ui.rowCount) g_ui.scroll = g_ui.rowCount > shown ? g_ui.rowCount - shown : 0;

        for (uint32_t i = 0; i < shown; ++i) {
            const uint32_t rowIndex = g_ui.scroll + i;
            const ProcRecord* recordPtr = rowRecord(*snapshot, rowIndex);
            if (!recordPtr) continue;
            const ProcRecord& record = *recordPtr;

            // A child with the same image as its parent is indented: that is
            // what tells forty chrome.exe rows apart from the one that owns
            // them, and it costs nothing.
            wchar_t image[kImageChars + 4];
            const ProcRecord* parent = findRecord(*snapshot, record.parentPid);
            if (parent && parent->createTime <= record.createTime && !wcscmp(parent->image, record.image)) {
                swprintf_s(image, L"  > %ls", record.image);
            } else {
                wcscpy_s(image, record.image);
            }

            wchar_t priv[16];
            formatBytes(record.privateBytes, priv, 16);

            wchar_t flags[40] = L"";
            if (record.flags & kFlagHungWindow) wcscat_s(flags, L"HUNG ");
            else if (record.flags & kFlagHungByState) wcscat_s(flags, L"HUNG? ");
            if (record.flags & kFlagPaging) wcscat_s(flags, L"PAGING ");
            if (record.flags & kFlagSelf) wcscat_s(flags, L"SELF ");
            const bool denied = isDenied(record.pid, record.image);
            if (denied) wcscat_s(flags, L"PROTECTED ");

            // Tier 2 (friendly name) is a cache lookup; Tier 1 (title) was read
            // by the sampler. A miss shows nothing extra, never a wait.
            wchar_t name[kFriendlyChars + kTitleChars + 4] = L"";
            wchar_t friendly[kFriendlyChars];
            if (metaLookup(record.pid, record.createTime, friendly)) wcscpy_s(name, friendly);
            if (record.title[0]) {
                if (name[0]) wcscat_s(name, L" - ");
                wcscat_s(name, record.title);
            }

            swprintf_s(line, L"%-7u %-24.24ls %6.1f %7ls %7.0f %7.0f %4u/%-5u %-10.10ls %ls",
                record.pid, image, record.cpuCores * 100.0, priv,
                record.hardFaultsPerSec, record.softFaultsPerSec,
                record.runnable, record.waiting, flags, name);

            COLORREF color = kText;
            if (denied) color = kDenied;
            else if (record.flags & (kFlagHungWindow | kFlagHungByState)) color = kHung;
            else if (record.flags & kFlagPaging) color = kPaging;

            const bool isSelected = static_cast<int>(rowIndex) == selected;
            drawLine(surface, y, line, isSelected ? kSelectedText : color, isSelected ? kSelectedBg : kBackground);
            y += kRowHeight;
        }

        if (g_ui.rowCount > shown) {
            swprintf_s(line, L"... %u more (%ls)", g_ui.rowCount - shown, g_ui.showAll ? L"PgUp/PgDn scroll" : L"PgDn shows all");
            drawLine(surface, y, line, kDim);
        }
    }

    // Status, bottom two lines.
    int statusY = kHeight - kMargin - kRowHeight * 2;
    const OverlayStatus status = overlayStatus();
    swprintf_s(line, L"trigger->pixel %u.%u ms   sample %u.%u ms   %u procs   tick %u%ls%ls",
        status.lastTriggerToPixelMicros / 1000, (status.lastTriggerToPixelMicros / 100) % 10,
        sampler.lastTickMicros / 1000, (sampler.lastTickMicros / 100) % 10,
        snapshot ? snapshot->count : 0, snapshot ? snapshot->sequence : 0,
        sampler.bufferTooSmall ? L"   BUFFER TRUNCATED" : L"",
        sampler.memoryLocked ? L"" : L"   MEMORY NOT LOCKED");
    drawLine(surface, statusY, line, kDim);
    statusY += kRowHeight;

    const PauseInfo paused = pausedInfo();
    if (g_ui.confirm == 1) {
        swprintf_s(line, L"Kill %u %ls ?   Y = yes   N = no", g_ui.confirmPid, g_ui.confirmImage);
        drawLine(surface, statusY, line, kConfirm);
    } else if (paused.active) {
        swprintf_s(line, L"PAUSED %u %ls: %ls   Enter kills it   Esc resumes it", paused.pid, paused.image, culpritName(paused.why));
        drawLine(surface, statusY, line, kConfirm);
    } else if (g_ui.filterMode) {
        swprintf_s(line, L"filter: %ls_     (Enter keeps it, Esc clears it)", g_ui.filter);
        drawLine(surface, statusY, line, kTitle);
    } else {
        wchar_t result[kResultChars];
        AcquireSRWLockShared(&g_resultLock);
        wcscpy_s(result, g_result);
        ReleaseSRWLockShared(&g_resultLock);
        swprintf_s(line, L"Up/Down select   Enter kill   C close   / filter   S sort   R re-sort   PgDn all   Esc leave   %ls%ls",
            result[0] ? L"|  " : L"", result);
        drawLine(surface, statusY, line, kText);
    }

    if (snapshot) snapshotRelease(snapshot);
}

void present(Surface& surface) {
    HDC dc = GetDC(surface.hwnd);
    if (!dc) return;
    BitBlt(dc, 0, 0, kWidth, kHeight, surface.memDc, 0, 0, SRCCOPY);
    GdiFlush();
    ReleaseDC(surface.hwnd, dc);
}

void refresh(Surface& surface) {
    render(surface);
    present(surface);
}

// ---- show / dismiss ---------------------------------------------------------------

void dismiss(Surface& surface) {
    // The private window stays shown on its hidden desktop, ready for next
    // time; only the fallback window is on a desktop the user can see.
    if (g_ui.tier != Tier::Private) ShowWindow(surface.hwnd, SW_HIDE);
    if (g_ui.tier == Tier::Private) {
        HDESK back = g_ui.returnDesktop ? g_ui.returnDesktop : g_homeDesktop;
        if (!SwitchDesktop(back) && back != g_homeDesktop) SwitchDesktop(g_homeDesktop);
        if (g_ui.returnDesktop) {
            CloseDesktop(g_ui.returnDesktop);
            g_ui.returnDesktop = nullptr;
        }
    }
    resumePaused();
    g_ui.tier = Tier::None;
    g_ui.surface = nullptr;
    g_ui.confirm = 0;
    g_ui.filterMode = false;
    g_ui.showAll = false;
    g_visible.store(false);
    samplerSetBusy(false);

    AcquireSRWLockExclusive(&g_statusLock);
    g_status.visible = false;
    ReleaseSRWLockExclusive(&g_statusLock);
    log(L"rescue menu closed");
}

void show(Surface& surface, Tier tier) {
    g_ui.tier = tier;
    g_ui.surface = &surface;
    g_ui.scroll = 0;
    g_ui.rebuild = true;
    g_ui.lastKeyQpc = qpcNow();
    g_visible.store(true);

    // Order matters under thrash. On the private desktop the window is
    // already shown, so pixels go up first (switch + memory blend + one
    // BitBlt) and activation comes after: Windows clears the foreground on a
    // desktop switch, and re-activating waits on the old foreground window,
    // which is typically the frozen app (5.6 s measured). Keys become live
    // when that finishes; the user is not staring at a black screen meanwhile.
    const int64_t showStart = qpcNow();
    if (tier != Tier::Private) {
        ShowWindow(surface.hwnd, SW_SHOW);
        SetWindowPos(surface.hwnd, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_SHOWWINDOW);
    }
    const int64_t shown = qpcNow();

    render(surface);
    const int64_t rendered = qpcNow();
    present(surface);
    const int64_t presented = qpcNow();
    g_pendingSinceQpc.store(0);
    const uint32_t micros = g_ui.triggerQpc ? microsSince(g_ui.triggerQpc) : 0;

    if (GetForegroundWindow() != surface.hwnd) {
        SetForegroundWindow(surface.hwnd);
    }
    // SetFocus only works from the window's own thread; activation already
    // routes keys to it, this just makes sure.
    PostMessageW(surface.hwnd, WM_RESCUE_FOCUS, 0, 0);
    const int64_t activated = qpcNow();

    {
        const auto ms = [](int64_t from, int64_t to) {
            return static_cast<double>(to - from) * 1000.0 / static_cast<double>(g_qpcFrequency.QuadPart);
        };
        log(L"show stages: wake %.1f  desktop-switch %.1f  window %.1f  render %.1f  present %.1f  activate %.1f ms",
            g_ui.triggerQpc ? ms(g_ui.triggerQpc, g_ui.wakeQpc) : 0.0,
            ms(g_ui.wakeQpc, g_ui.switchQpc ? g_ui.switchQpc : showStart),
            ms(showStart, shown), ms(shown, rendered), ms(rendered, presented), ms(presented, activated));
    }
    AcquireSRWLockExclusive(&g_statusLock);
    g_status.visible = true;
    ++g_status.shows;
    g_status.lastTriggerToPixelMicros = micros;
    wcscpy_s(g_status.lastTier, tier == Tier::Private ? L"private desktop" : L"fallback overlay");
    ReleaseSRWLockExclusive(&g_statusLock);
    log(L"rescue menu shown on %ls, trigger->pixel %u.%u ms",
        tier == Tier::Private ? L"private desktop" : L"fallback overlay", micros / 1000, (micros / 100) % 10);

    // Now the pixels are up, redraw once with the timing filled in.
    refresh(surface);
}

// Runs on the show thread when the trigger event fires. That thread pumps
// no messages, so nothing Windows sends a window can delay it.
void onTrigger() {
    if (g_visible.load()) return;
    g_ui.triggerQpc = lastTriggerQpc();
    g_ui.wakeQpc = qpcNow();
    g_ui.switchQpc = 0;
    g_pendingSinceQpc.store(g_ui.wakeQpc);
    samplerSetBusy(true);

    // Stop the cause before fighting the display for pixels. Only when the
    // evidence is unambiguous; otherwise the menu comes up untouched.
    if (g_autoPause.load()) {
        const Snapshot* snapshot = snapshotAcquire();
        if (snapshot) {
            Culprit why = Culprit::None;
            const ProcRecord* culprit = findCulprit(*snapshot, &why);
            if (culprit && pauseProcess(*culprit, why)) {
                g_ui.selectedPid = culprit->pid;
                g_ui.selectedCreateTime = culprit->createTime;
            }
            snapshotRelease(snapshot);
        }
    }

    if (g_private.ok && g_rescueDesktop) {
        // Remember where the user was so Esc goes back there, not blindly to
        // Default. If the input desktop cannot be opened, Default it is.
        g_ui.returnDesktop = OpenInputDesktop(0, FALSE, DESKTOP_SWITCHDESKTOP);
        if (SwitchDesktop(g_rescueDesktop)) {
            g_ui.switchQpc = qpcNow();
            show(g_private, Tier::Private);
            return;
        }
        log(L"SwitchDesktop refused (error %lu); using fallback overlay", GetLastError());
        if (g_ui.returnDesktop) {
            CloseDesktop(g_ui.returnDesktop);
            g_ui.returnDesktop = nullptr;
        }
    }

    if (g_fallback.ok) {
        show(g_fallback, Tier::Fallback);
        return;
    }

    samplerSetBusy(false);
    g_pendingSinceQpc.store(0);
    log(L"rescue trigger fired but no overlay surface is available");
}

// Trigger-thread hook: a second press while the first is still not on screen.
void onRepeatedPress() {
    const int64_t since = g_pendingSinceQpc.load();
    if (since == 0) return;
    if (microsSince(since) < kEscalateAfterMicros) return;
    if (killPausedNow()) g_pendingSinceQpc.store(0);
}

// ---- keys -------------------------------------------------------------------------------

void moveSelection(Surface& surface, int delta) {
    const Snapshot* snapshot = snapshotAcquire();
    if (!snapshot) return;
    refreshRows(*snapshot);
    int row = selectedRow();
    selectRow(row < 0 ? 0 : row + delta);
    snapshotRelease(snapshot);
    refresh(surface);
}

void resort(Surface& surface, SortMode mode) {
    g_ui.sort = mode;
    g_ui.rebuild = true;
    g_ui.scroll = 0;
    refresh(surface);
}

void beginConfirm(Surface& surface) {
    const Snapshot* snapshot = snapshotAcquire();
    if (!snapshot) return;
    const ProcRecord* record = findRecord(*snapshot, g_ui.selectedPid);
    if (record && record->createTime == g_ui.selectedCreateTime) {
        if (isDenied(record->pid, record->image)) {
            log(L"kill %u %ls refused before confirm: deny-listed", record->pid, record->image);
            overlaySetResult(L"refused: terminating that process would crash Windows");
        } else {
            g_ui.confirm = 1;
            g_ui.confirmPid = record->pid;
            g_ui.confirmCreateTime = record->createTime;
            wcscpy_s(g_ui.confirmImage, record->image);
        }
    }
    snapshotRelease(snapshot);
    refresh(surface);
}

void closeSelected(Surface& surface) {
    const Snapshot* snapshot = snapshotAcquire();
    if (!snapshot) return;
    const ProcRecord* record = findRecord(*snapshot, g_ui.selectedPid);
    if (record && record->createTime == g_ui.selectedCreateTime) requestClose(record->pid, record->createTime);
    snapshotRelease(snapshot);
    refresh(surface);
}

bool handleKey(Surface& surface, WPARAM vk) {
    if (g_ui.confirm == 1) {
        if (vk == 'Y') {
            requestKill(g_ui.confirmPid, g_ui.confirmCreateTime);
            g_ui.confirm = 0;
        } else if (vk == 'N' || vk == VK_ESCAPE) {
            g_ui.confirm = 0;
        } else {
            return true;
        }
        refresh(surface);
        return true;
    }

    if (g_ui.filterMode) {
        if (vk == VK_ESCAPE) {
            g_ui.filterMode = false;
            g_ui.filterLen = 0;
            g_ui.filter[0] = L'\0';
            g_ui.rebuild = true;
        } else if (vk == VK_RETURN) {
            g_ui.filterMode = false;
        } else if (vk == VK_BACK) {
            if (g_ui.filterLen) g_ui.filter[--g_ui.filterLen] = L'\0';
            g_ui.rebuild = true;
        } else if (vk == VK_UP || vk == VK_DOWN) {
            moveSelection(surface, vk == VK_UP ? -1 : 1);
            return true;
        } else {
            return true;   // WM_CHAR fills the filter
        }
        refresh(surface);
        return true;
    }

    switch (vk) {
    case VK_ESCAPE:
        dismiss(surface);
        return true;
    case VK_UP:
        moveSelection(surface, -1);
        return true;
    case VK_DOWN:
        moveSelection(surface, 1);
        return true;
    case VK_NEXT:
        if (!g_ui.showAll) g_ui.showAll = true;
        else moveSelection(surface, static_cast<int>(rowsThatFit()));
        refresh(surface);
        return true;
    case VK_PRIOR:
        if (g_ui.showAll && g_ui.scroll == 0) g_ui.showAll = false;
        else moveSelection(surface, -static_cast<int>(rowsThatFit()));
        refresh(surface);
        return true;
    case VK_HOME:
        moveSelection(surface, -static_cast<int>(kMaxRecords));
        return true;
    case VK_END:
        moveSelection(surface, static_cast<int>(kMaxRecords));
        return true;
    case VK_RETURN:
        beginConfirm(surface);
        return true;
    case 'C':
        closeSelected(surface);
        return true;
    case 'S':
        resort(surface, g_ui.sort == SortMode::Score ? SortMode::Name
            : g_ui.sort == SortMode::Name ? SortMode::Pid : SortMode::Score);
        return true;
    case 'R':
        resort(surface, g_ui.sort);
        return true;
    case VK_OEM_2:   // '/'
        g_ui.filterMode = true;
        refresh(surface);
        return true;
    default:
        return false;
    }
}

void handleChar(Surface& surface, wchar_t ch) {
    if (!g_ui.filterMode) return;
    if (ch < 32 || ch == L'/') return;
    if (g_ui.filterLen + 1 >= kFilterChars) return;
    g_ui.filter[g_ui.filterLen++] = static_cast<wchar_t>(towlower(ch));
    g_ui.filter[g_ui.filterLen] = L'\0';
    g_ui.rebuild = true;
    refresh(surface);
}

// ---- window ------------------------------------------------------------------------

LRESULT CALLBACK overlayProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
    auto* surface = reinterpret_cast<Surface*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (message == WM_NCCREATE) {
        auto* create = reinterpret_cast<CREATESTRUCTW*>(lParam);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(create->lpCreateParams));
        return DefWindowProcW(hwnd, message, wParam, lParam);
    }
    if (!surface) return DefWindowProcW(hwnd, message, wParam, lParam);

    switch (message) {
    case WM_RESCUE_SHOW:
        show(*surface, wParam ? Tier::Private : Tier::Fallback);
        return 0;
    case WM_RESCUE_REFRESH:
        if (g_ui.surface == surface) refresh(*surface);
        return 0;
    case WM_RESCUE_FOCUS:
        if (g_ui.surface == surface) SetFocus(hwnd);
        return 0;
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(hwnd, &ps);
        BitBlt(dc, 0, 0, kWidth, kHeight, surface->memDc, 0, 0, SRCCOPY);
        EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_ERASEBKGND:
        return 1;
    case WM_KEYDOWN:
    case WM_SYSKEYDOWN:
        g_ui.lastKeyQpc = qpcNow();
        if (g_ui.surface == surface && handleKey(*surface, wParam)) return 0;
        break;
    case WM_CHAR:
        if (g_ui.surface == surface) handleChar(*surface, static_cast<wchar_t>(wParam));
        return 0;
    case WM_MOUSEACTIVATE:
        return MA_ACTIVATE;
    case WM_CLOSE:
        if (g_ui.surface == surface) dismiss(*surface);
        return 0;
    default:
        break;
    }
    return DefWindowProcW(hwnd, message, wParam, lParam);
}

bool createSurface(Surface& surface) {
    WNDCLASSW wc{};
    wc.lpfnWndProc = overlayProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = kWindowClass;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    if (!RegisterClassW(&wc) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) return false;

    const int x = (GetSystemMetrics(SM_CXSCREEN) - kWidth) / 2;
    const int y = (GetSystemMetrics(SM_CYSCREEN) - kHeight) / 2;
    surface.hwnd = CreateWindowExW(
        WS_EX_TOPMOST | WS_EX_TOOLWINDOW,
        kWindowClass, L"KeyRail rescue", WS_POPUP,
        x < 0 ? 0 : x, y < 0 ? 0 : y, kWidth, kHeight,
        nullptr, nullptr, wc.hInstance, &surface);
    if (!surface.hwnd) return false;

    // Detach from the theme service so uxtheme never gets involved in painting.
    SetWindowTheme(surface.hwnd, L"", L"");

    surface.memDc = CreateCompatibleDC(nullptr);
    BITMAPINFO info{};
    info.bmiHeader.biSize = sizeof(info.bmiHeader);
    info.bmiHeader.biWidth = kWidth;
    info.bmiHeader.biHeight = -kHeight;
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    info.bmiHeader.biCompression = BI_RGB;
    surface.dib = CreateDIBSection(surface.memDc, &info, DIB_RGB_COLORS, &surface.bits, nullptr, 0);
    if (!surface.memDc || !surface.dib) return false;
    surface.oldBitmap = static_cast<HBITMAP>(SelectObject(surface.memDc, surface.dib));
    VirtualLock(surface.bits, static_cast<SIZE_T>(kWidth) * kHeight * 4);

    surface.font = CreateFontW(-kFontHeight, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_TT_PRECIS, CLIP_DEFAULT_PRECIS, ANTIALIASED_QUALITY,
        FIXED_PITCH | FF_MODERN, L"Consolas");
    if (!surface.font) return false;
    surface.oldFont = static_cast<HFONT>(SelectObject(surface.memDc, surface.font));
    SIZE extent{};
    GetTextExtentPoint32W(surface.memDc, L"M", 1, &extent);
    surface.charWidth = extent.cx ? extent.cx : 8;
    if (!buildGlyphAtlas(surface.font, surface.charWidth)) return false;

    render(surface);

    if (surface.desktop) {
        // Shown and focused now, while the machine is healthy. The desktop is
        // not the active one, so nothing is visible; when the switch happens
        // the window is simply already there.
        ShowWindow(surface.hwnd, SW_SHOW);
        SetWindowPos(surface.hwnd, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_SHOWWINDOW);
        SetForegroundWindow(surface.hwnd);
        SetFocus(surface.hwnd);
        present(surface);
    }
    return true;
}

void destroySurface(Surface& surface) {
    if (surface.memDc) {
        if (surface.oldFont) SelectObject(surface.memDc, surface.oldFont);
        if (surface.oldBitmap) SelectObject(surface.memDc, surface.oldBitmap);
        DeleteDC(surface.memDc);
        surface.memDc = nullptr;
    }
    if (surface.font) DeleteObject(surface.font);
    if (surface.dib) DeleteObject(surface.dib);
    if (surface.hwnd) DestroyWindow(surface.hwnd);
    surface.font = nullptr;
    surface.dib = nullptr;
    surface.hwnd = nullptr;
}

DWORD WINAPI surfaceThread(LPVOID param) {
    auto* surface = static_cast<Surface*>(param);
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);
    SetThreadDescription(GetCurrentThread(), surface->desktop ? L"keyrail-rescue-private" : L"keyrail-rescue-fallback");

    // Must happen before this thread creates any window: a window belongs to
    // the desktop its thread was on at creation, and that cannot change later.
    if (surface->desktop && !SetThreadDesktop(surface->desktop)) {
        log(L"SetThreadDesktop failed (error %lu); private surface unavailable", GetLastError());
        SetEvent(surface->ready);
        return 1;
    }

    surface->ok = createSurface(*surface);
    SetEvent(surface->ready);
    if (!surface->ok) {
        destroySurface(*surface);
        return 1;
    }

    HANDLE waits[1] = {g_stop};
    for (;;) {
        const DWORD timeout = (g_ui.surface == surface) ? kVisibleRefreshMs : INFINITE;
        const DWORD woke = MsgWaitForMultipleObjectsEx(1, waits, timeout, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
        if (woke == WAIT_OBJECT_0) break;
        if (woke == WAIT_TIMEOUT) {
            // While on screen, redraw only when the sampler published a new
            // tick; the data changes at 1 Hz, the pixels follow.
            if (g_ui.surface == surface) {
                // Safety net for the private desktop: there is no other way
                // back to the session if this window ever stops taking keys.
                if (g_ui.tier == Tier::Private && microsSince(g_ui.lastKeyQpc) > kIdleDismissSeconds * 1000000ull) {
                    log(L"rescue menu auto-closed after %u s without a keypress", kIdleDismissSeconds);
                    dismiss(*surface);
                    continue;
                }
                const Snapshot* snapshot = snapshotAcquire();
                const bool changed = snapshot && snapshot->sequence != g_ui.lastSequence;
                if (snapshot) snapshotRelease(snapshot);
                if (changed) refresh(*surface);
            }
            continue;
        }

        MSG msg;
        bool quit = false;
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) {
                quit = true;
                break;
            }
            TranslateMessage(&msg);
            const int64_t before = qpcNow();
            DispatchMessageW(&msg);
            const uint32_t took = microsSince(before);
            // Anything slow here is the window manager waiting on the disk;
            // worth knowing which message it was.
            if (took > 100000) log(L"window message 0x%04X took %u ms", msg.message, took / 1000);
        }
        if (quit) break;
    }

    if (g_ui.surface == surface) dismiss(*surface);
    destroySurface(*surface);
    return 0;
}

DWORD WINAPI showThread(LPVOID) {
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);
    SetThreadDescription(GetCurrentThread(), L"keyrail-rescue-show");
    // Same desktop as the private window so GetDC and activation are local.
    if (g_rescueDesktop) SetThreadDesktop(g_rescueDesktop);
    HANDLE waits[2] = {g_stop, g_trigger};
    for (;;) {
        const DWORD woke = WaitForMultipleObjects(2, waits, FALSE, INFINITE);
        if (woke != WAIT_OBJECT_0 + 1) break;
        onTrigger();
    }
    return 0;
}

bool startSurface(Surface& surface, HDESK desktop, bool controller) {
    surface.desktop = desktop;
    surface.controller = controller;
    surface.ready = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    surface.thread = CreateThread(nullptr, 0, surfaceThread, &surface, 0, &surface.threadId);
    if (!surface.thread) return false;
    WaitForSingleObject(surface.ready, 5000);
    return surface.ok;
}

void stopSurface(Surface& surface) {
    if (surface.thread) {
        if (surface.threadId) PostThreadMessageW(surface.threadId, WM_QUIT, 0, 0);
        WaitForSingleObject(surface.thread, 3000);
        CloseHandle(surface.thread);
        surface.thread = nullptr;
    }
    if (surface.ready) {
        CloseHandle(surface.ready);
        surface.ready = nullptr;
    }
    surface.ok = false;
}

} // namespace

bool overlayStart(const RescueSettings& settings, std::wstring* report) {
    if (g_trigger) return true;
    QueryPerformanceFrequency(&g_qpcFrequency);
    g_autoPause.store(settings.autoPause);
    g_rowLimit.store(settings.rowCount);
    g_titleDeadlineMs.store(settings.titleDeadlineMs);

    g_trigger = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    g_stop = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    g_homeDesktop = GetThreadDesktop(GetCurrentThreadId());

    std::wstring out;
    if (settings.usePrivateDesktop) {
        g_rescueDesktop = CreateDesktopW(kDesktopName, nullptr, nullptr, 0, GENERIC_ALL, nullptr);
        if (!g_rescueDesktop) {
            out += L"rescue private desktop unavailable (error " + std::to_wstring(GetLastError()) + L"); fallback overlay only\n";
        }
    }

    // The private surface is the controller when it exists; otherwise the
    // fallback surface listens for the trigger itself.
    const bool privateOk = g_rescueDesktop && startSurface(g_private, g_rescueDesktop, true);
    if (g_rescueDesktop && !privateOk) {
        out += L"rescue private surface failed; fallback overlay only\n";
        stopSurface(g_private);
    }
    const bool fallbackOk = startSurface(g_fallback, nullptr, !privateOk);
    if (!fallbackOk) {
        out += L"rescue fallback overlay failed\n";
        stopSurface(g_fallback);
    }

    if (!privateOk && !fallbackOk) {
        if (report) *report = out + L"rescue overlay unavailable\n";
        return false;
    }

    triggerSetPressHook(onRepeatedPress);
    g_showThread = CreateThread(nullptr, 0, showThread, nullptr, 0, nullptr);
    if (!g_showThread) {
        stopSurface(g_private);
        stopSurface(g_fallback);
        if (report) *report = out + L"rescue overlay unavailable: show thread failed\n";
        return false;
    }

    AcquireSRWLockExclusive(&g_statusLock);
    g_status.started = true;
    g_status.privateDesktop = privateOk;
    ReleaseSRWLockExclusive(&g_statusLock);

    out += std::wstring(L"rescue overlay ready: ") + (privateOk ? L"private desktop" : L"fallback only")
        + (privateOk && fallbackOk ? L" + fallback" : L"") + L"\n";
    if (report) *report = out;
    return true;
}

void overlayStop() {
    if (!g_trigger) return;
    triggerSetPressHook(nullptr);
    SetEvent(g_stop);
    if (g_showThread) {
        WaitForSingleObject(g_showThread, 3000);
        CloseHandle(g_showThread);
        g_showThread = nullptr;
    }
    stopSurface(g_private);
    stopSurface(g_fallback);
    if (g_rescueDesktop) {
        CloseDesktop(g_rescueDesktop);
        g_rescueDesktop = nullptr;
    }
    CloseHandle(g_trigger);
    CloseHandle(g_stop);
    g_trigger = nullptr;
    g_stop = nullptr;
    AcquireSRWLockExclusive(&g_statusLock);
    g_status.started = false;
    ReleaseSRWLockExclusive(&g_statusLock);
}

void overlayApply(const RescueSettings& settings) {
    g_autoPause.store(settings.autoPause);
    g_rowLimit.store(settings.rowCount);
    g_titleDeadlineMs.store(settings.titleDeadlineMs);
}

HANDLE overlayTriggerEvent() {
    return g_trigger;
}

bool overlayVisible() {
    return g_visible.load();
}

OverlayStatus overlayStatus() {
    AcquireSRWLockShared(&g_statusLock);
    OverlayStatus copy = g_status;
    ReleaseSRWLockShared(&g_statusLock);
    return copy;
}

bool overlayDismiss() {
    Surface* surface = g_ui.surface;
    if (!g_visible.load() || !surface || !surface->hwnd) return false;
    PostMessageW(surface->hwnd, WM_CLOSE, 0, 0);
    return true;
}

void overlaySetResult(const wchar_t* text) {
    AcquireSRWLockExclusive(&g_resultLock);
    wcsncpy_s(g_result, text, _TRUNCATE);
    ReleaseSRWLockExclusive(&g_resultLock);
    Surface* surface = g_ui.surface;
    if (surface && surface->hwnd) PostMessageW(surface->hwnd, WM_RESCUE_REFRESH, 0, 0);
}

} // namespace rescue
