#include "media_sessions.h"

#include "browser_bridge.h"

#include <windows.h>

#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Media.Control.h>
#include <winrt/base.h>

#include <algorithm>
#include <cwctype>
#include <functional>
#include <mutex>
#include <sstream>
#include <thread>

using winrt::Windows::Media::Control::GlobalSystemMediaTransportControlsSession;
using winrt::Windows::Media::Control::GlobalSystemMediaTransportControlsSessionManager;
using winrt::Windows::Media::Control::GlobalSystemMediaTransportControlsSessionPlaybackStatus;

namespace {

constexpr UINT WM_MEDIA_OVERLAY_UPDATE = WM_APP + 40;
constexpr UINT WM_MEDIA_OVERLAY_HIDE = WM_APP + 41;
constexpr UINT_PTR kOverlayTimer = 1;
constexpr int kPickerWidth = 612;
constexpr int kToastWidth = 460;

std::mutex g_pickerMutex;
std::vector<MediaTarget> g_pickerTargets;
size_t g_pickerIndex = 0;
bool g_pickerOpen = false;
bool g_hasSelectedTarget = false;
std::wstring g_selectedId;
std::wstring g_selectedKind;
std::wstring g_selectedAppId;
std::wstring g_selectedTitle;
unsigned int g_selectedSessionIndex = 0;

enum class OverlayMode {
    Message,
    Picker,
    Confirmation,
};

struct OverlayState {
    OverlayMode mode = OverlayMode::Message;
    std::wstring message;
    std::vector<MediaTarget> targets;
    size_t selected = 0;
};

std::mutex g_overlayMutex;
OverlayState g_overlayState;
HWND g_overlayWindow = nullptr;
DWORD g_overlayThreadId = 0;
HANDLE g_overlayReady = nullptr;
std::thread g_overlayThread;

std::wstring wideFromHstring(const winrt::hstring& value) {
    return std::wstring(value.c_str(), value.size());
}

std::wstring lowerCopy(std::wstring value) {
    std::transform(value.begin(), value.end(), value.begin(), [](wchar_t c) {
        return static_cast<wchar_t>(towlower(c));
    });
    return value;
}

std::wstring statusName(GlobalSystemMediaTransportControlsSessionPlaybackStatus status) {
    switch (status) {
    case GlobalSystemMediaTransportControlsSessionPlaybackStatus::Closed:
        return L"closed";
    case GlobalSystemMediaTransportControlsSessionPlaybackStatus::Opened:
        return L"opened";
    case GlobalSystemMediaTransportControlsSessionPlaybackStatus::Changing:
        return L"changing";
    case GlobalSystemMediaTransportControlsSessionPlaybackStatus::Stopped:
        return L"stopped";
    case GlobalSystemMediaTransportControlsSessionPlaybackStatus::Playing:
        return L"playing";
    case GlobalSystemMediaTransportControlsSessionPlaybackStatus::Paused:
        return L"paused";
    default:
        return L"unknown";
    }
}

std::wstring displayName(const MediaTarget& target) {
    std::wstring text;
    if (!target.title.empty()) {
        text = target.title;
        if (!target.artist.empty()) text += L" - " + target.artist;
    } else {
        text = target.appId.empty() ? L"Untitled media session" : target.appId;
    }
    return text;
}

std::wstring mediaTitle(const MediaTarget& target) {
    if (!target.title.empty()) return target.title;
    if (!target.tabTitle.empty()) return target.tabTitle;
    if (!target.documentTitle.empty()) return target.documentTitle;
    return target.appId.empty() ? L"Untitled media" : target.appId;
}

std::wstring mediaSubtitle(const MediaTarget& target) {
    if (!target.artist.empty()) return target.artist;
    if (!target.documentTitle.empty() && target.documentTitle != mediaTitle(target)) return target.documentTitle;
    return target.playing ? L"Playing" : L"Paused";
}

std::wstring shortAppName(const std::wstring& appId) {
    if (appId.empty()) return L"unknown app";

    size_t bang = appId.find(L'!');
    std::wstring value = bang == std::wstring::npos ? appId : appId.substr(0, bang);

    size_t dot = value.find_last_of(L'.');
    if (dot != std::wstring::npos && dot + 1 < value.size()) {
        value = value.substr(dot + 1);
    }
    return value;
}

bool isBrowserWindowsSession(const MediaTarget& target) {
    if (target.kind != L"windows") return false;

    const std::wstring app = lowerCopy(target.appId);
    return app.find(L"brave") != std::wstring::npos
        || app.find(L"chrome") != std::wstring::npos
        || app.find(L"edge") != std::wstring::npos
        || app.find(L"firefox") != std::wstring::npos
        || app.find(L"opera") != std::wstring::npos
        || app.find(L"vivaldi") != std::wstring::npos;
}

std::wstring hostFromUrl(const std::wstring& url) {
    size_t start = url.find(L"://");
    start = start == std::wstring::npos ? 0 : start + 3;
    while (start < url.size() && url[start] == L'/') ++start;

    size_t end = url.find_first_of(L"/?#", start);
    std::wstring host = url.substr(start, end == std::wstring::npos ? std::wstring::npos : end - start);
    size_t at = host.rfind(L'@');
    if (at != std::wstring::npos) host = host.substr(at + 1);
    size_t colon = host.find(L':');
    if (colon != std::wstring::npos) host = host.substr(0, colon);
    if (host.rfind(L"www.", 0) == 0) host = host.substr(4);
    return host;
}

std::wstring sourceName(const MediaTarget& target) {
    if (!target.sourceHost.empty()) return target.sourceHost;
    std::wstring host = hostFromUrl(target.url);
    if (!host.empty()) return host;
    return shortAppName(target.appId);
}

std::wstring commandToastText(MediaCommand command) {
    switch (command) {
    case MediaCommand::TogglePlayPause:
        return L"Play / pause sent";
    case MediaCommand::Next:
        return L"Next track sent";
    case MediaCommand::Previous:
        return L"Previous track sent";
    default:
        return L"Media command sent";
    }
}

COLORREF artworkColor(const MediaTarget& target, int offset = 0) {
    const size_t hash = std::hash<std::wstring>{}(mediaTitle(target) + sourceName(target));
    const int r = 42 + static_cast<int>((hash >> (offset + 0)) & 0x3f);
    const int g = 46 + static_cast<int>((hash >> (offset + 7)) & 0x3f);
    const int b = 70 + static_cast<int>((hash >> (offset + 14)) & 0x5f);
    return RGB((std::min)(r, 145), (std::min)(g, 145), (std::min)(b, 170));
}

std::wstring overlayTextForTargets(const std::vector<MediaTarget>& targets, size_t selected, bool active) {
    std::wstringstream out;
    out << (active ? L"Choose media target" : L"Selected media target") << L"\n\n";

    if (targets.empty()) {
        out << L"No Windows media sessions found.";
        return out.str();
    }

    const size_t first = selected > 3 ? selected - 3 : 0;
    const size_t last = (std::min)(targets.size(), first + 7);

    for (size_t i = first; i < last; ++i) {
        const MediaTarget& target = targets[i];
        out << (i == selected ? L"> " : L"  ");
        out << (target.playing ? L"[playing] " : L"[paused] ");
        out << shortAppName(target.appId) << L" - " << displayName(target);
        if (target.kind == L"browser") out << L" [tab]";
        out << L"\n";
    }

    if (active) {
        out << L"\nNext/Previous moves, Play/Pause selects.";
    }
    return out.str();
}

HFONT makeFont(int height, int weight = FW_NORMAL) {
    return CreateFontW(
        -height, 0, 0, 0, weight, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
        OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
        DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
}

void fillRoundRect(HDC dc, const RECT& rect, int radius, COLORREF fill, COLORREF border = static_cast<COLORREF>(-1)) {
    HBRUSH brush = CreateSolidBrush(fill);
    HPEN pen = border == static_cast<COLORREF>(-1)
        ? static_cast<HPEN>(GetStockObject(NULL_PEN))
        : CreatePen(PS_SOLID, 1, border);
    HGDIOBJ oldBrush = SelectObject(dc, brush);
    HGDIOBJ oldPen = SelectObject(dc, pen);
    RoundRect(dc, rect.left, rect.top, rect.right, rect.bottom, radius, radius);
    SelectObject(dc, oldPen);
    SelectObject(dc, oldBrush);
    if (border != static_cast<COLORREF>(-1)) DeleteObject(pen);
    DeleteObject(brush);
}

void fillRectColor(HDC dc, const RECT& rect, COLORREF color) {
    HBRUSH brush = CreateSolidBrush(color);
    FillRect(dc, &rect, brush);
    DeleteObject(brush);
}

void drawTextLine(HDC dc, const std::wstring& text, RECT rect, HFONT font, COLORREF color, UINT align = DT_LEFT) {
    HGDIOBJ oldFont = SelectObject(dc, font);
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, color);
    DrawTextW(dc, text.c_str(), -1, &rect, align | DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS | DT_NOPREFIX);
    SelectObject(dc, oldFont);
}

void drawShortcut(HDC dc, int x, int y, const std::wstring& key, const std::wstring& label, HFONT keyFont, HFONT labelFont) {
    const int keyWidth = key.size() <= 3 ? 40 : 52;
    RECT keyRect{x, y, x + keyWidth, y + 24};
    fillRoundRect(dc, keyRect, 6, RGB(38, 40, 48), RGB(70, 73, 84));
    drawTextLine(dc, key, keyRect, keyFont, RGB(238, 240, 245), DT_CENTER);

    RECT labelRect{keyRect.right + 8, y, keyRect.right + 140, y + 24};
    drawTextLine(dc, label, labelRect, labelFont, RGB(160, 164, 176));
}

void drawPlaybackBadge(HDC dc, const RECT& art, bool playing) {
    const int radius = 10;
    const int cx = art.right - 5;
    const int cy = art.bottom - 5;
    HBRUSH brush = CreateSolidBrush(playing ? RGB(92, 142, 255) : RGB(78, 82, 94));
    HPEN pen = CreatePen(PS_SOLID, 1, RGB(14, 16, 23));
    HGDIOBJ oldBrush = SelectObject(dc, brush);
    HGDIOBJ oldPen = SelectObject(dc, pen);
    Ellipse(dc, cx - radius, cy - radius, cx + radius, cy + radius);
    SelectObject(dc, oldPen);
    SelectObject(dc, oldBrush);
    DeleteObject(pen);
    DeleteObject(brush);

    HBRUSH mark = CreateSolidBrush(RGB(9, 11, 16));
    oldBrush = SelectObject(dc, mark);
    oldPen = SelectObject(dc, GetStockObject(NULL_PEN));
    if (playing) {
        POINT triangle[]{{cx - 3, cy - 5}, {cx - 3, cy + 5}, {cx + 5, cy}};
        Polygon(dc, triangle, 3);
    } else {
        RECT left{cx - 4, cy - 5, cx - 1, cy + 5};
        RECT right{cx + 2, cy - 5, cx + 5, cy + 5};
        FillRect(dc, &left, mark);
        FillRect(dc, &right, mark);
    }
    SelectObject(dc, oldPen);
    SelectObject(dc, oldBrush);
    DeleteObject(mark);
}

void drawArtwork(HDC dc, const RECT& art, const MediaTarget& target, bool selected) {
    fillRoundRect(dc, art, 9, artworkColor(target), selected ? RGB(92, 142, 255) : RGB(46, 49, 58));

    if (!target.playing) {
        HPEN pen = CreatePen(PS_SOLID, 1, RGB(125, 132, 150));
        HGDIOBJ oldPen = SelectObject(dc, pen);
        HGDIOBJ oldBrush = SelectObject(dc, GetStockObject(NULL_BRUSH));
        POINT triangle[]{{art.left + 19, art.top + 16}, {art.left + 19, art.top + 30}, {art.left + 30, art.top + 23}};
        Polygon(dc, triangle, 3);
        SelectObject(dc, oldBrush);
        SelectObject(dc, oldPen);
        DeleteObject(pen);
    }

    drawPlaybackBadge(dc, art, target.playing);
}

void drawEqualizer(HDC dc, int x, int y, COLORREF color) {
    HBRUSH brush = CreateSolidBrush(color);
    const int heights[] = {13, 23, 17};
    for (int i = 0; i < 3; ++i) {
        RECT bar{x + i * 5, y + 24 - heights[i], x + i * 5 + 3, y + 24};
        FillRect(dc, &bar, brush);
    }
    DeleteObject(brush);
}

void drawSourceChip(HDC dc, int x, int y, const MediaTarget& target, HFONT font) {
    RECT favicon{x, y + 5, x + 14, y + 19};
    fillRoundRect(dc, favicon, 3, artworkColor(target, 9), RGB(88, 96, 118));

    RECT hostRect{x + 22, y, x + 150, y + 24};
    drawTextLine(dc, sourceName(target), hostRect, font, RGB(137, 141, 153));

    RECT chip{hostRect.right + 6, y + 3, hostRect.right + 38, y + 21};
    fillRoundRect(dc, chip, 4, RGB(44, 46, 55), RGB(70, 73, 84));
    drawTextLine(dc, target.kind == L"browser" ? L"TAB" : L"APP", chip, font, RGB(158, 162, 174), DT_CENTER);
}

size_t firstVisibleIndex(size_t count, size_t selected, size_t visibleRows) {
    if (count <= visibleRows || selected < visibleRows) return 0;
    size_t first = selected - visibleRows + 1;
    return (std::min)(first, count - visibleRows);
}

void drawMediaRow(HDC dc, const RECT& row, const MediaTarget& target, bool selected, HFONT titleFont, HFONT bodyFont, HFONT metaFont) {
    if (selected) {
        fillRoundRect(dc, row, 10, RGB(42, 55, 82), RGB(92, 142, 255));
        RECT edge{row.left, row.top + 8, row.left + 4, row.bottom - 8};
        fillRoundRect(dc, edge, 4, RGB(92, 142, 255));
    }

    RECT art{row.left + 18, row.top + 12, row.left + 64, row.top + 58};
    drawArtwork(dc, art, target, selected);

    RECT titleRect{art.right + 14, row.top + 11, row.right - 52, row.top + 32};
    drawTextLine(dc, mediaTitle(target), titleRect, titleFont, RGB(240, 242, 248));

    RECT subtitleRect{titleRect.left, row.top + 32, row.right - 52, row.top + 52};
    drawTextLine(dc, mediaSubtitle(target), subtitleRect, bodyFont, RGB(164, 168, 181));

    drawSourceChip(dc, titleRect.left, row.top + 50, target, metaFont);

    if (target.playing) {
        drawEqualizer(dc, row.right - 30, row.top + 24, RGB(92, 142, 255));
    }
}

void drawFooter(HDC dc, const RECT& footer, bool picker, bool hasControlledTarget, HFONT keyFont, HFONT labelFont) {
    RECT line{footer.left, footer.top, footer.right, footer.top + 1};
    fillRectColor(dc, line, RGB(43, 46, 56));

    int x = footer.left + 16;
    if (picker) {
        drawShortcut(dc, x, footer.top + 12, L"|<", L"Next / Prev move", keyFont, labelFont);
        x += 206;
        drawShortcut(dc, x, footer.top + 12, L">||", L"Play / Pause selects", keyFont, labelFont);
        x += 206;
    } else if (hasControlledTarget) {
        drawShortcut(dc, x, footer.top + 12, L">||", L"Media keys now control this tab", keyFont, labelFont);
        x += 240;
    }
    drawShortcut(dc, x, footer.top + 12, L"Esc", L"Dismiss", keyFont, labelFont);
}

void paintPickerOverlay(HDC dc, const RECT& bounds, const OverlayState& state, HFONT titleFont, HFONT bodyFont, HFONT metaFont, HFONT keyFont) {
    RECT panel{0, 0, bounds.right, bounds.bottom};
    fillRoundRect(dc, panel, 18, RGB(29, 31, 39), RGB(58, 61, 72));

    RECT accent{panel.left + 16, panel.top + 17, panel.left + 19, panel.top + 31};
    fillRoundRect(dc, accent, 2, state.targets.empty() ? RGB(81, 85, 98) : RGB(92, 142, 255));

    RECT headerTitle{panel.left + 30, panel.top + 8, panel.right - 100, panel.top + 42};
    drawTextLine(dc, L"Choose media target", headerTitle, titleFont, RGB(244, 246, 250));

    size_t browserCount = 0;
    for (const MediaTarget& target : state.targets) {
        if (target.kind == L"browser") ++browserCount;
    }
    const std::wstring count = std::to_wstring(state.targets.size())
        + (browserCount == state.targets.size() ? L" tabs" : L" targets");
    RECT countRect{panel.right - 92, panel.top + 8, panel.right - 18, panel.top + 42};
    drawTextLine(dc, count, countRect, metaFont, RGB(146, 150, 162), DT_RIGHT);

    RECT footer{panel.left, panel.bottom - 48, panel.right, panel.bottom};
    RECT content{panel.left + 8, panel.top + 54, panel.right - 8, footer.top};

    if (state.targets.empty()) {
        drawEqualizer(dc, (panel.left + panel.right) / 2 - 8, content.top + 58, RGB(72, 76, 90));
        RECT emptyTitle{content.left + 34, content.top + 100, content.right - 34, content.top + 126};
        drawTextLine(dc, L"No media playing in any tab", emptyTitle, titleFont, RGB(211, 214, 224), DT_CENTER);
        RECT emptyBody{content.left + 80, content.top + 134, content.right - 80, content.top + 178};
        drawTextLine(dc, L"Start playback in a browser tab and it will appear here.", emptyBody, bodyFont, RGB(141, 145, 156), DT_CENTER);
        drawFooter(dc, footer, false, false, keyFont, metaFont);
        return;
    }

    const int rowHeight = 76;
    const size_t visibleRows = (std::min)(state.targets.size(), static_cast<size_t>((content.bottom - content.top) / rowHeight));
    const size_t first = firstVisibleIndex(state.targets.size(), state.selected, visibleRows);

    for (size_t rowIndex = 0; rowIndex < visibleRows; ++rowIndex) {
        const size_t targetIndex = first + rowIndex;
        RECT row{content.left, content.top + static_cast<int>(rowIndex) * rowHeight,
                 content.right, content.top + static_cast<int>(rowIndex + 1) * rowHeight - 6};
        drawMediaRow(dc, row, state.targets[targetIndex], targetIndex == state.selected, titleFont, bodyFont, metaFont);
    }

    drawFooter(dc, footer, true, false, keyFont, metaFont);
}

void paintConfirmationOverlay(HDC dc, const RECT& bounds, const OverlayState& state, HFONT titleFont, HFONT bodyFont, HFONT metaFont, HFONT keyFont) {
    RECT panel{0, 0, bounds.right, bounds.bottom};
    fillRoundRect(dc, panel, 18, RGB(29, 31, 39), RGB(92, 142, 255));

    RECT banner{panel.left, panel.top, panel.right, panel.top + 48};
    fillRoundRect(dc, banner, 16, RGB(42, 55, 82), RGB(92, 142, 255));
    RECT check{banner.left + 18, banner.top + 15, banner.left + 36, banner.top + 33};
    HBRUSH checkBrush = CreateSolidBrush(RGB(92, 142, 255));
    HPEN noPen = static_cast<HPEN>(GetStockObject(NULL_PEN));
    HGDIOBJ oldBrush = SelectObject(dc, checkBrush);
    HGDIOBJ oldPen = SelectObject(dc, noPen);
    Ellipse(dc, check.left, check.top, check.right, check.bottom);
    SelectObject(dc, oldPen);
    SelectObject(dc, oldBrush);
    DeleteObject(checkBrush);
    RECT bannerText{banner.left + 46, banner.top + 8, banner.right - 16, banner.bottom};
    drawTextLine(dc, L"NOW CONTROLLING", bannerText, metaFont, RGB(166, 194, 255));

    if (!state.targets.empty()) {
        RECT row{panel.left + 16, panel.top + 66, panel.right - 16, panel.top + 142};
        drawMediaRow(dc, row, state.targets[0], false, titleFont, bodyFont, metaFont);
    }

    RECT footer{panel.left, panel.bottom - 48, panel.right, panel.bottom};
    drawFooter(dc, footer, false, true, keyFont, metaFont);

    RECT dismissBar{panel.left + 1, panel.bottom - 3, panel.right - 1, panel.bottom - 1};
    fillRectColor(dc, dismissBar, RGB(92, 142, 255));
}

void paintMessageOverlay(HDC dc, const RECT& bounds, const OverlayState& state, HFONT titleFont) {
    RECT panel{0, 0, bounds.right, bounds.bottom};
    fillRoundRect(dc, panel, 18, RGB(29, 31, 39), RGB(70, 82, 112));

    RECT dot{18, 24, 38, 44};
    HBRUSH dotBrush = CreateSolidBrush(RGB(92, 142, 255));
    HGDIOBJ oldBrush = SelectObject(dc, dotBrush);
    HGDIOBJ oldPen = SelectObject(dc, GetStockObject(NULL_PEN));
    Ellipse(dc, dot.left, dot.top, dot.right, dot.bottom);
    SelectObject(dc, oldPen);
    SelectObject(dc, oldBrush);
    DeleteObject(dotBrush);

    HPEN checkPen = CreatePen(PS_SOLID, 2, RGB(12, 15, 22));
    HGDIOBJ oldCheckPen = SelectObject(dc, checkPen);
    MoveToEx(dc, 23, 34, nullptr);
    LineTo(dc, 27, 38);
    LineTo(dc, 34, 29);
    SelectObject(dc, oldCheckPen);
    DeleteObject(checkPen);

    RECT textRect{50, 13, panel.right - 18, panel.bottom - 12};
    drawTextLine(dc, state.message, textRect, titleFont, RGB(240, 242, 248));
}

void paintOverlay(HDC dc, const RECT& bounds, const OverlayState& state) {
    HFONT titleFont = makeFont(15, FW_SEMIBOLD);
    HFONT bodyFont = makeFont(13, FW_NORMAL);
    HFONT metaFont = makeFont(11, FW_NORMAL);
    HFONT keyFont = makeFont(10, FW_SEMIBOLD);

    if (state.mode == OverlayMode::Picker) {
        paintPickerOverlay(dc, bounds, state, titleFont, bodyFont, metaFont, keyFont);
    } else if (state.mode == OverlayMode::Confirmation) {
        paintConfirmationOverlay(dc, bounds, state, titleFont, bodyFont, metaFont, keyFont);
    } else {
        paintMessageOverlay(dc, bounds, state, titleFont);
    }

    DeleteObject(keyFont);
    DeleteObject(metaFont);
    DeleteObject(bodyFont);
    DeleteObject(titleFont);
}

int overlayHeightForState(const OverlayState& state) {
    if (state.mode == OverlayMode::Message) return 68;
    if (state.mode == OverlayMode::Confirmation) return 212;
    if (state.mode == OverlayMode::Picker && state.targets.empty()) return 272;
    if (state.mode == OverlayMode::Picker) {
        const size_t visibleRows = (std::min)(state.targets.size(), static_cast<size_t>(5));
        return 54 + static_cast<int>(visibleRows) * 76 + 48;
    }
    return 68;
}

int overlayWidthForState(const OverlayState& state) {
    return state.mode == OverlayMode::Message ? kToastWidth : kPickerWidth;
}

LRESULT CALLBACK overlayWndProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
    switch (message) {
    case WM_MEDIA_OVERLAY_UPDATE:
    {
        OverlayState state;
        {
            std::lock_guard<std::mutex> lock(g_overlayMutex);
            state = g_overlayState;
        }
        const int width = overlayWidthForState(state);
        const int height = overlayHeightForState(state);
        const int x = GetSystemMetrics(SM_CXSCREEN) - width - 36;
        HRGN region = CreateRoundRectRgn(0, 0, width + 1, height + 1, 18, 18);
        SetWindowRgn(hwnd, region, TRUE);
        SetWindowPos(hwnd, HWND_TOPMOST, x, 72, width, height, SWP_NOACTIVATE | SWP_SHOWWINDOW);
        ShowWindow(hwnd, SW_SHOWNOACTIVATE);
        InvalidateRect(hwnd, nullptr, TRUE);
        SetTimer(hwnd, kOverlayTimer, static_cast<UINT>(wParam == 0 ? 5000 : wParam), nullptr);
        return 0;
    }
    case WM_MEDIA_OVERLAY_HIDE:
        ShowWindow(hwnd, SW_HIDE);
        KillTimer(hwnd, kOverlayTimer);
        return 0;
    case WM_TIMER:
        if (wParam == kOverlayTimer) {
            ShowWindow(hwnd, SW_HIDE);
            KillTimer(hwnd, kOverlayTimer);
        }
        return 0;
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(hwnd, &ps);

        RECT rect;
        GetClientRect(hwnd, &rect);
        OverlayState state;
        {
            std::lock_guard<std::mutex> lock(g_overlayMutex);
            state = g_overlayState;
        }
        paintOverlay(dc, rect, state);

        EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    default:
        return DefWindowProcW(hwnd, message, wParam, lParam);
    }
}

void overlayThreadMain() {
    g_overlayThreadId = GetCurrentThreadId();

    WNDCLASSW wc{};
    wc.lpfnWndProc = overlayWndProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"HotkeyToCommandMediaOverlay";
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    RegisterClassW(&wc);

    const int height = 68;
    const int x = GetSystemMetrics(SM_CXSCREEN) - kToastWidth - 36;
    const int y = 72;

    g_overlayWindow = CreateWindowExW(
        WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_LAYERED,
        wc.lpszClassName,
        L"Hotkey To Command Media Picker",
        WS_POPUP,
        x, y, kToastWidth, height,
        nullptr, nullptr, wc.hInstance, nullptr);

    if (g_overlayWindow) {
        SetLayeredWindowAttributes(g_overlayWindow, 0, 235, LWA_ALPHA);
    }

    if (g_overlayReady) SetEvent(g_overlayReady);

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
}

void ensureOverlayThread() {
    if (g_overlayWindow) return;

    if (!g_overlayReady) {
        g_overlayReady = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    }

    if (!g_overlayThread.joinable()) {
        g_overlayThread = std::thread(overlayThreadMain);
        g_overlayThread.detach();
    }

    WaitForSingleObject(g_overlayReady, 2000);
}

void showOverlayState(const OverlayState& state, DWORD milliseconds = 5000) {
    ensureOverlayThread();
    if (!g_overlayWindow) return;

    {
        std::lock_guard<std::mutex> lock(g_overlayMutex);
        g_overlayState = state;
    }
    PostMessageW(g_overlayWindow, WM_MEDIA_OVERLAY_UPDATE, milliseconds, 0);
}

void showOverlay(const std::wstring& text, DWORD milliseconds = 5000) {
    OverlayState state;
    state.mode = OverlayMode::Message;
    state.message = text;
    showOverlayState(state, milliseconds);
}

void showPickerOverlay(const std::vector<MediaTarget>& targets, size_t selected, DWORD milliseconds = 5000) {
    OverlayState state;
    state.mode = OverlayMode::Picker;
    state.targets = targets;
    state.selected = targets.empty() ? 0 : (std::min)(selected, targets.size() - 1);
    showOverlayState(state, milliseconds);
}

void showConfirmationOverlay(const MediaTarget& target, DWORD milliseconds = 1800) {
    OverlayState state;
    state.mode = OverlayMode::Confirmation;
    state.targets.push_back(target);
    showOverlayState(state, milliseconds);
}

void hideOverlay() {
    if (g_overlayWindow) PostMessageW(g_overlayWindow, WM_MEDIA_OVERLAY_HIDE, 0, 0);
}

std::vector<MediaTarget> queryWindowsMediaTargets(std::wstring* report) {
    std::vector<MediaTarget> targets;

    try {
        winrt::init_apartment(winrt::apartment_type::multi_threaded);
        auto manager = GlobalSystemMediaTransportControlsSessionManager::RequestAsync().get();
        auto sessions = manager.GetSessions();

        for (uint32_t i = 0; i < sessions.Size(); ++i) {
            auto session = sessions.GetAt(i);
            MediaTarget target;
            target.kind = L"windows";
            target.sessionIndex = i;
            target.appId = wideFromHstring(session.SourceAppUserModelId());
            target.id = L"windows:" + std::to_wstring(i) + L":" + target.appId;

            try {
                auto properties = session.TryGetMediaPropertiesAsync().get();
                target.title = wideFromHstring(properties.Title());
                target.artist = wideFromHstring(properties.Artist());
            } catch (...) {
            }

            try {
                auto playback = session.GetPlaybackInfo();
                auto status = playback.PlaybackStatus();
                auto controls = playback.Controls();
                target.playing = status == GlobalSystemMediaTransportControlsSessionPlaybackStatus::Playing;
                target.playbackStatus = statusName(status);
                target.canTogglePlayPause = controls.IsPlayEnabled() || controls.IsPauseEnabled();
                target.canNext = controls.IsNextEnabled();
                target.canPrevious = controls.IsPreviousEnabled();
            } catch (...) {
                target.playbackStatus = L"unknown";
            }

            targets.push_back(std::move(target));
        }

        std::stable_sort(targets.begin(), targets.end(), [](const MediaTarget& left, const MediaTarget& right) {
            if (left.playing != right.playing) return left.playing;
            return displayName(left) < displayName(right);
        });

        if (report) {
            *report = targets.empty()
                ? L"No Windows media sessions found."
                : L"Found " + std::to_wstring(targets.size()) + L" Windows media session(s).";
        }
    } catch (const winrt::hresult_error& ex) {
        if (report) *report = L"Windows media session query failed: " + std::wstring(ex.message().c_str());
    } catch (...) {
        if (report) *report = L"Windows media session query failed.";
    }

    return targets;
}

GlobalSystemMediaTransportControlsSession findSessionForTarget(
    const std::vector<MediaTarget>& targets,
    const std::wstring& appId,
    const std::wstring& title,
    unsigned int fallbackIndex,
    std::wstring* report) {
    winrt::init_apartment(winrt::apartment_type::multi_threaded);
    auto manager = GlobalSystemMediaTransportControlsSessionManager::RequestAsync().get();
    auto sessions = manager.GetSessions();

    for (const MediaTarget& target : targets) {
        if (target.appId == appId && (title.empty() || target.title == title)) {
            if (target.sessionIndex < sessions.Size()) return sessions.GetAt(target.sessionIndex);
        }
    }

    for (const MediaTarget& target : targets) {
        if (target.appId == appId && target.sessionIndex < sessions.Size()) {
            return sessions.GetAt(target.sessionIndex);
        }
    }

    if (fallbackIndex < sessions.Size()) return sessions.GetAt(fallbackIndex);

    if (report) *report = L"The selected media session disappeared.";
    return nullptr;
}

bool runMediaCommand(const GlobalSystemMediaTransportControlsSession& session, MediaCommand command, std::wstring* report) {
    if (!session) {
        if (report) *report = L"No media session selected.";
        return false;
    }

    bool ok = false;
    switch (command) {
    case MediaCommand::TogglePlayPause:
        ok = session.TryTogglePlayPauseAsync().get();
        break;
    case MediaCommand::Next:
        ok = session.TrySkipNextAsync().get();
        break;
    case MediaCommand::Previous:
        ok = session.TrySkipPreviousAsync().get();
        break;
    }

    if (report) *report = ok ? L"Media command sent." : L"Media session rejected the command.";
    return ok;
}

void chooseInitialPickerIndex() {
    g_pickerIndex = 0;
    if (g_pickerTargets.empty()) return;

    if (g_hasSelectedTarget) {
        for (size_t i = 0; i < g_pickerTargets.size(); ++i) {
            if (g_pickerTargets[i].appId == g_selectedAppId) {
                g_pickerIndex = i;
                return;
            }
        }
    }

    for (size_t i = 0; i < g_pickerTargets.size(); ++i) {
        if (g_pickerTargets[i].playing) {
            g_pickerIndex = i;
            return;
        }
    }
}

bool selectPickerTarget(std::wstring* report) {
    if (g_pickerTargets.empty()) {
        if (report) *report = L"No media targets to select.";
        return false;
    }

    const MediaTarget& selected = g_pickerTargets[g_pickerIndex];
    g_selectedId = selected.id;
    g_selectedKind = selected.kind;
    g_selectedAppId = selected.appId;
    g_selectedTitle = selected.title;
    g_selectedSessionIndex = selected.sessionIndex;
    g_hasSelectedTarget = true;
    g_pickerOpen = false;

    std::wstring text = L"Selected: " + shortAppName(selected.appId) + L" - " + displayName(selected);
    if (report) *report = text;
    showConfirmationOverlay(selected);
    return true;
}

bool selectedTargetExists(std::wstring* report) {
    std::lock_guard<std::mutex> lock(g_pickerMutex);
    if (g_hasSelectedTarget) return true;
    if (report) *report = L"No media target selected. Open the media picker first.";
    return false;
}

} // namespace

std::vector<MediaTarget> listMediaTargets(std::wstring* report) {
    std::wstring windowsReport;
    std::wstring browserReport;
    std::vector<MediaTarget> targets = queryWindowsMediaTargets(&windowsReport);
    std::vector<MediaTarget> browserTargets = cachedBrowserMediaTargets(&browserReport);
    size_t hiddenBrowserAppSessions = 0;

    const size_t before = targets.size();
    targets.erase(std::remove_if(targets.begin(), targets.end(), isBrowserWindowsSession), targets.end());
    hiddenBrowserAppSessions = before - targets.size();

    targets.insert(targets.end(), browserTargets.begin(), browserTargets.end());
    std::stable_sort(targets.begin(), targets.end(), [](const MediaTarget& left, const MediaTarget& right) {
        if (left.playing != right.playing) return left.playing;
        if (left.kind != right.kind) return left.kind < right.kind;
        return displayName(left) < displayName(right);
    });

    if (report) {
        *report = L"Media targets: " + std::to_wstring(targets.size()) + L"\n"
            + windowsReport + L"\n" + browserReport;
        if (hiddenBrowserAppSessions > 0) {
            *report += L"\nhid " + std::to_wstring(hiddenBrowserAppSessions)
                + L" duplicate browser app media session"
                + (hiddenBrowserAppSessions == 1 ? L"" : L"s");
        }
    }
    return targets;
}

bool openMediaPicker(std::wstring* report) {
    std::wstring queryReport;
    std::vector<MediaTarget> targets = listMediaTargets(&queryReport);

    std::lock_guard<std::mutex> lock(g_pickerMutex);
    g_pickerTargets = std::move(targets);
    g_pickerOpen = true;
    chooseInitialPickerIndex();

    showPickerOverlay(g_pickerTargets, g_pickerIndex);
    if (report) *report = queryReport;
    return !g_pickerTargets.empty();
}

bool moveMediaPicker(int direction, std::wstring* report) {
    std::lock_guard<std::mutex> lock(g_pickerMutex);
    if (!g_pickerOpen) {
        g_pickerTargets = listMediaTargets(report);
        g_pickerOpen = true;
        chooseInitialPickerIndex();
    }

    if (g_pickerTargets.empty()) {
        if (report) *report = L"No media targets to move through.";
        showPickerOverlay(g_pickerTargets, 0);
        return false;
    }

    const size_t count = g_pickerTargets.size();
    if (direction >= 0) {
        g_pickerIndex = (g_pickerIndex + 1) % count;
    } else {
        g_pickerIndex = (g_pickerIndex + count - 1) % count;
    }

    const MediaTarget& selected = g_pickerTargets[g_pickerIndex];
    if (report) *report = L"Picker target: " + displayName(selected);
    showPickerOverlay(g_pickerTargets, g_pickerIndex);
    return true;
}

bool confirmMediaPicker(std::wstring* report) {
    std::lock_guard<std::mutex> lock(g_pickerMutex);
    if (!g_pickerOpen) {
        if (report) *report = L"Media picker is not open.";
        return false;
    }
    return selectPickerTarget(report);
}

bool cancelMediaPicker(std::wstring* report) {
    std::lock_guard<std::mutex> lock(g_pickerMutex);
    g_pickerOpen = false;
    hideOverlay();
    if (report) *report = L"Media picker cancelled.";
    return true;
}

bool mediaPickerIsOpen() {
    std::lock_guard<std::mutex> lock(g_pickerMutex);
    return g_pickerOpen;
}

bool controlSelectedMedia(MediaCommand command, std::wstring* report) {
    std::wstring id;
    std::wstring kind;
    std::wstring appId;
    std::wstring title;
    unsigned int fallbackIndex = 0;

    {
        std::lock_guard<std::mutex> lock(g_pickerMutex);
        if (!g_hasSelectedTarget) {
            g_pickerTargets = listMediaTargets(report);
            chooseInitialPickerIndex();
            if (!selectPickerTarget(report)) return false;
        }

        id = g_selectedId;
        kind = g_selectedKind;
        appId = g_selectedAppId;
        title = g_selectedTitle;
        fallbackIndex = g_selectedSessionIndex;
    }

    try {
        if (kind == L"browser") {
            bool ok = controlBrowserMediaTarget(id, command, report);
            if (ok) showOverlay(report && !report->empty() ? *report : commandToastText(command), 950);
            else if (report && !report->empty()) showOverlay(*report, 1600);
            return ok;
        }

        std::wstring queryReport;
        std::vector<MediaTarget> targets = queryWindowsMediaTargets(&queryReport);
        auto session = findSessionForTarget(targets, appId, title, fallbackIndex, report);
        bool ok = runMediaCommand(session, command, report);
        if (ok) showOverlay(commandToastText(command), 950);
        else if (report && !report->empty()) showOverlay(*report, 1600);
        return ok;
    } catch (const winrt::hresult_error& ex) {
        if (report) *report = L"Media command failed: " + std::wstring(ex.message().c_str());
    } catch (...) {
        if (report) *report = L"Media command failed.";
    }

    return false;
}

bool mediaNextContextual(std::wstring* report) {
    if (mediaPickerIsOpen()) return moveMediaPicker(1, report);
    if (!selectedTargetExists(report)) return false;
    return controlSelectedMedia(MediaCommand::Next, report);
}

bool mediaPreviousContextual(std::wstring* report) {
    if (mediaPickerIsOpen()) return moveMediaPicker(-1, report);
    if (!selectedTargetExists(report)) return false;
    return controlSelectedMedia(MediaCommand::Previous, report);
}

bool mediaPlayPauseContextual(std::wstring* report) {
    return mediaPickerIsOpen() ? confirmMediaPicker(report) : controlSelectedMedia(MediaCommand::TogglePlayPause, report);
}

std::wstring describeMediaTargets() {
    std::wstring report;
    std::vector<MediaTarget> targets = listMediaTargets(&report);
    return overlayTextForTargets(targets, 0, false);
}
