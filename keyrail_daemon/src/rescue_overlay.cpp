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
// One concept: a view is both which order the list is in and what the
// chip bar shows. Suspects is the ranking; the rest are plain sorts.
enum class SortMode { Score, Memory, Cpu, Disk, Name };
constexpr int kViewCount = 5;

// Rows are generated every render from two pieces of persistent state: the
// order of process groups (fixed when the list is (re)built, so nothing moves
// under the cursor; dead groups drop, new ones append) and which groups are
// expanded. A group is every process sharing an image name; a group of one
// renders as a plain process row. Groups whose members own a visible window
// are the APPS section, the rest BACKGROUND PROCESSES, like Task Manager.
enum class RowKind : uint8_t { Apps, Background, Group, Process };

struct Row {
    RowKind kind;
    uint16_t group;      // index into the per-render group table
    uint16_t record;     // snapshot record for Process rows
};

constexpr uint32_t kMaxGroups = 2048;          // power of two
constexpr uint32_t kMaxRows = kMaxRecords + kMaxGroups + 2;
constexpr uint32_t kMaxExpanded = 64;

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
    bool followSelection = true;       // keyboard moved the cursor: keep it on screen (wheel turns this off)
    wchar_t selectedGroup[kImageChars] = {};   // non-empty when a group row is selected
    wchar_t confirmGroup[kImageChars] = {};    // non-empty when confirming a group kill
    uint32_t confirmCount = 0;
    Row rows[kMaxRows] = {};
    uint32_t rowCount = 0;
    uint32_t processRows = 0;          // rows that are processes or groups (for "N of M")
    uint32_t appsCount = 0;            // groups in each section, for the headers
    uint32_t backgroundCount = 0;
    // Persistent group order and expanded set, by image name.
    wchar_t order[kMaxGroups][kImageChars] = {};
    uint32_t orderCount = 0;
    wchar_t expanded[kMaxExpanded][kImageChars] = {};
    uint32_t expandedCount = 0;
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
std::atomic<int> g_style{0};        // 0 modern, 1 legacy
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

// ---- modern style ------------------------------------------------------------------
//
// Same renderer contract as the legacy look (opaque fills and pre-rendered
// glyphs into a locked DIB), laid out to the KeyRail Rescue render spec:
// 8 colours, a 13 px monospace table font, a 20 px title font, 26 px rows.

namespace modern {

constexpr COLORREF kBg = RGB(0x00, 0x00, 0x00);
constexpr COLORREF kBand = RGB(0x16, 0x15, 0x14);
constexpr COLORREF kInk = RGB(0xF3, 0xF2, 0xF2);
constexpr COLORREF kDimInk = RGB(0x7E, 0x7A, 0x77);
constexpr COLORREF kSel = RGB(0x2B, 0x27, 0x24);
constexpr COLORREF kAccent = RGB(0xEC, 0x30, 0x13);
constexpr COLORREF kWarn = RGB(0xE3, 0xA0, 0x08);
constexpr COLORREF kProtected = RGB(0x4A, 0x46, 0x44);
constexpr COLORREF kOnAccent = RGB(0xFF, 0xF2, 0xEF);

constexpr int kPad = 20;
constexpr int kTitleBar = 56;
constexpr int kChipBar = 36;
constexpr int kBanner = 58;
constexpr int kRow = 26;
constexpr int kStatus1 = 26;
constexpr int kStatus2 = 28;
constexpr int kCursorBar = 3;
constexpr int kBarHeight = 8;
constexpr int kBarChars = 5;
constexpr int kChipCount = kViewCount;

// A font pre-rendered into a locked coverage table. Proportional fonts keep a
// per-glyph advance; the monospace table font has one advance for all.
// ASCII plus the handful of symbols the spec uses; anything else draws as '?'.
constexpr wchar_t kExtra[] = {0x00b7, 0x2192, 0x2014, 0x2502, 0x2191, 0x2193, 0x2013, 0x25b8, 0x25be};
constexpr int kExtraCount = 9;
constexpr int kModernGlyphs = kGlyphCount + kExtraCount;

int glyphIndex(wchar_t ch) {
    const int ascii = static_cast<int>(ch) - kFirstGlyph;
    if (ascii >= 0 && ascii < kGlyphCount) return ascii;
    for (int i = 0; i < kExtraCount; ++i) {
        if (kExtra[i] == ch) return kGlyphCount + i;
    }
    return static_cast<int>(L'?') - kFirstGlyph;
}

wchar_t glyphChar(int index) {
    return index < kGlyphCount ? static_cast<wchar_t>(kFirstGlyph + index) : kExtra[index - kGlyphCount];
}

struct Atlas {
    int cellWidth = 0;      // widest glyph
    int cellHeight = 0;
    uint8_t advance[kModernGlyphs] = {};
    uint8_t* alpha = nullptr;
    bool ok = false;
};

Atlas g_mono;    // table + chrome
Atlas g_title;   // title bar + PAUSED banner

struct Chip {
    const wchar_t* label;
    RECT rect;
};
Chip g_chips[kChipCount] = {
    {L"1 SUSPECTS", {}}, {L"2 MEMORY", {}}, {L"3 CPU", {}}, {L"4 DISK", {}}, {L"5 NAME", {}},
};
int g_firstRowY = 0;     // for mouse hit-testing rows
int g_rowsDrawn = 0;
bool g_whyShown = false; // the WHY strip took a row under the selection
int g_whyRow = -1;       // drawn-row index the strip sits under
RECT g_topButton = {};   // "HOME top": scroll up and select the first row
RECT g_filterButton = {}; // "/ FILTER": click to start typing a name

bool buildAtlas(Atlas& atlas, HFONT font, int cellHeight) {
    if (atlas.ok) return true;
    HDC dc = CreateCompatibleDC(nullptr);
    if (!dc) return false;
    HGDIOBJ oldFont = SelectObject(dc, font);

    int maxAdvance = 1;
    for (int i = 0; i < kModernGlyphs; ++i) {
        int width = 0;
        const UINT code = static_cast<UINT>(glyphChar(i));
        GetCharWidth32W(dc, code, code, &width);
        if (width < 1) width = 1;
        if (width > 255) width = 255;
        atlas.advance[i] = static_cast<uint8_t>(width);
        if (width > maxAdvance) maxAdvance = width;
    }
    TEXTMETRICW metrics{};
    GetTextMetricsW(dc, &metrics);
    const int textTop = (cellHeight - metrics.tmHeight) / 2;

    atlas.cellWidth = maxAdvance + 1;
    atlas.cellHeight = cellHeight;
    const SIZE_T bytes = static_cast<SIZE_T>(kModernGlyphs) * cellHeight * atlas.cellWidth;
    atlas.alpha = static_cast<uint8_t*>(VirtualAlloc(nullptr, bytes, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
    if (!atlas.alpha) {
        SelectObject(dc, oldFont);
        DeleteDC(dc);
        return false;
    }
    VirtualLock(atlas.alpha, bytes);

    BITMAPINFO info{};
    info.bmiHeader.biSize = sizeof(info.bmiHeader);
    info.bmiHeader.biWidth = atlas.cellWidth * kModernGlyphs;
    info.bmiHeader.biHeight = -cellHeight;
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    info.bmiHeader.biCompression = BI_RGB;
    void* bits = nullptr;
    HBITMAP bitmap = CreateDIBSection(dc, &info, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (!bitmap) {
        SelectObject(dc, oldFont);
        DeleteDC(dc);
        return false;
    }
    HGDIOBJ oldBitmap = SelectObject(dc, bitmap);
    SetBkColor(dc, RGB(0, 0, 0));
    SetTextColor(dc, RGB(255, 255, 255));
    RECT all{0, 0, atlas.cellWidth * kModernGlyphs, cellHeight};
    ExtTextOutW(dc, 0, 0, ETO_OPAQUE, &all, L"", 0, nullptr);
    for (int i = 0; i < kModernGlyphs; ++i) {
        const wchar_t ch = glyphChar(i);
        ExtTextOutW(dc, i * atlas.cellWidth, textTop, ETO_CLIPPED, &all, &ch, 1, nullptr);
    }
    GdiFlush();
    const auto* pixels = static_cast<const uint32_t*>(bits);
    for (int g = 0; g < kModernGlyphs; ++g) {
        for (int row = 0; row < cellHeight; ++row) {
            for (int col = 0; col < atlas.cellWidth; ++col) {
                const uint32_t pixel = pixels[row * (atlas.cellWidth * kModernGlyphs) + g * atlas.cellWidth + col];
                atlas.alpha[(g * cellHeight + row) * atlas.cellWidth + col] = static_cast<uint8_t>((pixel >> 8) & 0xff);
            }
        }
    }
    SelectObject(dc, oldBitmap);
    SelectObject(dc, oldFont);
    DeleteObject(bitmap);
    DeleteDC(dc);
    atlas.ok = true;
    return true;
}

int textWidth(const Atlas& atlas, const wchar_t* text) {
    int width = 0;
    for (const wchar_t* ch = text; *ch; ++ch) width += atlas.advance[glyphIndex(*ch)];
    return width;
}

// Blends text at (x, y) over `background`, clipped at maxX. Returns the end x.
int drawText(const Surface& surface, const Atlas& atlas, int x, int y, const wchar_t* text,
             COLORREF color, COLORREF background, int maxX = kWidth - kPad) {
    if (!atlas.ok || y < 0 || y + atlas.cellHeight > kHeight) return x;
    auto* pixels = static_cast<uint32_t*>(surface.bits);
    const int fr = GetRValue(color), fg = GetGValue(color), fb = GetBValue(color);
    const int br = GetRValue(background), bg = GetGValue(background), bb = GetBValue(background);
    for (const wchar_t* ch = text; *ch; ++ch) {
        const int glyph = glyphIndex(*ch);
        const int advance = atlas.advance[glyph];
        if (x + advance > maxX) break;
        const uint8_t* alpha = atlas.alpha + static_cast<size_t>(glyph) * atlas.cellHeight * atlas.cellWidth;
        const int columns = (std::min)(atlas.cellWidth, maxX - x);
        for (int row = 0; row < atlas.cellHeight; ++row) {
            uint32_t* line = pixels + static_cast<size_t>(y + row) * kWidth + x;
            const uint8_t* coverage = alpha + row * atlas.cellWidth;
            for (int col = 0; col < columns; ++col) {
                const int a = coverage[col];
                if (a == 0) continue;
                const int r = br + ((fr - br) * a) / 255;
                const int g = bg + ((fg - bg) * a) / 255;
                const int b = bb + ((fb - bb) * a) / 255;
                line[col] = (static_cast<uint32_t>(r) << 16) | (static_cast<uint32_t>(g) << 8) | static_cast<uint32_t>(b);
            }
        }
        x += advance;
    }
    return x;
}

// Returns the x where the text starts, so callers can keep stacking leftwards.
int drawTextRight(const Surface& surface, const Atlas& atlas, int rightX, int y, const wchar_t* text,
                  COLORREF color, COLORREF background) {
    const int startX = rightX - textWidth(atlas, text);
    drawText(surface, atlas, startX, y, text, color, background, rightX);
    return startX;
}

// Row-height cell helpers: text sits in a kRow-tall cell, so y is the row top.
int cw() { return g_mono.advance[static_cast<int>(L'M') - kFirstGlyph]; }

void inlineBar(const Surface& surface, int x, int y, float fraction, COLORREF fill, COLORREF track) {
    const int width = kBarChars * cw();
    const int top = y + (kRow - kBarHeight) / 2;
    fillRect(surface, x, top, width, kBarHeight, track);
    if (fraction < 0.0f) fraction = 0.0f;
    if (fraction > 1.0f) fraction = 1.0f;
    const int filled = static_cast<int>(width * fraction + 0.5f);
    if (filled > 0) fillRect(surface, x, top, filled, kBarHeight, fill);
}

void formatCount(float value, wchar_t* out, size_t chars) {
    if (value < 1000.0f) swprintf_s(out, chars, L"%.0f", value);
    else if (value < 100000.0f) swprintf_s(out, chars, L"%.1fk", value / 1000.0f);
    else swprintf_s(out, chars, L"%.0fk", value / 1000.0f);
}

// One line under the selected row saying why it ranks where it does.
void reasonFor(const ProcRecord& record, const Snapshot& snapshot, wchar_t* out, size_t chars) {
    wchar_t part[160];
    out[0] = 0;
    auto append = [&](const wchar_t* text) {
        if (out[0]) wcscat_s(out, chars, L" \x00b7 ");
        wcscat_s(out, chars, text);
    };
    if (record.flags & kFlagHungWindow) append(L"not responding: its window stopped pumping messages");
    else if (record.flags & kFlagHungByState) append(L"looks hung: owns a window, nothing runnable, nobody pumping");
    if (snapshot.physicalBytes) {
        const double share = static_cast<double>((std::max)(record.privateBytes, record.workingSetBytes)) / static_cast<double>(snapshot.physicalBytes);
        if (share >= 0.15) {
            swprintf_s(part, L"owns %.0f%% of RAM%ls", share * 100.0,
                record.privGrowthPerSec >= 50.0f * 1048576 ? L" and still growing" : L"");
            append(part);
        }
    }
    if (record.hardFaultsPerSec >= 100.0f) {
        swprintf_s(part, L"%.0f hard faults/s", record.hardFaultsPerSec);
        append(part);
    }
    if (record.flags & kFlagPaging) append(L"waiting on page-ins (thrash victim)");
    if (record.flags & kFlagRealtime) append(L"realtime priority");
    if (record.cpuCores >= 0.9f) {
        swprintf_s(part, L"%.1f cores busy", record.cpuCores);
        append(part);
    }
    if (record.flags & kFlagIoStorm) {
        swprintf_s(part, L"%.0f MB/s disk, everyone else waits on it", record.ioBytesPerSec / 1048576.0f);
        append(part);
    }
    if (record.flags & kFlagSpawnStorm) {
        swprintf_s(part, L"spawning %.0f children/s", record.childSpawnsPerSec);
        append(part);
    }
    if (record.threadGrowthPerSec >= 2.0f) {
        swprintf_s(part, L"thread count climbing %.0f/s", record.threadGrowthPerSec);
        append(part);
    }
    if (!out[0]) {
        swprintf_s(part, L"nothing alarming: score %.1f", record.score);
        append(part);
    }
}

} // namespace modern

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
    case SortMode::Memory: return L"memory";
    case SortMode::Cpu: return L"cpu";
    case SortMode::Disk: return L"disk";
    case SortMode::Name: return L"name";
    default: return L"suspects";
    }
}

SortMode viewFromIndex(int index) {
    switch (index) {
    case 1: return SortMode::Memory;
    case 2: return SortMode::Cpu;
    case 3: return SortMode::Disk;
    case 4: return SortMode::Name;
    default: return SortMode::Score;
    }
}

int viewIndex(SortMode mode) {
    switch (mode) {
    case SortMode::Memory: return 1;
    case SortMode::Cpu: return 2;
    case SortMode::Disk: return 3;
    case SortMode::Name: return 4;
    default: return 0;
    }
}

// ---- groups ---------------------------------------------------------------------

struct Group {
    wchar_t image[kImageChars];
    uint16_t head;        // first member record index (linked through g_next)
    uint16_t count;
    uint16_t rep;         // representative member: highest score, windows first
    bool app;             // any member owns a visible window
    float key;            // sort key for the current view
};

Group g_groups[kMaxGroups];
uint32_t g_groupCount = 0;
uint16_t g_groupSlot[kMaxGroups * 2];   // name hash -> group index + 1
uint16_t g_next[kMaxRecords];           // member linked list, 0xFFFF ends
constexpr uint16_t kNoRecord = 0xFFFF;

uint32_t nameHash(const wchar_t* image) {
    uint32_t hash = 2166136261u;
    for (const wchar_t* ch = image; *ch; ++ch) {
        hash ^= static_cast<uint32_t>(towlower(*ch));
        hash *= 16777619u;
    }
    return hash & (kMaxGroups * 2 - 1);
}

int findGroup(const wchar_t* image) {
    uint32_t slot = nameHash(image);
    for (uint32_t probe = 0; probe < kMaxGroups * 2; ++probe) {
        const uint16_t entry = g_groupSlot[slot];
        if (entry == 0) return -1;
        if (_wcsicmp(g_groups[entry - 1].image, image) == 0) return static_cast<int>(entry - 1);
        slot = (slot + 1) & (kMaxGroups * 2 - 1);
    }
    return -1;
}

float groupKey(const Snapshot& snapshot, const Group& group, SortMode mode) {
    float key = 0.0f;
    for (uint16_t i = group.head; i != kNoRecord; i = g_next[i]) {
        const ProcRecord& record = snapshot.records[i];
        switch (mode) {
        case SortMode::Memory: key += static_cast<float>(record.privateBytes); break;
        case SortMode::Cpu: key += record.cpuCores; break;
        case SortMode::Disk: key += record.ioBytesPerSec; break;
        case SortMode::Name: break;
        default: key = (std::max)(key, record.score); break;
        }
    }
    return key;
}

void buildGroups(const Snapshot& snapshot) {
    memset(g_groupSlot, 0, sizeof(g_groupSlot));
    g_groupCount = 0;
    for (uint32_t i = 0; i < snapshot.count; ++i) {
        const ProcRecord& record = snapshot.records[i];
        int index = findGroup(record.image);
        if (index < 0) {
            if (g_groupCount >= kMaxGroups) break;
            index = static_cast<int>(g_groupCount++);
            Group& group = g_groups[index];
            wcscpy_s(group.image, record.image);
            group.head = kNoRecord;
            group.count = 0;
            group.rep = static_cast<uint16_t>(i);
            group.app = false;
            uint32_t slot = nameHash(record.image);
            while (g_groupSlot[slot] != 0) slot = (slot + 1) & (kMaxGroups * 2 - 1);
            g_groupSlot[slot] = static_cast<uint16_t>(index + 1);
        }
        Group& group = g_groups[index];
        g_next[i] = group.head;
        group.head = static_cast<uint16_t>(i);
        ++group.count;
        if (record.flags & kFlagOwnsWindow) group.app = true;
        const ProcRecord& rep = snapshot.records[group.rep];
        const bool better = (record.flags & kFlagOwnsWindow) && !(rep.flags & kFlagOwnsWindow)
            ? true
            : ((record.flags & kFlagOwnsWindow) == (rep.flags & kFlagOwnsWindow) && record.score > rep.score);
        if (better) group.rep = static_cast<uint16_t>(i);
    }
    for (uint32_t g = 0; g < g_groupCount; ++g) g_groups[g].key = groupKey(snapshot, g_groups[g], g_ui.sort);
}

bool isExpanded(const wchar_t* image) {
    for (uint32_t i = 0; i < g_ui.expandedCount; ++i) {
        if (_wcsicmp(g_ui.expanded[i], image) == 0) return true;
    }
    return false;
}

void toggleExpanded(const wchar_t* image) {
    for (uint32_t i = 0; i < g_ui.expandedCount; ++i) {
        if (_wcsicmp(g_ui.expanded[i], image) == 0) {
            g_ui.expanded[i][0] = 0;
            wcscpy_s(g_ui.expanded[i], g_ui.expanded[g_ui.expandedCount - 1]);
            --g_ui.expandedCount;
            return;
        }
    }
    if (g_ui.expandedCount < kMaxExpanded) wcscpy_s(g_ui.expanded[g_ui.expandedCount++], image);
}

void sortGroupIndices(const Snapshot& snapshot, uint16_t* indices, uint32_t count) {
    (void)snapshot;
    std::sort(indices, indices + count, [](uint16_t a, uint16_t b) {
        const Group& ga = g_groups[a];
        const Group& gb = g_groups[b];
        if (g_ui.sort == SortMode::Name) {
            const int cmp = _wcsicmp(ga.image, gb.image);
            return cmp < 0;
        }
        if (ga.key != gb.key) return ga.key > gb.key;
        return _wcsicmp(ga.image, gb.image) < 0;
    });
}

// Full rebuild: the order becomes the current view's sort.
void buildOrder(const Snapshot& snapshot) {
    static uint16_t indices[kMaxGroups];
    for (uint32_t g = 0; g < g_groupCount; ++g) indices[g] = static_cast<uint16_t>(g);
    sortGroupIndices(snapshot, indices, g_groupCount);
    g_ui.orderCount = 0;
    for (uint32_t i = 0; i < g_groupCount; ++i) wcscpy_s(g_ui.order[g_ui.orderCount++], g_groups[indices[i]].image);
    g_ui.rebuild = false;
}

// Incremental: keep the order, drop groups that vanished, append new ones.
void syncOrder(const Snapshot& snapshot) {
    static bool placed[kMaxGroups];
    memset(placed, 0, sizeof(placed));
    uint32_t kept = 0;
    for (uint32_t i = 0; i < g_ui.orderCount; ++i) {
        const int g = findGroup(g_ui.order[i]);
        if (g < 0) continue;
        placed[g] = true;
        if (kept != i) wcscpy_s(g_ui.order[kept], g_ui.order[i]);
        ++kept;
    }
    g_ui.orderCount = kept;
    static uint16_t fresh[kMaxGroups];
    uint32_t freshCount = 0;
    for (uint32_t g = 0; g < g_groupCount; ++g) {
        if (!placed[g]) fresh[freshCount++] = static_cast<uint16_t>(g);
    }
    sortGroupIndices(snapshot, fresh, freshCount);
    for (uint32_t i = 0; i < freshCount && g_ui.orderCount < kMaxGroups; ++i) {
        wcscpy_s(g_ui.order[g_ui.orderCount++], g_groups[fresh[i]].image);
    }
}

bool groupMatchesFilter(const Group& group) {
    if (g_ui.filterLen == 0) return true;
    wchar_t lower[kImageChars];
    for (uint32_t i = 0; i < kImageChars; ++i) {
        lower[i] = static_cast<wchar_t>(towlower(group.image[i]));
        if (!group.image[i]) break;
    }
    return wcsstr(lower, g_ui.filter) != nullptr;
}

void emitSection(bool app, RowKind header) {
    bool headerDone = false;
    for (uint32_t i = 0; i < g_ui.orderCount && g_ui.rowCount + 2 < kMaxRows; ++i) {
        const int g = findGroup(g_ui.order[i]);
        if (g < 0) continue;
        const Group& group = g_groups[g];
        if (group.app != app || !groupMatchesFilter(group)) continue;
        if (!headerDone) {
            g_ui.rows[g_ui.rowCount++] = Row{header, 0, 0};
            headerDone = true;
        }
        if (app) ++g_ui.appsCount; else ++g_ui.backgroundCount;
        if (group.count == 1) {
            g_ui.rows[g_ui.rowCount++] = Row{RowKind::Process, static_cast<uint16_t>(g), group.head};
            ++g_ui.processRows;
            continue;
        }
        g_ui.rows[g_ui.rowCount++] = Row{RowKind::Group, static_cast<uint16_t>(g), group.rep};
        ++g_ui.processRows;
        if (!isExpanded(group.image)) continue;
        for (uint16_t m = group.head; m != kNoRecord && g_ui.rowCount + 1 < kMaxRows; m = g_next[m]) {
            g_ui.rows[g_ui.rowCount++] = Row{RowKind::Process, static_cast<uint16_t>(g), m};
        }
    }
}

// Regenerates rows for this snapshot. Cheap: one pass over records to group
// them, one pass over the persisted order to lay rows out.
void refreshRows(const Snapshot& snapshot) {
    buildGroups(snapshot);
    if (g_ui.rebuild) buildOrder(snapshot);
    else syncOrder(snapshot);
    g_ui.rowCount = 0;
    g_ui.processRows = 0;
    g_ui.appsCount = 0;
    g_ui.backgroundCount = 0;
    emitSection(true, RowKind::Apps);
    emitSection(false, RowKind::Background);
    g_ui.lastSequence = snapshot.sequence;
}

// What a renderer needs for one row. Group rows carry a synthetic record with
// the members' numbers summed, so both renderers draw them like a process.
struct RowView {
    RowKind kind = RowKind::Process;
    ProcRecord record;
    uint32_t count = 1;
    bool expanded = false;
    bool member = false;      // process row inside an expanded group: indented
    bool denied = false;
};

void describeRow(const Snapshot& snapshot, uint32_t index, RowView& view) {
    const Row& row = g_ui.rows[index];
    view.kind = row.kind;
    view.count = 1;
    view.expanded = false;
    view.member = false;
    view.denied = false;
    if (row.kind == RowKind::Apps || row.kind == RowKind::Background) return;

    const Group& group = g_groups[row.group];
    if (row.kind == RowKind::Process) {
        view.record = snapshot.records[row.record];
        view.member = group.count > 1;
        view.denied = isDenied(view.record.pid, view.record.image);
        return;
    }

    view.record = snapshot.records[group.rep];
    view.count = group.count;
    view.expanded = isExpanded(group.image);
    ProcRecord& sum = view.record;
    sum.cpuCores = 0; sum.privateBytes = 0; sum.workingSetBytes = 0; sum.hardFaultsPerSec = 0; sum.softFaultsPerSec = 0;
    sum.ioBytesPerSec = 0; sum.runnable = 0; sum.waiting = 0; sum.threadCount = 0; sum.score = 0; sum.flags = 0;
    sum.privGrowthPerSec = 0; sum.threadGrowthPerSec = 0; sum.childSpawnsPerSec = 0;
    bool anyDenied = false;
    for (uint16_t m = group.head; m != kNoRecord; m = g_next[m]) {
        const ProcRecord& r = snapshot.records[m];
        sum.cpuCores += r.cpuCores;
        sum.privateBytes += r.privateBytes;
        sum.workingSetBytes += r.workingSetBytes;
        sum.hardFaultsPerSec += r.hardFaultsPerSec;
        sum.softFaultsPerSec += r.softFaultsPerSec;
        sum.ioBytesPerSec += r.ioBytesPerSec;
        sum.runnable = static_cast<uint16_t>(sum.runnable + r.runnable);
        sum.waiting = static_cast<uint16_t>(sum.waiting + r.waiting);
        sum.threadCount += r.threadCount;
        sum.score = (std::max)(sum.score, r.score);
        sum.flags |= r.flags;
        sum.privGrowthPerSec += r.privGrowthPerSec;
        sum.threadGrowthPerSec += r.threadGrowthPerSec;
        sum.childSpawnsPerSec += r.childSpawnsPerSec;
        if (isDenied(r.pid, r.image)) anyDenied = true;
    }
    view.denied = anyDenied;
}

bool isSectionRow(uint32_t index) {
    return g_ui.rows[index].kind == RowKind::Apps || g_ui.rows[index].kind == RowKind::Background;
}

int selectedRow() {
    for (uint32_t i = 0; i < g_ui.rowCount; ++i) {
        const Row& row = g_ui.rows[i];
        if (row.kind == RowKind::Group) {
            if (g_ui.selectedGroup[0] && _wcsicmp(g_groups[row.group].image, g_ui.selectedGroup) == 0) return static_cast<int>(i);
        } else if (row.kind == RowKind::Process && !g_ui.selectedGroup[0]) {
            (void)row;
        }
    }
    return -1;
}

// Process rows are matched by (pid, create time); needs the snapshot.
int selectedRow(const Snapshot& snapshot) {
    if (g_ui.selectedGroup[0]) return selectedRow();
    for (uint32_t i = 0; i < g_ui.rowCount; ++i) {
        const Row& row = g_ui.rows[i];
        if (row.kind != RowKind::Process) continue;
        const ProcRecord& record = snapshot.records[row.record];
        if (record.pid == g_ui.selectedPid && record.createTime == g_ui.selectedCreateTime) return static_cast<int>(i);
    }
    return -1;
}

void selectRow(const Snapshot& snapshot, int index, int direction = 1) {
    if (g_ui.rowCount == 0) {
        g_ui.selectedPid = 0;
        g_ui.selectedGroup[0] = 0;
        return;
    }
    if (index < 0) index = 0;
    if (index >= static_cast<int>(g_ui.rowCount)) index = static_cast<int>(g_ui.rowCount) - 1;
    // Never land on a section header.
    while (index >= 0 && index < static_cast<int>(g_ui.rowCount) && isSectionRow(static_cast<uint32_t>(index))) index += direction;
    if (index < 0 || index >= static_cast<int>(g_ui.rowCount)) {
        index = direction > 0 ? static_cast<int>(g_ui.rowCount) - 1 : 0;
        while (index >= 0 && index < static_cast<int>(g_ui.rowCount) && isSectionRow(static_cast<uint32_t>(index))) index -= direction;
        if (index < 0 || index >= static_cast<int>(g_ui.rowCount)) return;
    }
    const Row& row = g_ui.rows[static_cast<uint32_t>(index)];
    if (row.kind == RowKind::Group) {
        wcscpy_s(g_ui.selectedGroup, g_groups[row.group].image);
        g_ui.selectedPid = 0;
    } else {
        g_ui.selectedGroup[0] = 0;
        const ProcRecord& record = snapshot.records[row.record];
        g_ui.selectedPid = record.pid;
        g_ui.selectedCreateTime = record.createTime;
    }
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

void renderLegacy(Surface& surface) {
    const Snapshot* snapshot = snapshotAcquire();
    const SamplerStatus sampler = samplerStatus();

    fillRect(surface, 0, 0, kWidth, kHeight, kBackground);
    int y = kMargin;

    wchar_t line[256];
    swprintf_s(line, L"KeyRail rescue   %ls   view: %ls (1-5 change, R refreshes order)   %ls",
        g_ui.tier == Tier::Private ? L"[private desktop]" : L"[fallback overlay]",
        sortName(g_ui.sort),
        snapshot ? L"" : L"no sample yet");
    drawLine(surface, y, line, kTitle);
    y += kRowHeight;

    drawLine(surface, y, L"PID     IMAGE                     CPU%    PRIV   HFLT/s  SFLT/s  THR R/W    FLAGS      NAME", kHeader);
    y += kRowHeight;

    if (snapshot) {
        refreshRows(*snapshot);

        int selected = selectedRow(*snapshot);
        if (selected < 0) {
            selectRow(*snapshot, 0);
            selected = selectedRow(*snapshot);
        }

        const uint32_t shown = rowsShown();
        if (selected >= 0 && g_ui.followSelection) {
            if (static_cast<uint32_t>(selected) < g_ui.scroll) g_ui.scroll = static_cast<uint32_t>(selected);
            if (shown && static_cast<uint32_t>(selected) >= g_ui.scroll + shown) g_ui.scroll = static_cast<uint32_t>(selected) - shown + 1;
        }
        if (g_ui.scroll + shown > g_ui.rowCount) g_ui.scroll = g_ui.rowCount > shown ? g_ui.rowCount - shown : 0;

        for (uint32_t i = 0; i < shown; ++i) {
            const uint32_t rowIndex = g_ui.scroll + i;
            RowView view;
            describeRow(*snapshot, rowIndex, view);
            if (view.kind == RowKind::Apps || view.kind == RowKind::Background) {
                drawLine(surface, y, view.kind == RowKind::Apps ? L"-- APPS --" : L"-- BACKGROUND PROCESSES --", kHeader);
                y += kRowHeight;
                continue;
            }
            const ProcRecord& record = view.record;

            wchar_t image[kImageChars + 12];
            if (view.kind == RowKind::Group) swprintf_s(image, L"%ls %ls (%u)", view.expanded ? L"-" : L"+", record.image, view.count);
            else if (view.member) swprintf_s(image, L"    %ls", record.image);
            else wcscpy_s(image, record.image);

            wchar_t priv[16];
            formatBytes(record.privateBytes, priv, 16);

            wchar_t flags[40] = L"";
            if (record.flags & kFlagHungWindow) wcscat_s(flags, L"HUNG ");
            else if (record.flags & kFlagHungByState) wcscat_s(flags, L"HUNG? ");
            if (record.flags & kFlagPaging) wcscat_s(flags, L"PAGING ");
            if (record.flags & kFlagSelf) wcscat_s(flags, L"SELF ");
            const bool denied = view.denied;
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
                view.kind == RowKind::Group ? 0u : record.pid, image, record.cpuCores * 100.0, priv,
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
        if (g_ui.confirmGroup[0]) swprintf_s(line, L"Kill %ls (%u processes) ?   Y = yes   N = no", g_ui.confirmGroup, g_ui.confirmCount);
        else swprintf_s(line, L"Kill %u %ls ?   Y = yes   N = no", g_ui.confirmPid, g_ui.confirmImage);
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
        swprintf_s(line, L"Up/Down select   Enter kill   C close   / filter   1-5 view   R refresh   PgDn all   Esc leave   %ls%ls",
            result[0] ? L"|  " : L"", result);
        drawLine(surface, statusY, line, kText);
    }

    if (snapshot) snapshotRelease(snapshot);
}

void renderModern(Surface& surface) {
    using namespace modern;
    const Snapshot* snapshot = snapshotAcquire();
    const SamplerStatus sampler = samplerStatus();
    const OverlayStatus status = overlayStatus();
    const PauseInfo paused = pausedInfo();
    const int c = cw();
    wchar_t line[320];

    fillRect(surface, 0, 0, kWidth, kHeight, kBg);

    // Title bar: name, tier badge, sort at the right. 2 px band divider below.
    {
        const int y = (kTitleBar - g_title.cellHeight) / 2;
        int x = drawText(surface, g_title, kPad, y, L"KeyRail Rescue", kInk, kBg);
        x += 14;
        const wchar_t* badge = g_ui.tier == Tier::Private ? L"PRIVATE DESKTOP" : L"FALLBACK OVERLAY";
        const int badgeWidth = textWidth(g_mono, badge) + c * 2;
        const int badgeTop = (kTitleBar - kRow) / 2;
        fillRect(surface, x, badgeTop + 2, badgeWidth, kRow - 4, kBg);
        fillRect(surface, x, badgeTop + 2, badgeWidth, 1, kAccent);
        fillRect(surface, x, badgeTop + kRow - 3, badgeWidth, 1, kAccent);
        fillRect(surface, x, badgeTop + 2, 1, kRow - 4, kAccent);
        fillRect(surface, x + badgeWidth - 1, badgeTop + 2, 1, kRow - 4, kAccent);
        drawText(surface, g_mono, x + c, badgeTop, badge, kAccent, kBg);

        if (snapshot) {
            swprintf_s(line, L"%u processes", snapshot->count);
            drawTextRight(surface, g_mono, kWidth - kPad, badgeTop, line, kDimInk, kBg);
        }
        fillRect(surface, 0, kTitleBar - 2, kWidth, 2, kSel);
    }

    int y = kTitleBar;
    if (paused.active) {
        fillRect(surface, 0, y, kWidth, kBanner, kAccent);
        const int ty = y + (kBanner - g_title.cellHeight) / 2;
        int x = drawText(surface, g_title, kPad, ty, L"PAUSED", kOnAccent, kAccent);
        swprintf_s(line, L"%u %ls: %ls \x2014 Enter kills it, Esc resumes it", paused.pid, paused.image, culpritName(paused.why));
        drawText(surface, g_mono, x + 18, y + (kBanner - kRow) / 2, line, kOnAccent, kAccent);
        y += kBanner;
    } else {
        // Chip bar. Chips are hit-tested for the mouse; keys 1-6 do the same.
        int x = kPad;
        const int chipTop = y + (kChipBar - kRow) / 2 + 2;
        const int chipHeight = kRow - 4;
        for (int i = 0; i < kChipCount; ++i) {
            const int width = textWidth(g_mono, g_chips[i].label) + c * 2;
            const bool active = viewIndex(g_ui.sort) == i;
            g_chips[i].rect = RECT{x, chipTop, x + width, chipTop + chipHeight};
            if (active) {
                fillRect(surface, x, chipTop, width, chipHeight, kAccent);
                drawText(surface, g_mono, x + c, chipTop - 2, g_chips[i].label, kOnAccent, kAccent);
            } else {
                fillRect(surface, x, chipTop, width, 1, kSel);
                fillRect(surface, x, chipTop + chipHeight - 1, width, 1, kSel);
                fillRect(surface, x, chipTop, 1, chipHeight, kSel);
                fillRect(surface, x + width - 1, chipTop, 1, chipHeight, kSel);
                drawText(surface, g_mono, x + c, chipTop - 2, g_chips[i].label, kDimInk, kBg);
            }
            x += width + 6;
        }
        int right = kWidth - kPad;
        if (g_ui.filterMode || g_ui.filterLen) {
            swprintf_s(line, L"/ %ls%ls   %u of %u", g_ui.filter, g_ui.filterMode ? L"_" : L"",
                g_ui.rowCount, snapshot ? snapshot->count : 0);
            right = drawTextRight(surface, g_mono, right, chipTop - 2, line, kInk, kBg);
        } else {
            right = drawTextRight(surface, g_mono, right, chipTop - 2, L"/ FILTER", kDimInk, kBg);
        }
        g_filterButton = RECT{right, chipTop, kWidth - kPad, chipTop + chipHeight};
        // Back-to-top button: same outline as an inactive chip, clickable.
        {
            const wchar_t* label = L"HOME TOP";
            const int width = textWidth(g_mono, label) + c * 2;
            const int bx = right - c * 3 - width;
            g_topButton = RECT{bx, chipTop, bx + width, chipTop + chipHeight};
            fillRect(surface, bx, chipTop, width, 1, kSel);
            fillRect(surface, bx, chipTop + chipHeight - 1, width, 1, kSel);
            fillRect(surface, bx, chipTop, 1, chipHeight, kSel);
            fillRect(surface, bx + width - 1, chipTop, 1, chipHeight, kSel);
            drawText(surface, g_mono, bx + c, chipTop - 2, label, kDimInk, kBg);
        }
        y += kChipBar;
    }

    // Column geometry in characters, per the spec.
    const int xPid = kPad;                         // 7, right-aligned
    const int xImage = xPid + c * 8;                // 22
    const int xCpu = xImage + c * 23;               // num 5 + bar 5
    const int xPriv = xCpu + c * 13;
    const int xHard = xPriv + c * 13;               // 6 right
    const int xSoft = xHard + c * 7;                // 7 right
    const int xDisk = xSoft + c * 8;                // 6 right
    const int xThr = xDisk + c * 7;                 // 8 right
    const int xFlags = xThr + c * 9;                // 14 left
    const int xName = xFlags + c * 15;              // rest
    const int maxX = kWidth - kPad;

    // Header band.
    fillRect(surface, 0, y, kWidth, kRow, kBand);
    drawTextRight(surface, g_mono, xPid + c * 7, y, L"PID", kDimInk, kBand);
    drawText(surface, g_mono, xImage, y, L"IMAGE", kDimInk, kBand);
    drawText(surface, g_mono, xCpu, y, L"CPU %", kDimInk, kBand);
    drawText(surface, g_mono, xPriv, y, L"PRIV MEM", kDimInk, kBand);
    drawTextRight(surface, g_mono, xHard + c * 6, y, L"HARD", kDimInk, kBand);
    drawTextRight(surface, g_mono, xSoft + c * 7, y, L"SOFT", kDimInk, kBand);
    drawTextRight(surface, g_mono, xDisk + c * 6, y, L"MB/S", kDimInk, kBand);
    drawTextRight(surface, g_mono, xThr + c * 8, y, L"THR R/W", kDimInk, kBand);
    drawText(surface, g_mono, xFlags, y, L"FLAGS", kDimInk, kBand);
    drawText(surface, g_mono, xName, y, L"NAME \x00b7 WINDOW TITLE", kDimInk, kBand);
    y += kRow;
    g_firstRowY = y;
    g_rowsDrawn = 0;
    g_whyShown = false;

    const int statusTop = kHeight - kStatus1 - kStatus2;
    if (snapshot) {
        refreshRows(*snapshot);
        int selected = selectedRow(*snapshot);
        if (selected < 0) {
            selectRow(*snapshot, 0);
            selected = selectedRow(*snapshot);
        }

        // Rows that fit: leave room for the WHY strip and the hint row.
        const int available = statusTop - y - kRow * 2;
        uint32_t fit = available > 0 ? static_cast<uint32_t>(available / kRow) : 0;
        uint32_t shown = g_ui.showAll ? fit : (std::min)(fit, static_cast<uint32_t>(g_rowLimit.load()));
        shown = (std::min)(shown, g_ui.rowCount);
        if (selected >= 0 && g_ui.followSelection) {
            if (static_cast<uint32_t>(selected) < g_ui.scroll) g_ui.scroll = static_cast<uint32_t>(selected);
            if (shown && static_cast<uint32_t>(selected) >= g_ui.scroll + shown) g_ui.scroll = static_cast<uint32_t>(selected) - shown + 1;
        }
        if (g_ui.scroll + shown > g_ui.rowCount) g_ui.scroll = g_ui.rowCount > shown ? g_ui.rowCount - shown : 0;

        for (uint32_t i = 0; i < shown; ++i) {
            const uint32_t rowIndex = g_ui.scroll + i;
            RowView view;
            describeRow(*snapshot, rowIndex, view);
            if (view.kind == RowKind::Apps || view.kind == RowKind::Background) {
                // Section divider: rule above, bright label with a red underline
                // and the section's count, so the split reads at a glance.
                const bool apps = view.kind == RowKind::Apps;
                fillRect(surface, 0, y, kWidth, kRow, kBand);
                fillRect(surface, 0, y, kWidth, 2, kSel);
                const wchar_t* label = apps ? L"APPS" : L"BACKGROUND PROCESSES";
                const int endX = drawText(surface, g_mono, kPad, y + 1, label, kInk, kBand);
                fillRect(surface, kPad, y + kRow - 4, endX - kPad, 2, kAccent);
                swprintf_s(line, L"%u", apps ? g_ui.appsCount : g_ui.backgroundCount);
                drawText(surface, g_mono, endX + c, y + 1, line, kDimInk, kBand);
                y += kRow;
                ++g_rowsDrawn;
                continue;
            }
            const ProcRecord& record = view.record;
            const bool isSelected = static_cast<int>(rowIndex) == selected;
            const bool denied = view.denied;
            const bool self = (record.flags & kFlagSelf) != 0;
            const COLORREF bg = isSelected ? kSel : kBg;
            COLORREF ink = kInk;
            if (denied) ink = kProtected;
            else if (self) ink = kDimInk;
            const COLORREF nameInk = isSelected ? ink : (denied ? kProtected : kDimInk);

            if (isSelected) {
                fillRect(surface, 0, y, kWidth, kRow, kSel);
                fillRect(surface, 0, y, kCursorBar, kRow, kAccent);
            }

            if (view.kind == RowKind::Group) {
                // Arrow, name, count: the name gives way so the count always fits.
                wchar_t count[16];
                swprintf_s(count, L" (%u)", view.count);
                const int nameChars = (std::max)(4, 22 - 2 - static_cast<int>(wcslen(count)));
                swprintf_s(line, L"%lc %.*ls", view.expanded ? L'\x25be' : L'\x25b8', nameChars, record.image);
                int nx = drawText(surface, g_mono, xImage, y, line, ink, bg, xImage + c * 22);
                drawText(surface, g_mono, nx, y, count, kDimInk, bg, xImage + c * 22);
            } else {
                swprintf_s(line, L"%u", record.pid);
                drawTextRight(surface, g_mono, xPid + c * 7, y, line, view.member ? kDimInk : ink, bg);
                // Members of an expanded group are indented two characters.
                drawText(surface, g_mono, xImage + (view.member ? c * 2 : 0), y, record.image, ink, bg, xImage + c * 22);
            }

            swprintf_s(line, L"%.1f", record.cpuCores * 100.0);
            drawTextRight(surface, g_mono, xCpu + c * 5, y, line, ink, bg);
            inlineBar(surface, xCpu + c * 6, y, record.cpuCores, denied ? kProtected : ink, kSel);

            formatBytes(record.privateBytes, line, 32);
            drawTextRight(surface, g_mono, xPriv + c * 5, y, line, ink, bg);
            const float memShare = snapshot->physicalBytes
                ? static_cast<float>(static_cast<double>(record.privateBytes) / static_cast<double>(snapshot->physicalBytes)) : 0.0f;
            const bool memWarn = (record.flags & kFlagPaging) && memShare > 0.6f;
            inlineBar(surface, xPriv + c * 6, y, memShare, denied ? kProtected : (memWarn ? kWarn : ink), kSel);

            formatCount(record.hardFaultsPerSec, line, 32);
            drawTextRight(surface, g_mono, xHard + c * 6, y, line, ink, bg);
            formatCount(record.softFaultsPerSec, line, 32);
            drawTextRight(surface, g_mono, xSoft + c * 7, y, line, ink, bg);
            swprintf_s(line, L"%.1f", record.ioBytesPerSec / 1048576.0f);
            drawTextRight(surface, g_mono, xDisk + c * 6, y, line, ink, bg);
            swprintf_s(line, L"%u/%u", record.runnable, record.waiting);
            drawTextRight(surface, g_mono, xThr + c * 8, y, line, ink, bg);

            // Flags, each in its own colour.
            {
                int fx = xFlags;
                const int flagMax = xFlags + c * 14;
                auto flag = [&](const wchar_t* text, COLORREF color) {
                    if (fx > xFlags) fx += c;
                    fx = drawText(surface, g_mono, fx, y, text, denied ? kProtected : color, bg, flagMax);
                };
                if (paused.active && paused.pid == record.pid) flag(L"PAUSED", kAccent);
                if (record.flags & kFlagHungWindow) flag(L"HUNG", kAccent);
                else if (record.flags & kFlagHungByState) flag(L"HUNG?", kAccent);
                if (record.flags & kFlagPaging) flag(L"PAGING", kWarn);
                if (denied) flag(L"PROTECTED", kProtected);
                if (record.flags & kFlagIoStorm) flag(L"IO", kWarn);
                if (record.flags & kFlagRealtime) flag(L"RT", kWarn);
                if (record.flags & kFlagSpawnStorm) flag(L"SPAWN", kAccent);
                if (self) flag(L"SELF", kDimInk);
                if (record.flags & kFlagNew) flag(L"NEW", kAccent);
            }

            wchar_t name[kFriendlyChars + kTitleChars + 4] = L"";
            wchar_t friendly[kFriendlyChars];
            if (metaLookup(record.pid, record.createTime, friendly)) wcscpy_s(name, friendly);
            if (record.title[0]) {
                if (name[0]) wcscat_s(name, L" \x2014 ");
                wcscat_s(name, record.title);
            }
            if ((record.flags & kFlagHungWindow) && !wcsstr(name, L"not responding")) {
                if (name[0]) wcscat_s(name, L" ");
                wcscat_s(name, L"(not responding)");
            }
            drawText(surface, g_mono, xName, y, name, nameInk, bg, maxX);
            y += kRow;
            ++g_rowsDrawn;

            const bool flagged = (record.flags & (kFlagHungWindow | kFlagHungByState | kFlagPaging | kFlagIoStorm | kFlagRealtime | kFlagSpawnStorm))
                || (paused.active && (paused.pid == record.pid || (view.kind == RowKind::Group && _wcsicmp(paused.image, record.image) == 0)));
            if (isSelected && flagged) {
                fillRect(surface, 0, y, kWidth, kRow, kBand);
                drawTextRight(surface, g_mono, xPid + c * 7, y, L"WHY", kAccent, kBand);
                swprintf_s(line, L"#%u", rowIndex);
                drawText(surface, g_mono, xPid + c * 8, y, line, kAccent, kBand);
                drawText(surface, g_mono, xImage + c * 3, y, L"\x2502", kSel, kBand);
                wchar_t reason[300];
                reasonFor(record, *snapshot, reason, 300);
                drawText(surface, g_mono, xImage + c * 5, y, reason, kInk, kBand, maxX);
                y += kRow;
                g_whyShown = true;
                g_whyRow = g_rowsDrawn - 1;
            }
        }

        if (g_ui.rowCount > shown) {
            swprintf_s(line, L"... %u more", g_ui.rowCount - shown);
            drawText(surface, g_mono, xPid, y, line, kDimInk, kBg);
            drawTextRight(surface, g_mono, maxX, y, g_ui.showAll ? L"PGUP/PGDN SCROLL" : L"PGDN SHOW ALL", kDimInk, kBg);
        }
    } else {
        drawText(surface, g_mono, xImage, y, L"no sample yet", kDimInk, kBg);
    }

    // Status line 1: timing and health, warnings at the right.
    int sy = statusTop;
    fillRect(surface, 0, sy, kWidth, kStatus1, kBand);
    fillRect(surface, 0, sy, kWidth, 2, kSel);
    swprintf_s(line, L"trigger\x2192pixel %u.%u ms \x00b7 sample %u.%u ms \x00b7 %u procs \x00b7 tick %u",
        status.lastTriggerToPixelMicros / 1000, (status.lastTriggerToPixelMicros / 100) % 10,
        sampler.lastTickMicros / 1000, (sampler.lastTickMicros / 100) % 10,
        snapshot ? snapshot->count : 0, snapshot ? snapshot->sequence : 0);
    drawText(surface, g_mono, kPad, sy, line, kDimInk, kBand);
    {
        int right = maxX;
        if (!sampler.memoryLocked) right = drawTextRight(surface, g_mono, right, sy, L"MEMORY NOT LOCKED", kWarn, kBand) - c * 2;
        if (sampler.bufferTooSmall) drawTextRight(surface, g_mono, right, sy, L"BUFFER TRUNCATED", kWarn, kBand);
    }
    sy += kStatus1;

    // Status line 2: exactly one of confirm / result / legend.
    fillRect(surface, 0, sy, kWidth, kStatus2, kBand);
    const int ly = sy + (kStatus2 - kRow) / 2;
    if (g_ui.confirm == 1) {
        if (g_ui.confirmGroup[0]) swprintf_s(line, L"KILL %ls (%u processes)?", g_ui.confirmGroup, g_ui.confirmCount);
        else swprintf_s(line, L"KILL %u %ls?", g_ui.confirmPid, g_ui.confirmImage);
        int x = drawText(surface, g_mono, kPad, ly, line, kAccent, kBand);
        x = drawText(surface, g_mono, x + c * 3, ly, L"Y", kInk, kBand);
        x = drawText(surface, g_mono, x, ly, L" = yes", kDimInk, kBand);
        x = drawText(surface, g_mono, x + c * 3, ly, L"N", kInk, kBand);
        drawText(surface, g_mono, x, ly, L" = no", kDimInk, kBand);
    } else {
        wchar_t result[kResultChars];
        AcquireSRWLockShared(&g_resultLock);
        wcscpy_s(result, g_result);
        ReleaseSRWLockShared(&g_resultLock);
        if (result[0]) {
            const bool failure = wcsstr(result, L"refused") || wcsstr(result, L"failed") || wcsstr(result, L"not terminable")
                || wcsstr(result, L"access denied") || wcsstr(result, L"still alive");
            drawText(surface, g_mono, kPad, ly, result, failure ? kAccent : kInk, kBand, maxX);
        } else {
            int x = kPad;
            auto item = [&](const wchar_t* key, const wchar_t* action) {
                x = drawText(surface, g_mono, x, ly, key, kInk, kBand);
                x = drawText(surface, g_mono, x + c / 2, ly, action, kDimInk, kBand);
                x += c * 2;
            };
            if (paused.active) {
                item(L"ENTER", L"kill paused process");
                item(L"ESC", L"resume it");
                item(L"\x2191\x2193", L"select");
                item(L"C", L"close politely");
            } else {
                item(L"\x2191\x2193", L"select");
                item(L"ENTER", L"kill");
                item(L"SPACE", L"expand");
                item(L"C", L"close politely");
                item(L"/", L"filter");
                item(L"1-5", L"view");
                item(L"HOME", L"top");
                item(L"R", L"refresh order");
                item(L"PGDN", L"show all");
                item(L"ESC", L"leave");
            }
        }
    }

    if (snapshot) snapshotRelease(snapshot);
}

void render(Surface& surface) {
    if (g_style.load() == 1 || !modern::g_mono.ok) renderLegacy(surface);
    else renderModern(surface);
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
                g_ui.selectedGroup[0] = 0;
                // Make sure the culprit is visible even inside a collapsed group.
                if (!isExpanded(culprit->image)) toggleExpanded(culprit->image);
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
    int row = selectedRow(*snapshot);
    selectRow(*snapshot, row < 0 ? 0 : row + delta, delta < 0 ? -1 : 1);
    g_ui.followSelection = true;
    snapshotRelease(snapshot);
    refresh(surface);
}

// Space / Right / Left / click on a group row.
void toggleSelectedGroup(Surface& surface, int wantExpanded) {
    if (!g_ui.selectedGroup[0]) return;
    const bool now = isExpanded(g_ui.selectedGroup);
    if (wantExpanded < 0 || (wantExpanded == 1) != now) toggleExpanded(g_ui.selectedGroup);
    refresh(surface);
}

// Group actions apply to every member; denied members are skipped and said so.
void forEachSelectedMember(const Snapshot& snapshot, void (*action)(uint32_t, int64_t)) {
    const int g = findGroup(g_ui.selectedGroup);
    if (g < 0) return;
    for (uint16_t m = g_groups[g].head; m != kNoRecord; m = g_next[m]) {
        const ProcRecord& record = snapshot.records[m];
        if (isDenied(record.pid, record.image)) continue;
        action(record.pid, record.createTime);
    }
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
    g_ui.confirmGroup[0] = 0;
    if (g_ui.selectedGroup[0]) {
        refreshRows(*snapshot);
        const int g = findGroup(g_ui.selectedGroup);
        if (g >= 0) {
            g_ui.confirm = 1;
            wcscpy_s(g_ui.confirmGroup, g_groups[g].image);
            g_ui.confirmCount = g_groups[g].count;
        }
        snapshotRelease(snapshot);
        refresh(surface);
        return;
    }
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
    if (g_ui.selectedGroup[0]) {
        refreshRows(*snapshot);
        forEachSelectedMember(*snapshot, requestClose);
    } else {
        const ProcRecord* record = findRecord(*snapshot, g_ui.selectedPid);
        if (record && record->createTime == g_ui.selectedCreateTime) requestClose(record->pid, record->createTime);
    }
    snapshotRelease(snapshot);
    refresh(surface);
}

bool handleKey(Surface& surface, WPARAM vk) {
    if (g_ui.confirm == 1) {
        if (vk == 'Y') {
            if (g_ui.confirmGroup[0]) {
                const Snapshot* snapshot = snapshotAcquire();
                if (snapshot) {
                    refreshRows(*snapshot);
                    forEachSelectedMember(*snapshot, requestKill);
                    snapshotRelease(snapshot);
                }
                g_ui.confirmGroup[0] = 0;
            } else {
                requestKill(g_ui.confirmPid, g_ui.confirmCreateTime);
            }
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
        g_ui.scroll = 0;
        moveSelection(surface, -static_cast<int>(kMaxRecords));
        return true;
    case VK_END:
        moveSelection(surface, static_cast<int>(kMaxRecords));
        return true;
    case VK_RETURN:
        beginConfirm(surface);
        return true;
    case VK_SPACE:
        toggleSelectedGroup(surface, -1);
        return true;
    case VK_RIGHT:
        toggleSelectedGroup(surface, 1);
        return true;
    case VK_LEFT:
        toggleSelectedGroup(surface, 0);
        return true;
    case 'C':
        closeSelected(surface);
        return true;
    case '1': case '2': case '3': case '4': case '5':
        resort(surface, viewFromIndex(static_cast<int>(vk - '1')));
        return true;
    case 'S':
        resort(surface, viewFromIndex((viewIndex(g_ui.sort) + 1) % kViewCount));
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
    case WM_MOUSEWHEEL:
        if (g_ui.surface == surface) {
            const int delta = GET_WHEEL_DELTA_WPARAM(wParam);
            const int step = 3 * (delta < 0 ? 1 : -1);
            g_ui.showAll = true;                 // wheeling past the top 15 means "show me more"
            g_ui.followSelection = false;        // do not snap back to the cursor while wheeling
            int scroll = static_cast<int>(g_ui.scroll) + step;
            if (scroll < 0) scroll = 0;
            g_ui.scroll = static_cast<uint32_t>(scroll);   // upper clamp happens in render
            refresh(*surface);
        }
        return 0;
    case WM_LBUTTONDOWN:
        if (g_ui.surface == surface && g_style.load() == 0) {
            const int mx = static_cast<int>(static_cast<short>(LOWORD(lParam)));
            const int my = static_cast<int>(static_cast<short>(HIWORD(lParam)));
            {
                const RECT& r = modern::g_filterButton;
                if (mx >= r.left && mx < r.right && my >= r.top && my < r.bottom) {
                    g_ui.filterMode = true;
                    refresh(*surface);
                    return 0;
                }
            }
            {
                const RECT& r = modern::g_topButton;
                if (mx >= r.left && mx < r.right && my >= r.top && my < r.bottom) {
                    g_ui.scroll = 0;
                    const Snapshot* snapshot = snapshotAcquire();
                    if (snapshot) {
                        refreshRows(*snapshot);
                        selectRow(*snapshot, 0);
                        snapshotRelease(snapshot);
                    }
                    refresh(*surface);
                    return 0;
                }
            }
            for (int i = 0; i < modern::kChipCount; ++i) {
                const RECT& r = modern::g_chips[i].rect;
                if (mx >= r.left && mx < r.right && my >= r.top && my < r.bottom) {
                    resort(*surface, viewFromIndex(i));
                    return 0;
                }
            }
            if (my >= modern::g_firstRowY && g_ui.rowCount) {
                // Rows are kRow tall; the WHY strip under the selection adds one.
                int row = (my - modern::g_firstRowY) / modern::kRow;
                const int selected = selectedRow();   // group selection only; process rows need the snapshot
                (void)selected;
                if (modern::g_whyShown && row > modern::g_whyRow) --row;
                if (row >= 0 && row < modern::g_rowsDrawn) {
                    const Snapshot* snapshot = snapshotAcquire();
                    if (snapshot) {
                        refreshRows(*snapshot);
                        const uint32_t index = g_ui.scroll + static_cast<uint32_t>(row);
                        if (index < g_ui.rowCount && !isSectionRow(index)) {
                            selectRow(*snapshot, static_cast<int>(index));
                            if (g_ui.rows[index].kind == RowKind::Group) toggleExpanded(g_groups[g_ui.rows[index].group].image);
                        }
                        snapshotRelease(snapshot);
                    }
                    refresh(*surface);
                }
            }
        }
        return 0;
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

    // Modern style fonts: the table in a 13 px monospace (Cascadia Mono ships
    // with Windows 11; Consolas everywhere), the title in Segoe UI Semibold.
    // Both are rendered once into locked atlases now; nothing at show time.
    if (!modern::g_mono.ok) {
        HFONT mono = CreateFontW(-13, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_TT_PRECIS,
            CLIP_DEFAULT_PRECIS, ANTIALIASED_QUALITY, FIXED_PITCH | FF_MODERN, L"Cascadia Mono");
        if (mono) {
            HDC probe = CreateCompatibleDC(nullptr);
            HGDIOBJ old = SelectObject(probe, mono);
            wchar_t face[64] = L"";
            GetTextFaceW(probe, 64, face);
            SelectObject(probe, old);
            DeleteDC(probe);
            if (_wcsicmp(face, L"Cascadia Mono") != 0) {
                DeleteObject(mono);
                mono = CreateFontW(-13, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_TT_PRECIS,
                    CLIP_DEFAULT_PRECIS, ANTIALIASED_QUALITY, FIXED_PITCH | FF_MODERN, L"Consolas");
            }
        }
        HFONT title = CreateFontW(-20, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_TT_PRECIS,
            CLIP_DEFAULT_PRECIS, ANTIALIASED_QUALITY, VARIABLE_PITCH | FF_SWISS, L"Segoe UI");
        if (mono) modern::buildAtlas(modern::g_mono, mono, modern::kRow);
        if (title) modern::buildAtlas(modern::g_title, title, 28);
        if (mono) DeleteObject(mono);
        if (title) DeleteObject(title);
        log(L"rescue modern style: table font %ls, title font %ls",
            modern::g_mono.ok ? L"ready" : L"MISSING (legacy look)", modern::g_title.ok ? L"ready" : L"missing");
    }

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
    g_style.store(settings.style == L"legacy" ? 1 : 0);
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
    g_style.store(settings.style == L"legacy" ? 1 : 0);
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

bool overlayDumpBitmap(const wchar_t* path) {
    Surface& surface = g_private.ok ? g_private : g_fallback;
    if (!surface.ok || !surface.bits) return false;
    render(surface);

    BITMAPFILEHEADER fileHeader{};
    BITMAPINFOHEADER infoHeader{};
    const DWORD imageBytes = static_cast<DWORD>(kWidth) * kHeight * 4;
    fileHeader.bfType = 0x4D42;
    fileHeader.bfOffBits = sizeof(fileHeader) + sizeof(infoHeader);
    fileHeader.bfSize = fileHeader.bfOffBits + imageBytes;
    infoHeader.biSize = sizeof(infoHeader);
    infoHeader.biWidth = kWidth;
    infoHeader.biHeight = -kHeight;
    infoHeader.biPlanes = 1;
    infoHeader.biBitCount = 32;
    infoHeader.biCompression = BI_RGB;
    infoHeader.biSizeImage = imageBytes;

    HANDLE file = CreateFileW(path, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    DWORD written = 0;
    WriteFile(file, &fileHeader, sizeof(fileHeader), &written, nullptr);
    WriteFile(file, &infoHeader, sizeof(infoHeader), &written, nullptr);
    WriteFile(file, surface.bits, imageBytes, &written, nullptr);
    CloseHandle(file);
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
