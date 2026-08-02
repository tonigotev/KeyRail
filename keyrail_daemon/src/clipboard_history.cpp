#include "clipboard_history.h"

#include "display_state.h"
#include "synthetic_hotkey_suppression.h"

#include <windows.h>
#include <windowsx.h>

// gdiplus.h uses unqualified min/max; the project builds with NOMINMAX.
#include <algorithm>
using std::min;
using std::max;
#include <objidl.h>
#include <gdiplus.h>

#include <nlohmann/json.hpp>

#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

using nlohmann::json;

namespace {

constexpr UINT WM_CLIP_HISTORY_SHOW = WM_APP + 301;
constexpr UINT WM_CLIP_HISTORY_HIDE = WM_APP + 302;
constexpr UINT_PTR kPickerTimer = 12;
constexpr UINT_PTR kScrollTimer = 13;
constexpr UINT_PTR kHoverTimer = 14;
constexpr UINT_PTR kTopmostPulseTimer = 15;
constexpr size_t kMaxItems = 40;
constexpr DWORD kPickerIdleMs = 30000;
constexpr DWORD kPickerOpenMs = 12000;

// Shared chrome geometry.
constexpr int kHeaderH = 48;
constexpr int kFooterH = 40;
constexpr int kSidePad = 20;

// Mouse variant: horizontal cards.
constexpr int kCardW = 150;
constexpr int kCardH = 144;
constexpr int kCardGap = 12;
constexpr int kCardPitch = kCardW + kCardGap;
constexpr int kMouseWinW = 704;

// Keyboard variant: vertical list rows.
constexpr int kKbWinW = 448;
constexpr int kRowH = 36;
constexpr size_t kKbVisibleRows = 9;

enum class ClipKind {
    Text,
    Image,
};

struct ClipItem {
    unsigned long long id = 0;
    ClipKind kind = ClipKind::Text;
    std::wstring text;
    std::vector<unsigned char> dib;
    int width = 0;
    int height = 0;
    long long capturedAt = 0;   // unix seconds
    std::wstring sourceApp;     // friendly name of the app it was copied from
};

// Accent + surface palette (shared by both variants).
constexpr COLORREF kClipBg      = RGB(12, 17, 26);
constexpr COLORREF kClipCard    = RGB(24, 31, 43);
constexpr COLORREF kClipCardBrd = RGB(50, 62, 82);
constexpr COLORREF kClipAccent  = RGB(96, 160, 255);
constexpr COLORREF kClipText    = RGB(232, 238, 246);
constexpr COLORREF kClipDim     = RGB(140, 150, 166);
constexpr COLORREF kClipLink    = RGB(122, 170, 255);
constexpr COLORREF kClipTag     = RGB(150, 160, 178);
constexpr COLORREF kClipImage   = RGB(45, 75, 118);

std::mutex g_mutex;
std::vector<ClipItem> g_items;
size_t g_selected = 0;
bool g_pickerOpen = false;
unsigned long long g_nextId = 1;
bool g_internalWrite = false;

// Mouse-driven quick-paste variant state.
bool g_mouseMode = false;         // true when opened as the clickable variant
int g_hoverIndex = -1;            // card under the cursor, or -1
HWND g_pasteTarget = nullptr;     // window to paste into after a click
bool g_mouseTracking = false;     // WM_MOUSELEAVE tracking armed
float g_scrollX = 0.0f;           // horizontal scroll offset (mouse cards)
float g_scrollTargetX = 0.0f;     // animated destination for horizontal scroll
float g_hoverAmount[kMaxItems]{}; // 0..1 hover transition per visible history slot

ULONG_PTR g_gdiplusToken = 0;

void ensureGdiplus() {
    if (g_gdiplusToken) return;
    Gdiplus::GdiplusStartupInput input;
    Gdiplus::GdiplusStartup(&g_gdiplusToken, &input, nullptr);
}

void keepPickerAlive(HWND hwnd, DWORD milliseconds = kPickerIdleMs) {
    if (!hwnd) return;
    SetTimer(hwnd, kPickerTimer, milliseconds, nullptr);
}

std::thread g_thread;
DWORD g_threadId = 0;
HWND g_window = nullptr;
HANDLE g_ready = nullptr;

std::string wideToUtf8(const std::wstring& value) {
    if (value.empty()) return {};
    int size = WideCharToMultiByte(CP_UTF8, 0, value.c_str(), -1, nullptr, 0, nullptr, nullptr);
    if (size <= 0) return {};
    std::string out(size, '\0');
    WideCharToMultiByte(CP_UTF8, 0, value.c_str(), -1, out.data(), size, nullptr, nullptr);
    out.pop_back();
    return out;
}

std::wstring utf8ToWide(const std::string& value) {
    if (value.empty()) return {};
    int size = MultiByteToWideChar(CP_UTF8, 0, value.c_str(), static_cast<int>(value.size()), nullptr, 0);
    if (size <= 0) return {};
    std::wstring out(size, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, value.c_str(), static_cast<int>(value.size()), out.data(), size);
    return out;
}

long long nowSec() {
    return static_cast<long long>(time(nullptr));
}

// Friendly product name (e.g. "Google Chrome") from an exe's version resource.
std::wstring fileDescription(const std::wstring& path) {
    DWORD handle = 0;
    DWORD size = GetFileVersionInfoSizeW(path.c_str(), &handle);
    if (size == 0) return L"";
    std::vector<unsigned char> buf(size);
    if (!GetFileVersionInfoW(path.c_str(), handle, size, buf.data())) return L"";

    struct LangCode { WORD lang; WORD code; };
    LangCode* langs = nullptr;
    UINT langBytes = 0;
    if (!VerQueryValueW(buf.data(), L"\\VarFileInfo\\Translation", reinterpret_cast<void**>(&langs), &langBytes)
        || langBytes < sizeof(LangCode)) {
        return L"";
    }
    wchar_t sub[80];
    swprintf(sub, 80, L"\\StringFileInfo\\%04x%04x\\FileDescription", langs[0].lang, langs[0].code);
    wchar_t* desc = nullptr;
    UINT descLen = 0;
    if (VerQueryValueW(buf.data(), sub, reinterpret_cast<void**>(&desc), &descLen) && descLen > 1) {
        return std::wstring(desc, descLen - 1);
    }
    return L"";
}

// The app that currently has focus â€” i.e. the source of a just-copied item.
std::wstring foregroundAppName() {
    HWND hwnd = GetForegroundWindow();
    if (!hwnd) return L"";
    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    if (!pid) return L"";
    HANDLE proc = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!proc) return L"";

    wchar_t path[MAX_PATH]{};
    DWORD sz = MAX_PATH;
    std::wstring name;
    if (QueryFullProcessImageNameW(proc, 0, path, &sz)) {
        name = fileDescription(path);
        if (name.empty()) {
            std::wstring p(path);
            size_t slash = p.find_last_of(L"\\/");
            std::wstring base = slash == std::wstring::npos ? p : p.substr(slash + 1);
            size_t dot = base.find_last_of(L'.');
            name = dot == std::wstring::npos ? base : base.substr(0, dot);
        }
    }
    CloseHandle(proc);
    if (name.size() > 28) name = name.substr(0, 27) + L"â€¦";
    return name;
}

bool isLinkText(const std::wstring& t) {
    return t.rfind(L"http://", 0) == 0 || t.rfind(L"https://", 0) == 0;
}

std::wstring relativeTime(long long capturedAt) {
    if (capturedAt <= 0) return L"";
    long long d = nowSec() - capturedAt;
    if (d < 5) return L"now";
    if (d < 60) return std::to_wstring(d) + L"s";
    if (d < 3600) return std::to_wstring(d / 60) + L"m";
    if (d < 86400) return std::to_wstring(d / 3600) + L"h";
    return std::to_wstring(d / 86400) + L"d";
}

const wchar_t* itemTag(const ClipItem& item) {
    if (item.kind == ClipKind::Image) return L"IMG";
    if (isLinkText(item.text)) return L"LINK";
    return L"TXT";
}

std::wstring itemSubtitle(const ClipItem& item) {
    std::wstringstream out;
    if (!item.sourceApp.empty()) out << item.sourceApp;
    const bool hasApp = !item.sourceApp.empty();
    if (item.kind == ClipKind::Image) {
        if (item.width > 0 && item.height > 0) {
            if (hasApp) out << L" Â· ";
            out << item.width << L" Ã— " << item.height;
        }
    } else if (isLinkText(item.text)) {
        if (hasApp) out << L" Â· link";
    } else {
        if (hasApp) out << L" Â· ";
        out << item.text.size() << L" characters";
    }
    return out.str();
}

std::wstring appDataPath() {
    wchar_t appdata[MAX_PATH]{};
    DWORD size = GetEnvironmentVariableW(L"APPDATA", appdata, MAX_PATH);
    if (size == 0 || size >= MAX_PATH) return L"clipboard_history.json";
    return std::wstring(appdata) + L"\\KeyRail\\clipboard_history.json";
}

std::wstring historyDir() {
    std::wstring path = appDataPath();
    size_t slash = path.find_last_of(L"\\/");
    return slash == std::wstring::npos ? L"." : path.substr(0, slash);
}

void saveTextHistory() {
    CreateDirectoryW(historyDir().c_str(), nullptr);
    json root = json::array();
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        for (const auto& item : g_items) {
            if (item.kind != ClipKind::Text) continue;
            root.push_back({
                {"type", "text"},
                {"text", wideToUtf8(item.text)},
                {"capturedAt", item.capturedAt},
                {"sourceApp", wideToUtf8(item.sourceApp)}
            });
        }
    }

    std::string text = root.dump(2);
    std::wstring path = appDataPath();
    HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return;
    DWORD written = 0;
    WriteFile(file, text.data(), static_cast<DWORD>(text.size()), &written, nullptr);
    CloseHandle(file);
}

void loadTextHistory() {
    std::wstring path = appDataPath();
    HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return;

    DWORD size = GetFileSize(file, nullptr);
    if (size == INVALID_FILE_SIZE || size == 0 || size > 1024 * 1024) {
        CloseHandle(file);
        return;
    }

    std::string text(size, '\0');
    DWORD read = 0;
    BOOL ok = ReadFile(file, text.data(), size, &read, nullptr);
    CloseHandle(file);
    if (!ok) return;
    text.resize(read);

    try {
        json root = json::parse(text);
        if (!root.is_array()) return;
        std::lock_guard<std::mutex> lock(g_mutex);
        for (const auto& entry : root) {
            if (!entry.is_object() || entry.value("type", "") != "text") continue;
            std::wstring value = utf8ToWide(entry.value("text", ""));
            if (value.empty()) continue;
            ClipItem item;
            item.id = g_nextId++;
            item.kind = ClipKind::Text;
            item.text = std::move(value);
            item.capturedAt = entry.value("capturedAt", 0LL);
            item.sourceApp = utf8ToWide(entry.value("sourceApp", ""));
            g_items.push_back(std::move(item));
            if (g_items.size() >= kMaxItems) break;
        }
    } catch (...) {
    }
}

std::wstring singleLine(std::wstring text) {
    for (wchar_t& ch : text) {
        if (ch == L'\r' || ch == L'\n' || ch == L'\t') ch = L' ';
    }
    while (text.find(L"  ") != std::wstring::npos) {
        text.replace(text.find(L"  "), 2, L" ");
    }
    if (text.size() > 72) text = text.substr(0, 69) + L"...";
    return text.empty() ? L"(empty text)" : text;
}

std::wstring itemTitle(const ClipItem& item) {
    if (item.kind == ClipKind::Image) {
        std::wstringstream out;
        out << L"Image";
        if (item.width > 0 && item.height > 0) out << L" " << item.width << L"x" << item.height;
        return out.str();
    }
    return singleLine(item.text);
}

void addItem(ClipItem item) {
    bool changed = false;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        if (item.kind == ClipKind::Text) {
            auto existing = std::find_if(g_items.begin(), g_items.end(), [&](const ClipItem& other) {
                return other.kind == ClipKind::Text && other.text == item.text;
            });
            if (existing != g_items.end()) g_items.erase(existing);
        } else {
            auto existing = std::find_if(g_items.begin(), g_items.end(), [&](const ClipItem& other) {
                return other.kind == ClipKind::Image
                    && other.width == item.width
                    && other.height == item.height
                    && other.dib == item.dib;
            });
            if (existing != g_items.end()) g_items.erase(existing);
        }

        item.id = g_nextId++;
        g_items.insert(g_items.begin(), std::move(item));
        if (g_items.size() > kMaxItems) g_items.resize(kMaxItems);
        g_selected = 0;
        changed = true;
    }

    if (changed) saveTextHistory();
}

bool captureText() {
    HANDLE handle = GetClipboardData(CF_UNICODETEXT);
    if (!handle) return false;
    auto* data = static_cast<const wchar_t*>(GlobalLock(handle));
    if (!data) return false;
    std::wstring text(data);
    GlobalUnlock(handle);
    if (text.empty()) return false;

    ClipItem item;
    item.kind = ClipKind::Text;
    item.text = std::move(text);
    item.capturedAt = nowSec();
    item.sourceApp = foregroundAppName();
    addItem(std::move(item));
    return true;
}

bool captureImage() {
    HANDLE handle = GetClipboardData(CF_DIB);
    if (!handle) return false;
    SIZE_T size = GlobalSize(handle);
    if (size == 0 || size > 20 * 1024 * 1024) return false;
    auto* data = static_cast<const unsigned char*>(GlobalLock(handle));
    if (!data) return false;

    ClipItem item;
    item.kind = ClipKind::Image;
    item.dib.assign(data, data + size);
    if (size >= sizeof(BITMAPINFOHEADER)) {
        auto* header = reinterpret_cast<const BITMAPINFOHEADER*>(data);
        item.width = static_cast<int>(header->biWidth);
        item.height = static_cast<int>(std::abs(header->biHeight));
    }
    GlobalUnlock(handle);
    item.capturedAt = nowSec();
    item.sourceApp = foregroundAppName();
    addItem(std::move(item));
    return true;
}

void captureClipboardNow() {
    if (g_internalWrite) {
        g_internalWrite = false;
        return;
    }

    if (!OpenClipboard(nullptr)) return;
    bool captured = false;
    if (IsClipboardFormatAvailable(CF_UNICODETEXT)) captured = captureText();
    if (!captured && IsClipboardFormatAvailable(CF_DIB)) captureImage();
    CloseClipboard();
}

Gdiplus::Color gpc(COLORREF c, BYTE a = 255) {
    return Gdiplus::Color(a, GetRValue(c), GetGValue(c), GetBValue(c));
}

// Antialiased rounded rectangle. border == (COLORREF)-1 skips the stroke.
void gpRoundRect(HDC dc, float x, float y, float w, float h, float r,
                 COLORREF fill, COLORREF border = static_cast<COLORREF>(-1),
                 BYTE fillAlpha = 255, float borderW = 1.0f) {
    Gdiplus::Graphics g(dc);
    g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
    g.SetPixelOffsetMode(Gdiplus::PixelOffsetModeHalf);
    const float d = r * 2.0f;
    Gdiplus::GraphicsPath path;
    if (r <= 0.0f) {
        path.AddRectangle(Gdiplus::RectF(x, y, w, h));
    } else {
        path.AddArc(x, y, d, d, 180, 90);
        path.AddArc(x + w - d, y, d, d, 270, 90);
        path.AddArc(x + w - d, y + h - d, d, d, 0, 90);
        path.AddArc(x, y + h - d, d, d, 90, 90);
        path.CloseFigure();
    }
    Gdiplus::SolidBrush brush(gpc(fill, fillAlpha));
    g.FillPath(&brush, &path);
    if (border != static_cast<COLORREF>(-1)) {
        Gdiplus::Pen pen(gpc(border), borderW);
        g.DrawPath(&pen, &path);
    }
}

void drawText(HDC dc, const std::wstring& text, RECT rect, COLORREF color, int size, int weight, UINT flags) {
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, color);
    HFONT font = CreateFontW(size, 0, 0, 0, weight, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
        OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_SWISS, L"Segoe UI");
    HFONT old = static_cast<HFONT>(SelectObject(dc, font));
    DrawTextW(dc, text.c_str(), -1, &rect, flags);
    SelectObject(dc, old);
    DeleteObject(font);
}

int textWidth(HDC dc, const std::wstring& text, int size, int weight) {
    HFONT font = CreateFontW(size, 0, 0, 0, weight, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
        OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_SWISS, L"Segoe UI");
    HFONT old = static_cast<HFONT>(SelectObject(dc, font));
    SIZE sz{};
    GetTextExtentPoint32W(dc, text.c_str(), static_cast<int>(text.size()), &sz);
    SelectObject(dc, old);
    DeleteObject(font);
    return sz.cx;
}

// Small type tag ("TXT"/"LINK"/"IMG"). Returns the pill's right edge.
int drawTagPill(HDC dc, int x, int y, const std::wstring& label) {
    const int w = 14 + textWidth(dc, label, 12, FW_BOLD);
    gpRoundRect(dc, static_cast<float>(x), static_cast<float>(y), static_cast<float>(w), 18.0f, 4.0f,
                RGB(42, 48, 60), RGB(58, 66, 82));
    RECT r{x, y, x + w, y + 18};
    drawText(dc, label, r, kClipTag, 12, FW_BOLD, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    return x + w;
}

// A raised keycap for footer hints. Returns right edge.
int drawKeycap(HDC dc, int x, int y, const std::wstring& label) {
    const int w = (std::max)(20, 12 + textWidth(dc, label, 11, FW_SEMIBOLD));
    gpRoundRect(dc, static_cast<float>(x), static_cast<float>(y) + 1.0f, static_cast<float>(w), 18.0f, 4.0f,
                RGB(46, 52, 66), RGB(64, 72, 90));
    RECT r{x, y, x + w, y + 20};
    drawText(dc, label, r, RGB(200, 208, 224), 11, FW_SEMIBOLD, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    return x + w;
}

void drawClipboardGlyph(HDC dc, int x, int y, COLORREF color) {
    // Body + clip tab.
    gpRoundRect(dc, static_cast<float>(x) + 1.5f, static_cast<float>(y) + 2.0f, 15.0f, 18.0f, 3.0f,
                kClipBg, color, 255, 1.6f);
    gpRoundRect(dc, static_cast<float>(x) + 5.0f, static_cast<float>(y), 8.0f, 5.0f, 1.5f, color);
}

// Draw the DIB thumbnail, aspect-fill, clipped to a rounded rect.
void drawThumb(HDC dc, const RECT& area, const ClipItem& item) {
    if (item.dib.size() < sizeof(BITMAPINFOHEADER) || item.width <= 0 || item.height <= 0) return;
    const BITMAPINFO* bi = reinterpret_cast<const BITMAPINFO*>(item.dib.data());
    const DWORD hdr = bi->bmiHeader.biSize;
    DWORD extra = 0;
    if (bi->bmiHeader.biBitCount <= 8) {
        DWORD colors = bi->bmiHeader.biClrUsed ? bi->bmiHeader.biClrUsed : (1u << bi->bmiHeader.biBitCount);
        extra = colors * sizeof(RGBQUAD);
    } else if (bi->bmiHeader.biCompression == BI_BITFIELDS) {
        extra = 3 * sizeof(DWORD);
    }
    if (item.dib.size() < hdr + extra) return;
    const void* bits = item.dib.data() + hdr + extra;

    const int aw = area.right - area.left;
    const int ah = area.bottom - area.top;
    const float scale = (std::max)(static_cast<float>(aw) / item.width, static_cast<float>(ah) / item.height);
    const int dw = (std::max)(1, static_cast<int>(item.width * scale));
    const int dh = (std::max)(1, static_cast<int>(item.height * scale));
    const int dx = area.left + (aw - dw) / 2;
    const int dy = area.top + (ah - dh) / 2;

    HRGN clip = CreateRoundRectRgn(area.left, area.top, area.right + 1, area.bottom + 1, 9, 9);
    SelectClipRgn(dc, clip);
    SetStretchBltMode(dc, HALFTONE);
    SetBrushOrgEx(dc, 0, 0, nullptr);
    StretchDIBits(dc, dx, dy, dw, dh, 0, 0, item.width, item.height, bits, bi, DIB_RGB_COLORS, SRCCOPY);
    SelectClipRgn(dc, nullptr);
    DeleteObject(clip);
}

// ---- geometry ---------------------------------------------------------------

int mouseViewportWidth() { return kMouseWinW - 2 * kSidePad; }
int mouseContentWidth(int count) { return count > 0 ? count * kCardPitch - kCardGap : 0; }
int maxScrollX(int count) { return (std::max)(0, mouseContentWidth(count) - mouseViewportWidth()); }

float clampScrollX(float value, int count) {
    return (std::max)(0.0f, (std::min)(value, static_cast<float>(maxScrollX(count))));
}

RECT cardRect(int index) {
    const int x = kSidePad + index * kCardPitch - static_cast<int>(std::lround(g_scrollX));
    const int y = kHeaderH + 6;
    return RECT{x, y, x + kCardW, y + kCardH};
}

int cardHitTest(int x, int y, int count) {
    for (int i = 0; i < count; ++i) {
        RECT r = cardRect(i);
        if (r.right <= kSidePad || r.left >= kMouseWinW - kSidePad) continue; // off-screen
        if (x >= r.left && x < r.right && y >= r.top && y < r.bottom) return i;
    }
    return -1;
}

int kbFirstVisible(int count, int selected) {
    if (count <= static_cast<int>(kKbVisibleRows)) return 0;
    int first = selected - static_cast<int>(kKbVisibleRows) / 2;
    return (std::max)(0, (std::min)(first, count - static_cast<int>(kKbVisibleRows)));
}

RECT kbRowRect(int visibleIndex) {
    const int y = kHeaderH + visibleIndex * kRowH;
    return RECT{kSidePad, y, kKbWinW - kSidePad, y + kRowH};
}

// index of the visible row under (x, y), maps to item index via first
int kbHitTest(int x, int y, int visibleRows) {
    for (int i = 0; i < visibleRows; ++i) {
        RECT r = kbRowRect(i);
        if (x >= r.left && x < r.right && y >= r.top && y < r.bottom) return i;
    }
    return -1;
}

int pickerWidth(bool mouseMode) { return mouseMode ? kMouseWinW : kKbWinW; }

int pickerHeight(bool mouseMode, size_t itemCount) {
    if (mouseMode) return kHeaderH + 6 + kCardH + 14 + kFooterH;
    const int rows = itemCount == 0
        ? static_cast<int>(kKbVisibleRows)
        : (std::max)(1, static_cast<int>((std::min)(itemCount, kKbVisibleRows)));
    return kHeaderH + rows * kRowH + kFooterH;
}

RECT overlayBoundsForForeground() {
    HWND foreground = GetForegroundWindow();
    HMONITOR monitor = foreground ? MonitorFromWindow(foreground, MONITOR_DEFAULTTONEAREST) : nullptr;
    if (!monitor) {
        POINT cursor{};
        GetCursorPos(&cursor);
        monitor = MonitorFromPoint(cursor, MONITOR_DEFAULTTONEAREST);
    }

    MONITORINFO info{};
    info.cbSize = sizeof(info);
    if (!monitor || !GetMonitorInfoW(monitor, &info)) {
        return RECT{
            GetSystemMetrics(SM_XVIRTUALSCREEN),
            GetSystemMetrics(SM_YVIRTUALSCREEN),
            GetSystemMetrics(SM_XVIRTUALSCREEN) + GetSystemMetrics(SM_CXVIRTUALSCREEN),
            GetSystemMetrics(SM_YVIRTUALSCREEN) + GetSystemMetrics(SM_CYVIRTUALSCREEN)
        };
    }

    if (foreground) {
        RECT foregroundRect{};
        if (GetWindowRect(foreground, &foregroundRect)) {
            const bool coversMonitor =
                foregroundRect.left <= info.rcMonitor.left + 2
                && foregroundRect.top <= info.rcMonitor.top + 2
                && foregroundRect.right >= info.rcMonitor.right - 2
                && foregroundRect.bottom >= info.rcMonitor.bottom - 2;
            if (coversMonitor) return info.rcMonitor;
        }
    }
    return info.rcWork;
}

POINT pickerPositionForSize(int width, int height) {
    RECT bounds = overlayBoundsForForeground();
    int x = bounds.right - width - 30;
    int y = bounds.top + 72;
    if (x < bounds.left + 8) x = bounds.left + 8;
    if (y + height > bounds.bottom - 8) y = (std::max)(bounds.top + 8, bounds.bottom - height - 8);
    return POINT{x, y};
}

// No BringWindowToTop or SetForegroundWindow: activating the picker is what
// knocks a fullscreen game out of exclusive mode and minimizes it. SWP_NOACTIVATE
// changes z-order only, leaving the foreground window alone.
void showPickerTopmost(HWND hwnd, int x, int y, int width, int height) {
    constexpr UINT flags = SWP_NOACTIVATE | SWP_SHOWWINDOW | SWP_NOOWNERZORDER | SWP_NOSENDCHANGING;
    SetWindowPos(hwnd, HWND_TOPMOST, x, y, width, height, flags);
    ShowWindow(hwnd, SW_SHOWNOACTIVATE);
}

void pulsePickerTopmost(HWND hwnd) {
    if (!IsWindowVisible(hwnd)) return;
    SetWindowPos(hwnd, HWND_TOPMOST, 0, 0, 0, 0,
        SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOOWNERZORDER | SWP_NOSENDCHANGING);
}

// ---- shared chrome ----------------------------------------------------------

void paintHeader(HDC dc, int width, size_t itemCount) {
    drawClipboardGlyph(dc, kSidePad, 15, RGB(150, 190, 255));
    RECT title{kSidePad + 24, 12, width - 120, 38};
    drawText(dc, L"Clipboard", title, kClipText, 18, FW_BOLD, DT_LEFT | DT_VCENTER | DT_SINGLELINE);

    std::wstring count = std::to_wstring(itemCount) + (itemCount == 1 ? L" item" : L" items");
    RECT countRect{width - 140, 12, width - kSidePad, 38};
    drawText(dc, count, countRect, kClipDim, 13, FW_NORMAL, DT_RIGHT | DT_VCENTER | DT_SINGLELINE);
}

void paintFooter(HDC dc, int width, int height, bool mouseMode) {
    RECT line{kSidePad, height - kFooterH, width - kSidePad, height - kFooterH + 1};
    HBRUSH b = CreateSolidBrush(RGB(38, 44, 56));
    FillRect(dc, &line, b);
    DeleteObject(b);

    const int y = height - 28;
    int x = kSidePad;
    auto label = [&](const std::wstring& t, int w) {
        RECT r{x, y - 1, x + w, y + 19};
        drawText(dc, t, r, kClipDim, 12, FW_NORMAL, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
        x += w;
    };

    if (mouseMode) {
        label(L"Click a card to paste", textWidth(dc, L"Click a card to paste", 12, FW_NORMAL) + 6);
        label(L"Â·  Scroll for more   ", textWidth(dc, L"Â·  Scroll for more   ", 12, FW_NORMAL) + 6);
    } else {
        x = drawKeycap(dc, x, y, L"â†‘") + 3;
        x = drawKeycap(dc, x, y, L"â†“") + 8;
        label(L"move   ", textWidth(dc, L"move   ", 12, FW_NORMAL));
        x = drawKeycap(dc, x, y, L"Enter") + 8;
        label(L"paste   ", textWidth(dc, L"paste   ", 12, FW_NORMAL));
    }
    x = drawKeycap(dc, x, y, L"Esc") + 8;
    label(L"close", 60);
}

void paintFooterClean(HDC dc, int width, int height, bool mouseMode) {
    RECT line{kSidePad, height - kFooterH, width - kSidePad, height - kFooterH + 1};
    HBRUSH b = CreateSolidBrush(RGB(38, 44, 56));
    FillRect(dc, &line, b);
    DeleteObject(b);

    const int y = height - 28;
    int x = kSidePad;
    auto label = [&](const std::wstring& t, int w) {
        RECT r{x, y - 1, x + w, y + 19};
        drawText(dc, t, r, kClipDim, 12, FW_NORMAL, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
        x += w;
    };

    if (mouseMode) {
        label(L"Click a card to paste", textWidth(dc, L"Click a card to paste", 12, FW_NORMAL) + 6);
        label(L" -  Scroll sideways for more   ", textWidth(dc, L" -  Scroll sideways for more   ", 12, FW_NORMAL) + 6);
    }
    x = drawKeycap(dc, x, y, L"Esc") + 8;
    label(L"close", 60);
}

void paintEmpty(HDC dc, int width, int height, bool mouseMode) {
    drawClipboardGlyph(dc, width / 2 - 8, height / 2 - 34, RGB(80, 90, 108));
    RECT t{0, height / 2 - 6, width, height / 2 + 20};
    drawText(dc, L"Nothing copied yet", t, RGB(190, 198, 212), 16, FW_SEMIBOLD, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    RECT s{40, height / 2 + 22, width - 40, height / 2 + 58};
    drawText(dc, mouseMode ? L"Copied items land here as cards, newest on the left."
                           : L"Copy text or an image and it'll appear here, newest at the top.",
             s, kClipDim, 12, FW_NORMAL, DT_CENTER | DT_WORDBREAK);
}

// ---- mouse variant: horizontal cards ---------------------------------------

COLORREF blendColor(COLORREF a, COLORREF b, float t) {
    t = (std::max)(0.0f, (std::min)(1.0f, t));
    auto mix = [&](int av, int bv) {
        return static_cast<int>(av + (bv - av) * t + 0.5f);
    };
    return RGB(mix(GetRValue(a), GetRValue(b)), mix(GetGValue(a), GetGValue(b)), mix(GetBValue(a), GetBValue(b)));
}

void paintCard(HDC dc, const RECT& card, const ClipItem& item, float hover) {
    hover = (std::max)(0.0f, (std::min)(1.0f, hover));
    const float lift = 3.0f * hover;
    RECT animated{
        card.left,
        static_cast<LONG>(std::lround(card.top - lift)),
        card.right,
        static_cast<LONG>(std::lround(card.bottom - lift))
    };

    if (hover > 0.01f) {
        gpRoundRect(dc, static_cast<float>(animated.left) - 3.0f - hover,
                    static_cast<float>(animated.top) - 3.0f - hover,
                    static_cast<float>(animated.right - animated.left) + 6.0f + hover * 2.0f,
                    static_cast<float>(animated.bottom - animated.top) + 6.0f + hover * 2.0f,
                    14.0f, RGB(15, 30, 54), static_cast<COLORREF>(-1), static_cast<BYTE>(130.0f * hover));
    }
    gpRoundRect(dc, static_cast<float>(animated.left), static_cast<float>(animated.top),
                static_cast<float>(animated.right - animated.left), static_cast<float>(animated.bottom - animated.top),
                12.0f,
                blendColor(kClipCard, RGB(28, 39, 58), hover),
                blendColor(kClipCardBrd, kClipAccent, hover),
                255,
                1.0f + hover);

    RECT preview{animated.left + 1, animated.top + 1, animated.right - 1, animated.bottom - 40};
    RECT strip{animated.left + 12, animated.bottom - 34, animated.right - 12, animated.bottom - 10};
    const bool link = isLinkText(item.text);

    if (item.kind == ClipKind::Image) {
        RECT thumb{animated.left + 8, animated.top + 8, animated.right - 8, animated.bottom - 44};
        gpRoundRect(dc, static_cast<float>(thumb.left), static_cast<float>(thumb.top),
                    static_cast<float>(thumb.right - thumb.left), static_cast<float>(thumb.bottom - thumb.top),
                    9.0f, blendColor(kClipImage, RGB(58, 92, 140), hover));
        drawThumb(dc, thumb, item);
        if (item.width > 0 && item.height > 0) {
            std::wstring dims = std::to_wstring(item.width) + L" Ã— " + std::to_wstring(item.height);
            int w = 16 + textWidth(dc, dims, 11, FW_SEMIBOLD);
            gpRoundRect(dc, static_cast<float>(thumb.left) + 8.0f, static_cast<float>(thumb.bottom) - 26.0f,
                        static_cast<float>(w), 18.0f, 4.0f, RGB(12, 14, 20), static_cast<COLORREF>(-1), 190);
            RECT dr{thumb.left + 8, thumb.bottom - 27, thumb.left + 8 + w, thumb.bottom - 9};
            drawText(dc, dims, dr, RGB(220, 226, 236), 11, FW_SEMIBOLD, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        }
    } else {
        RECT text{preview.left + 14, preview.top + 14, preview.right - 14, preview.bottom - 6};
        drawText(dc, itemTitle(item), text, link ? kClipLink : kClipText, 14,
                 FW_NORMAL, DT_LEFT | DT_TOP | DT_WORDBREAK | DT_END_ELLIPSIS | DT_NOPREFIX);
    }

    int tagRight = drawTagPill(dc, strip.left, strip.top + 2, itemTag(item));
    std::wstring time = relativeTime(item.capturedAt);
    int timeW = time.empty() ? 0 : textWidth(dc, time, 12, FW_NORMAL) + 4;
    RECT src{tagRight + 8, strip.top, strip.right - timeW, strip.bottom};
    drawText(dc, item.sourceApp, src, kClipDim, 12, FW_NORMAL, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    if (!time.empty()) {
        RECT tr{strip.right - timeW, strip.top, strip.right, strip.bottom};
        drawText(dc, time, tr, kClipDim, 12, FW_NORMAL, DT_RIGHT | DT_VCENTER | DT_SINGLELINE);
    }

    if (hover > 0.02f) {
        const int pillW = 118;
        const int px = (animated.left + animated.right) / 2 - pillW / 2;
        const int py = (animated.top + animated.bottom) / 2 - 16;
        gpRoundRect(dc, static_cast<float>(px), static_cast<float>(py), static_cast<float>(pillW), 32.0f, 16.0f,
                    RGB(244, 247, 252), static_cast<COLORREF>(-1), static_cast<BYTE>(245.0f * hover));
        RECT pr{px, py, px + pillW, py + 32};
        drawText(dc, L"Click to paste", pr, blendColor(RGB(244, 247, 252), RGB(24, 30, 42), hover), 13, FW_SEMIBOLD, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    }
}

void paintMouseCards(HDC dc, const std::vector<ClipItem>& items, int hover) {
    const int count = static_cast<int>(items.size());
    g_scrollX = clampScrollX(g_scrollX, count);
    g_scrollTargetX = clampScrollX(g_scrollTargetX, count);

    // Clip cards to the viewport so scrolled cards don't bleed under the chrome.
    HRGN clip = CreateRectRgn(kSidePad, kHeaderH, kMouseWinW - kSidePad, kHeaderH + 6 + kCardH + 6);
    SelectClipRgn(dc, clip);
    for (int i = 0; i < count; ++i) {
        RECT r = cardRect(i);
        if (r.right <= kSidePad || r.left >= kMouseWinW - kSidePad) continue;
        const float hoverProgress = i >= 0 && i < static_cast<int>(kMaxItems) ? g_hoverAmount[i] : (i == hover ? 1.0f : 0.0f);
        paintCard(dc, r, items[i], hoverProgress);
    }
    SelectClipRgn(dc, nullptr);
    DeleteObject(clip);

    if (count > 0 && maxScrollX(count) > 0) {
        const int trackY = kHeaderH + 6 + kCardH + 8;
        const int trackW = kMouseWinW - 2 * kSidePad;
        gpRoundRect(dc, static_cast<float>(kSidePad), static_cast<float>(trackY), static_cast<float>(trackW), 5.0f, 2.5f,
                    RGB(46, 52, 64));
        const int thumbW = (std::max)(48, trackW * mouseViewportWidth() / mouseContentWidth(count));
        const int thumbX = kSidePad + static_cast<int>(std::lround((trackW - thumbW) * g_scrollX / maxScrollX(count)));
        gpRoundRect(dc, static_cast<float>(thumbX), static_cast<float>(trackY), static_cast<float>(thumbW), 5.0f, 2.5f,
                    RGB(111, 128, 154));
    }
}

// ---- keyboard variant: vertical list ---------------------------------------

void paintKbRow(HDC dc, const RECT& row, const ClipItem& item, bool selected) {
    if (selected) {
        gpRoundRect(dc, static_cast<float>(row.left), static_cast<float>(row.top) + 3.0f,
                    static_cast<float>(row.right - row.left), static_cast<float>(kRowH - 6),
                    9.0f, RGB(30, 52, 92), kClipAccent, 255, 1.6f);
    }

    RECT tag{row.left + 10, row.top + (kRowH - 26) / 2, row.left + 50, row.top + (kRowH + 26) / 2};
    gpRoundRect(dc, static_cast<float>(tag.left), static_cast<float>(tag.top), 40.0f, 26.0f, 5.0f,
                selected ? RGB(40, 64, 108) : RGB(38, 44, 56), selected ? kClipAccent : RGB(54, 62, 78));
    drawText(dc, itemTag(item), tag, selected ? RGB(196, 216, 255) : kClipTag, 11, FW_BOLD, DT_CENTER | DT_VCENTER | DT_SINGLELINE);

    const int textLeft = tag.right + 12;
    std::wstring time = relativeTime(item.capturedAt);
    const int timeW = time.empty() ? 0 : textWidth(dc, time, 12, FW_NORMAL) + 8;
    const int rightPad = (selected ? 34 : 12) + timeW;

    RECT preview{textLeft, row.top + 7, row.right - rightPad, row.top + 28};
    const bool link = isLinkText(item.text);
    drawText(dc, itemTitle(item), preview, link ? kClipLink : (selected ? RGB(245, 249, 255) : kClipText),
             15, selected ? FW_SEMIBOLD : FW_NORMAL, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);

    RECT sub{textLeft, row.top + 27, row.right - rightPad, row.top + 45};
    drawText(dc, itemSubtitle(item), sub, kClipDim, 12, FW_NORMAL, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);

    if (!time.empty()) {
        RECT tr{row.right - rightPad, row.top + 7, row.right - (selected ? 34 : 12), row.top + 28};
        drawText(dc, time, tr, kClipDim, 12, FW_NORMAL, DT_RIGHT | DT_VCENTER | DT_SINGLELINE);
    }
    if (selected) {
        drawClipboardGlyph(dc, row.right - 26, row.top + (kRowH - 20) / 2, RGB(150, 190, 255));
    }
}

void paintKbRowClean(HDC dc, const RECT& row, const ClipItem& item, bool selected) {
    if (selected) {
        gpRoundRect(dc, static_cast<float>(row.left), static_cast<float>(row.top) + 2.0f,
                    static_cast<float>(row.right - row.left), static_cast<float>(kRowH - 4),
                    7.0f, RGB(30, 52, 92), kClipAccent, 255, 1.6f);
    }

    RECT tag{row.left + 10, row.top + 5, row.left + 44, row.top + 31};
    gpRoundRect(dc, static_cast<float>(tag.left), static_cast<float>(tag.top), 34.0f, 26.0f, 5.0f,
                selected ? RGB(40, 64, 108) : RGB(38, 44, 56), selected ? kClipAccent : RGB(54, 62, 78));
    drawText(dc, itemTag(item), tag, selected ? RGB(196, 216, 255) : kClipTag, 10, FW_BOLD, DT_CENTER | DT_VCENTER | DT_SINGLELINE);

    const int textLeft = tag.right + 10;
    std::wstring time = relativeTime(item.capturedAt);
    const int timeW = time.empty() ? 0 : textWidth(dc, time, 12, FW_NORMAL) + 8;
    const int rightPad = (selected ? 34 : 12) + timeW;

    RECT preview{textLeft, row.top + 3, row.right - rightPad, row.top + 20};
    const bool link = isLinkText(item.text);
    drawText(dc, itemTitle(item), preview, link ? kClipLink : (selected ? RGB(245, 249, 255) : kClipText),
             14, selected ? FW_SEMIBOLD : FW_NORMAL, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);

    RECT sub{textLeft, row.top + 20, row.right - rightPad, row.top + 35};
    drawText(dc, itemSubtitle(item), sub, kClipDim, 11, FW_NORMAL, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);

    if (!time.empty()) {
        RECT tr{row.right - rightPad, row.top + 3, row.right - (selected ? 34 : 12), row.top + 20};
        drawText(dc, time, tr, kClipDim, 12, FW_NORMAL, DT_RIGHT | DT_VCENTER | DT_SINGLELINE);
    }
    if (selected) {
        drawClipboardGlyph(dc, row.right - 25, row.top + 8, RGB(150, 190, 255));
    }
}

void paintKeyboardList(HDC dc, const std::vector<ClipItem>& items, int selected) {
    const int count = static_cast<int>(items.size());
    const int visible = (std::min)(count, static_cast<int>(kKbVisibleRows));
    const int first = kbFirstVisible(count, selected);
    for (int i = 0; i < visible; ++i) {
        const int idx = first + i;
        paintKbRowClean(dc, kbRowRect(i), items[idx], idx == selected);
    }

    if (count > static_cast<int>(kKbVisibleRows)) {
        const int trackX = kKbWinW - kSidePad + 4;
        const int trackY = kHeaderH + 8;
        const int trackH = static_cast<int>(kKbVisibleRows) * kRowH - 16;
        gpRoundRect(dc, static_cast<float>(trackX), static_cast<float>(trackY), 5.0f, static_cast<float>(trackH), 2.5f,
                    RGB(46, 52, 64));
        const int thumbH = (std::max)(42, trackH * static_cast<int>(kKbVisibleRows) / count);
        const int thumbY = trackY + (trackH - thumbH) * first / (count - static_cast<int>(kKbVisibleRows));
        gpRoundRect(dc, static_cast<float>(trackX), static_cast<float>(thumbY), 5.0f, static_cast<float>(thumbH), 2.5f,
                    RGB(100, 108, 124));
    }
}

void paintPicker(HDC dc, const RECT& rect) {
    ensureGdiplus();
    HBRUSH bg = CreateSolidBrush(kClipBg);
    FillRect(dc, &rect, bg);
    DeleteObject(bg);

    std::vector<ClipItem> items;
    bool mouseMode = false;
    int selected = 0;
    int hover = -1;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        items = g_items;
        mouseMode = g_mouseMode;
        selected = static_cast<int>(g_selected);
        hover = g_hoverIndex;
    }

    const int width = rect.right;
    const int height = rect.bottom;
    paintHeader(dc, width, items.size());

    if (items.empty()) {
        paintEmpty(dc, width, height, mouseMode);
    } else if (mouseMode) {
        paintMouseCards(dc, items, hover);
    } else {
        paintKeyboardList(dc, items, selected);
    }

    paintFooterClean(dc, width, height, mouseMode);
}

void paintPickerBuffered(HWND hwnd, HDC targetDc) {
    RECT rect{};
    GetClientRect(hwnd, &rect);
    const int width = rect.right - rect.left;
    const int height = rect.bottom - rect.top;
    if (width <= 0 || height <= 0) return;

    HDC memDc = CreateCompatibleDC(targetDc);
    HBITMAP bitmap = CreateCompatibleBitmap(targetDc, width, height);
    HGDIOBJ oldBitmap = SelectObject(memDc, bitmap);
    paintPicker(memDc, RECT{0, 0, width, height});
    BitBlt(targetDc, 0, 0, width, height, memDc, 0, 0, SRCCOPY);
    SelectObject(memDc, oldBitmap);
    DeleteObject(bitmap);
    DeleteDC(memDc);
}

void showPicker(DWORD milliseconds = 6500) {
    if (!g_window) return;
    PostMessageW(g_window, WM_CLIP_HISTORY_SHOW, milliseconds, 0);
}

void hidePicker() {
    if (!g_window) return;
    PostMessageW(g_window, WM_CLIP_HISTORY_HIDE, 0, 0);
}

bool writeTextToClipboard(const std::wstring& text) {
    SIZE_T bytes = (text.size() + 1) * sizeof(wchar_t);
    HGLOBAL handle = GlobalAlloc(GMEM_MOVEABLE, bytes);
    if (!handle) return false;
    void* data = GlobalLock(handle);
    if (!data) {
        GlobalFree(handle);
        return false;
    }
    memcpy(data, text.c_str(), bytes);
    GlobalUnlock(handle);
    SetClipboardData(CF_UNICODETEXT, handle);
    return true;
}

bool writeImageToClipboard(const std::vector<unsigned char>& dib) {
    if (dib.empty()) return false;
    HGLOBAL handle = GlobalAlloc(GMEM_MOVEABLE, dib.size());
    if (!handle) return false;
    void* data = GlobalLock(handle);
    if (!data) {
        GlobalFree(handle);
        return false;
    }
    memcpy(data, dib.data(), dib.size());
    GlobalUnlock(handle);
    SetClipboardData(CF_DIB, handle);
    return true;
}

bool copySelected(std::wstring* report) {
    ClipItem selected;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        if (g_items.empty()) {
            if (report) *report = L"Clipboard history is empty.";
            return false;
        }
        if (g_selected >= g_items.size()) g_selected = 0;
        selected = g_items[g_selected];
        g_pickerOpen = false;
    }

    if (!OpenClipboard(nullptr)) {
        if (report) *report = L"Could not open clipboard.";
        return false;
    }
    EmptyClipboard();
    g_internalWrite = true;
    bool ok = selected.kind == ClipKind::Text ? writeTextToClipboard(selected.text) : writeImageToClipboard(selected.dib);
    CloseClipboard();

    if (report) *report = ok ? L"Copied from clipboard history: " + itemTitle(selected) : L"Could not copy selected history item.";
    hidePicker();
    return ok;
}

// Synthesize Ctrl+V into whatever window currently has focus. Raw Input reports
// injected keys too, so mark it before sending or a ctrl+v binding would fire
// itself again.
void sendCtrlV() {
    suppressNextSyntheticHotkey('V', 1200);

    INPUT inputs[4]{};
    inputs[0].type = INPUT_KEYBOARD; inputs[0].ki.wVk = VK_CONTROL;
    inputs[1].type = INPUT_KEYBOARD; inputs[1].ki.wVk = 'V';
    inputs[2].type = INPUT_KEYBOARD; inputs[2].ki.wVk = 'V'; inputs[2].ki.dwFlags = KEYEVENTF_KEYUP;
    inputs[3].type = INPUT_KEYBOARD; inputs[3].ki.wVk = VK_CONTROL; inputs[3].ki.dwFlags = KEYEVENTF_KEYUP;
    SendInput(4, inputs, sizeof(INPUT));
}

// Mouse variant: copy the clicked item to the clipboard and paste it into the
// window that was focused when the picker opened.
bool pasteItemAt(int index) {
    ClipItem item;
    HWND target = nullptr;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        if (index < 0 || static_cast<size_t>(index) >= g_items.size()) return false;
        item = g_items[index];
        target = g_pasteTarget;
        g_pickerOpen = false;
        g_mouseMode = false;
        g_hoverIndex = -1;
    }

    if (!OpenClipboard(nullptr)) return false;
    EmptyClipboard();
    g_internalWrite = true;
    bool ok = item.kind == ClipKind::Text ? writeTextToClipboard(item.text) : writeImageToClipboard(item.dib);
    CloseClipboard();

    hidePicker();

    if (ok) {
        // The picker never took focus (WS_EX_NOACTIVATE), so the target is still
        // foreground; restore it defensively, then paste.
        if (target && IsWindow(target)) SetForegroundWindow(target);
        Sleep(30);
        sendCtrlV();
    }
    return ok;
}

bool animateHoverAmounts(int count) {
    bool active = false;
    const int limit = (std::min)(count, static_cast<int>(kMaxItems));
    for (int i = 0; i < static_cast<int>(kMaxItems); ++i) {
        const float target = (i < limit && i == g_hoverIndex) ? 1.0f : 0.0f;
        const float delta = target - g_hoverAmount[i];
        if (std::abs(delta) < 0.035f) {
            g_hoverAmount[i] = target;
        } else {
            g_hoverAmount[i] += delta * 0.32f;
            active = true;
        }
    }
    return active;
}

void startHoverAnimation(HWND hwnd) {
    SetTimer(hwnd, kHoverTimer, 8, nullptr);
}

LRESULT CALLBACK clipboardWndProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
    switch (message) {
    case WM_CLIPBOARDUPDATE:
        captureClipboardNow();
        return 0;
    case WM_CLIP_HISTORY_SHOW: {
        size_t itemCount = 0;
        bool mouseMode = false;
        {
            std::lock_guard<std::mutex> lock(g_mutex);
            g_pickerOpen = true;
            itemCount = g_items.size();
            mouseMode = g_mouseMode;
        }
        const int width = pickerWidth(mouseMode);
        const int height = pickerHeight(mouseMode, itemCount);
        const POINT position = pickerPositionForSize(width, height);
        HRGN region = CreateRoundRectRgn(0, 0, width + 1, height + 1, 18, 18);
        SetWindowRgn(hwnd, region, TRUE);
        showPickerTopmost(hwnd, position.x, position.y, width, height);
        noteOverlayShown(L"clipboard history picker");
        InvalidateRect(hwnd, nullptr, FALSE);
        SetTimer(hwnd, kPickerTimer, static_cast<UINT>(wParam == 0 ? kPickerOpenMs : wParam), nullptr);
        SetTimer(hwnd, kTopmostPulseTimer, 250, nullptr);
        g_mouseTracking = false;
        startHoverAnimation(hwnd);
        return 0;
    }
    case WM_MOUSEMOVE: {
        if (!g_mouseMode) return 0;
        keepPickerAlive(hwnd);
        if (!g_mouseTracking) {
            TRACKMOUSEEVENT tme{sizeof(tme), TME_LEAVE, hwnd, 0};
            TrackMouseEvent(&tme);
            g_mouseTracking = true;
        }
        int hit = -1;
        {
            std::lock_guard<std::mutex> lock(g_mutex);
            hit = cardHitTest(GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam), static_cast<int>(g_items.size()));
            if (hit != g_hoverIndex) {
                g_hoverIndex = hit;
                startHoverAnimation(hwnd);
                InvalidateRect(hwnd, nullptr, FALSE);
            }
        }
        return 0;
    }
    case WM_MOUSELEAVE:
        g_mouseTracking = false;
        if (g_hoverIndex != -1) {
            g_hoverIndex = -1;
            startHoverAnimation(hwnd);
            InvalidateRect(hwnd, nullptr, FALSE);
        }
        return 0;
    case WM_LBUTTONDOWN: {
        if (!g_mouseMode) return 0;
        keepPickerAlive(hwnd);
        int hit = -1;
        {
            std::lock_guard<std::mutex> lock(g_mutex);
            hit = cardHitTest(GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam), static_cast<int>(g_items.size()));
        }
        if (hit >= 0) {
            KillTimer(hwnd, kPickerTimer);
            pasteItemAt(hit);
        }
        return 0;
    }
    case WM_MOUSEWHEEL: {
        if (!g_mouseMode) return 0;
        keepPickerAlive(hwnd);
        const int delta = GET_WHEEL_DELTA_WPARAM(wParam);
        POINT cursor{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
        ScreenToClient(hwnd, &cursor);
        bool changed = false;
        bool hoverChanged = false;
        {
            std::lock_guard<std::mutex> lock(g_mutex);
            const int count = static_cast<int>(g_items.size());
            const float next = clampScrollX(g_scrollTargetX - (delta / WHEEL_DELTA) * 120.0f, count);
            changed = next != g_scrollTargetX;
            g_scrollTargetX = next;
            const int nextHover = cardHitTest(cursor.x, cursor.y, count);
            hoverChanged = nextHover != g_hoverIndex;
            g_hoverIndex = nextHover;
        }
        if (changed) SetTimer(hwnd, kScrollTimer, 8, nullptr);
        if (hoverChanged) startHoverAnimation(hwnd);
        return 0;
    }
    case WM_SETCURSOR:
        if (g_mouseMode) {
            SetCursor(LoadCursorW(nullptr, IDC_HAND));
            return TRUE;
        }
        return DefWindowProcW(hwnd, message, wParam, lParam);
    case WM_CLIP_HISTORY_HIDE:
        g_pickerOpen = false;
        ShowWindow(hwnd, SW_HIDE);
        KillTimer(hwnd, kPickerTimer);
        KillTimer(hwnd, kScrollTimer);
        KillTimer(hwnd, kHoverTimer);
        KillTimer(hwnd, kTopmostPulseTimer);
        return 0;
    case WM_TIMER:
        if (wParam == kPickerTimer) {
            g_pickerOpen = false;
            ShowWindow(hwnd, SW_HIDE);
            KillTimer(hwnd, kPickerTimer);
            KillTimer(hwnd, kScrollTimer);
            KillTimer(hwnd, kHoverTimer);
            KillTimer(hwnd, kTopmostPulseTimer);
        } else if (wParam == kScrollTimer) {
            bool done = false;
            {
                std::lock_guard<std::mutex> lock(g_mutex);
                const float delta = g_scrollTargetX - g_scrollX;
                if (std::abs(delta) < 0.35f) {
                    g_scrollX = g_scrollTargetX;
                    done = true;
                } else {
                    g_scrollX += delta * 0.24f;
                }
            }
            InvalidateRect(hwnd, nullptr, FALSE);
            if (done) KillTimer(hwnd, kScrollTimer);
        } else if (wParam == kHoverTimer) {
            bool active = false;
            {
                std::lock_guard<std::mutex> lock(g_mutex);
                active = animateHoverAmounts(static_cast<int>(g_items.size()));
            }
            InvalidateRect(hwnd, nullptr, FALSE);
            if (!active) KillTimer(hwnd, kHoverTimer);
        } else if (wParam == kTopmostPulseTimer) {
            pulsePickerTopmost(hwnd);
        }
        return 0;
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(hwnd, &ps);
        paintPickerBuffered(hwnd, dc);
        EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_DESTROY:
        RemoveClipboardFormatListener(hwnd);
        PostQuitMessage(0);
        return 0;
    default:
        return DefWindowProcW(hwnd, message, wParam, lParam);
    }
}

void clipboardThreadMain() {
    g_threadId = GetCurrentThreadId();
    loadTextHistory();

    WNDCLASSW wc{};
    wc.lpfnWndProc = clipboardWndProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"KeyRailClipboardHistory";
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    RegisterClassW(&wc);

    g_window = CreateWindowExW(
        WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_LAYERED,
        wc.lpszClassName,
        L"KeyRail Clipboard History",
        WS_POPUP,
        100,
        100,
        490,
        318,
        nullptr,
        nullptr,
        wc.hInstance,
        nullptr);

    if (g_window) {
        SetLayeredWindowAttributes(g_window, 0, 252, LWA_ALPHA);
        AddClipboardFormatListener(g_window);
    }

    if (g_ready) SetEvent(g_ready);

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
}

void ensureClipboardThread() {
    if (g_window) return;
    if (!g_ready) g_ready = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!g_thread.joinable()) {
        g_thread = std::thread(clipboardThreadMain);
        g_thread.detach();
    }
    WaitForSingleObject(g_ready, 2000);
}

} // namespace

void startClipboardHistory() {
    ensureClipboardThread();
}

void stopClipboardHistory() {
    if (g_window) PostMessageW(g_window, WM_CLOSE, 0, 0);
    g_window = nullptr;
}

bool openClipboardHistoryPicker(std::wstring* report) {
    ensureClipboardThread();
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        g_selected = 0;
        g_mouseMode = false;
        g_hoverIndex = -1;
        std::fill(g_hoverAmount, g_hoverAmount + kMaxItems, 0.0f);
        g_pickerOpen = true;
        if (report) *report = L"Clipboard history items: " + std::to_wstring(g_items.size());
    }
    showPicker();
    return true;
}

bool openClipboardQuickPicker(std::wstring* report) {
    ensureClipboardThread();
    HWND target = GetForegroundWindow();
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        g_mouseMode = true;
        g_hoverIndex = -1;
        g_scrollX = 0;
        g_scrollTargetX = 0;
        std::fill(g_hoverAmount, g_hoverAmount + kMaxItems, 0.0f);
        g_pasteTarget = target;
        g_pickerOpen = true;
        if (report) *report = L"Quick clipboard: " + std::to_wstring(g_items.size()) + L" item(s)";
    }
    showPicker(8000);
    return true;
}

bool moveClipboardHistoryPicker(int direction, std::wstring* report) {
    ensureClipboardThread();
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        if (!g_pickerOpen) g_pickerOpen = true;
        if (g_items.empty()) {
            if (report) *report = L"Clipboard history is empty.";
            showPicker();
            return false;
        }
        const size_t count = g_items.size();
        if (direction >= 0) g_selected = (g_selected + 1) % count;
        else g_selected = (g_selected + count - 1) % count;
        if (report) *report = L"Clipboard item: " + itemTitle(g_items[g_selected]);
    }
    showPicker();
    return true;
}

bool confirmClipboardHistoryPicker(std::wstring* report) {
    ensureClipboardThread();
    return copySelected(report);
}

bool cancelClipboardHistoryPicker(std::wstring* report) {
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        g_pickerOpen = false;
        g_mouseMode = false;
        g_hoverIndex = -1;
    }
    hidePicker();
    if (report) *report = L"Clipboard history picker cancelled.";
    return true;
}

bool clipboardHistoryPickerIsOpen() {
    std::lock_guard<std::mutex> lock(g_mutex);
    return g_pickerOpen;
}

std::wstring describeClipboardHistory() {
    ensureClipboardThread();
    std::lock_guard<std::mutex> lock(g_mutex);
    std::wstringstream out;
    out << L"clipboard history items: " << g_items.size() << L"\n";
    out << L"path: " << appDataPath() << L"\n";
    for (size_t i = 0; i < (std::min)(g_items.size(), static_cast<size_t>(10)); ++i) {
        out << (i + 1) << L". " << itemTitle(g_items[i]) << L"\n";
    }
    return out.str();
}
