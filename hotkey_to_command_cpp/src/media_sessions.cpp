#include "media_sessions.h"

#include "browser_bridge.h"

#include <windows.h>
#include <wincodec.h>
#include <objidl.h>

// gdiplus.h uses unqualified min/max; the project builds with NOMINMAX, so make
// std::min/std::max visible at namespace scope before including it.
#include <algorithm>
using std::min;
using std::max;
#include <gdiplus.h>

#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Media.Control.h>
#include <winrt/Windows.Storage.Streams.h>
#include <winrt/base.h>

#include <algorithm>
#include <cwctype>
#include <cstdint>
#include <cstring>
#include <functional>
#include <mutex>
#include <string_view>
#include <sstream>
#include <thread>

using winrt::Windows::Media::Control::GlobalSystemMediaTransportControlsSession;
using winrt::Windows::Media::Control::GlobalSystemMediaTransportControlsSessionManager;
using winrt::Windows::Media::Control::GlobalSystemMediaTransportControlsSessionPlaybackStatus;
using winrt::Windows::Storage::Streams::DataReader;

namespace {

constexpr UINT WM_MEDIA_OVERLAY_UPDATE = WM_APP + 40;
constexpr UINT WM_MEDIA_OVERLAY_HIDE = WM_APP + 41;
constexpr UINT_PTR kOverlayTimer = 1;
constexpr int kPickerWidth = 612;
constexpr int kToastWidth = 460;
constexpr int kOverlayMargin = 36;
constexpr int kOverlayTopMargin = 72;

ULONG_PTR g_gdiplusToken = 0;

void ensureGdiplus() {
    if (g_gdiplusToken) return;
    Gdiplus::GdiplusStartupInput input;
    Gdiplus::GdiplusStartup(&g_gdiplusToken, &input, nullptr);
}

Gdiplus::Color gpColor(COLORREF c, BYTE a = 255) {
    return Gdiplus::Color(a, GetRValue(c), GetGValue(c), GetBValue(c));
}

constexpr COLORREF kAccent = RGB(92, 142, 255);
constexpr COLORREF kPanel = RGB(29, 31, 39);
constexpr COLORREF kPanelBorder = RGB(58, 61, 72);

std::string wideToNarrowAscii(const std::wstring& value) {
    std::string out;
    out.reserve(value.size());
    for (wchar_t c : value) {
        out.push_back(c <= 0x7f ? static_cast<char>(c) : '?');
    }
    return out;
}

int base64Value(char c) {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1;
}

bool decodeBase64(std::string_view input, std::vector<uint8_t>& out) {
    out.clear();
    int value = 0;
    int bits = -8;
    for (char c : input) {
        if (c == '=') break;
        if (c == '\r' || c == '\n' || c == ' ' || c == '\t') continue;
        const int decoded = base64Value(c);
        if (decoded < 0) return false;
        value = (value << 6) + decoded;
        bits += 6;
        if (bits >= 0) {
            out.push_back(static_cast<uint8_t>((value >> bits) & 0xff));
            bits -= 8;
        }
    }
    return !out.empty();
}

bool decodeDataUrlBytes(const std::wstring& url, std::vector<uint8_t>& bytes) {
    const std::string text = wideToNarrowAscii(url);
    if (text.rfind("data:", 0) != 0) return false;
    const size_t comma = text.find(',');
    if (comma == std::string::npos) return false;
    const std::string header = text.substr(0, comma);
    if (header.find(";base64") == std::string::npos) return false;
    return decodeBase64(std::string_view(text).substr(comma + 1), bytes);
}

void decodeImageBytes(const std::vector<uint8_t>& bytes, std::vector<uint8_t>& bgra, int& width, int& height) {
    if (bytes.empty() || bytes.size() > 4 * 1024 * 1024) return;

    HGLOBAL hg = GlobalAlloc(GMEM_MOVEABLE, bytes.size());
    if (!hg) return;
    void* ptr = GlobalLock(hg);
    if (!ptr) { GlobalFree(hg); return; }
    memcpy(ptr, bytes.data(), bytes.size());
    GlobalUnlock(hg);

    IStream* pStream = nullptr;
    if (FAILED(CreateStreamOnHGlobal(hg, TRUE, &pStream)) || !pStream) {
        GlobalFree(hg);
        return;
    }

    IWICImagingFactory* pFactory = nullptr;
    CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                     IID_IWICImagingFactory, reinterpret_cast<void**>(&pFactory));
    if (!pFactory) { pStream->Release(); return; }

    IWICBitmapDecoder* pDecoder = nullptr;
    pFactory->CreateDecoderFromStream(pStream, nullptr, WICDecodeMetadataCacheOnLoad, &pDecoder);
    pStream->Release();

    if (pDecoder) {
        IWICBitmapFrameDecode* pFrame = nullptr;
        pDecoder->GetFrame(0, &pFrame);
        pDecoder->Release();

        if (pFrame) {
            IWICFormatConverter* pConv = nullptr;
            pFactory->CreateFormatConverter(&pConv);
            if (pConv) {
                pConv->Initialize(pFrame, GUID_WICPixelFormat32bppBGRA,
                    WICBitmapDitherTypeNone, nullptr, 0.0f, WICBitmapPaletteTypeCustom);
                UINT w = 0, h = 0;
                pConv->GetSize(&w, &h);
                if (w > 0 && h > 0 && w <= 2048 && h <= 2048) {
                    width = static_cast<int>(w);
                    height = static_cast<int>(h);
                    bgra.resize(static_cast<size_t>(w) * h * 4);
                    pConv->CopyPixels(nullptr, w * 4,
                        static_cast<UINT>(bgra.size()),
                        bgra.data());
                }
                pConv->Release();
            }
            pFrame->Release();
        }
    }
    pFactory->Release();
}

void decodeImageBytesIntoArtwork(const std::vector<uint8_t>& bytes, MediaTarget& target) {
    decodeImageBytes(bytes, target.artworkBgra, target.artworkWidth, target.artworkHeight);
}

void decodeImageBytesIntoFavicon(const std::vector<uint8_t>& bytes, MediaTarget& target) {
    decodeImageBytes(bytes, target.favIconBgra, target.favIconWidth, target.favIconHeight);
}

// Build a rounded-rectangle path. Inset by 0.5px so a 1px stroke lands crisply.
void addRoundedRect(Gdiplus::GraphicsPath& path, Gdiplus::RectF r, float radius) {
    const float d = radius * 2.0f;
    path.Reset();
    if (radius <= 0.0f) {
        path.AddRectangle(r);
        return;
    }
    path.AddArc(r.X, r.Y, d, d, 180, 90);
    path.AddArc(r.X + r.Width - d, r.Y, d, d, 270, 90);
    path.AddArc(r.X + r.Width - d, r.Y + r.Height - d, d, d, 0, 90);
    path.AddArc(r.X, r.Y + r.Height - d, d, d, 90, 90);
    path.CloseFigure();
}

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

void fetchSmtcThumbnail(const GlobalSystemMediaTransportControlsSession& session, MediaTarget& target) {
    try {
        auto props = session.TryGetMediaPropertiesAsync().get();
        auto thumbRef = props.Thumbnail();
        if (!thumbRef) return;

        auto stream = thumbRef.OpenReadAsync().get();
        if (!stream) return;

        const uint64_t size = stream.Size();
        if (size == 0 || size > 4 * 1024 * 1024) return;

        DataReader reader(stream);
        reader.LoadAsync(static_cast<uint32_t>(size)).get();
        std::vector<uint8_t> bytes(static_cast<size_t>(size));
        reader.ReadBytes(winrt::array_view<uint8_t>(bytes));
        reader.DetachStream();
        decodeImageBytesIntoArtwork(bytes, target);
    } catch (...) {
        // Leave artworkBgra empty on any failure.
    }
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

void replaceBrowserAppSessionsWhenTabsExist(
    std::vector<MediaTarget>& targets,
    const std::vector<MediaTarget>& browserTargets,
    size_t* hiddenBrowserAppSessions) {
    if (hiddenBrowserAppSessions) *hiddenBrowserAppSessions = 0;
    if (browserTargets.empty()) return;

    const size_t before = targets.size();
    targets.erase(std::remove_if(targets.begin(), targets.end(), isBrowserWindowsSession), targets.end());
    if (hiddenBrowserAppSessions) *hiddenBrowserAppSessions = before - targets.size();
}

void hydrateBrowserArtwork(std::vector<MediaTarget>& targets) {
    for (MediaTarget& target : targets) {
        if (target.artworkBgra.empty() && !target.artworkUrl.empty()) {
            std::vector<uint8_t> bytes;
            if (decodeDataUrlBytes(target.artworkUrl, bytes)) {
                decodeImageBytesIntoArtwork(bytes, target);
            }
        }
        if (target.favIconBgra.empty() && !target.favIconUrl.empty()) {
            std::vector<uint8_t> iconBytes;
            if (decodeDataUrlBytes(target.favIconUrl, iconBytes)) {
                decodeImageBytesIntoFavicon(iconBytes, target);
            }
        }
    }
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
        return L"Play/pause sent";
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
    Gdiplus::Graphics g(dc);
    g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
    g.SetPixelOffsetMode(Gdiplus::PixelOffsetModeHalf);

    const bool hasBorder = border != static_cast<COLORREF>(-1);
    // Inset by 0.5px so the 1px stroke is fully inside the fill (crisp edge).
    const float inset = hasBorder ? 0.5f : 0.0f;
    Gdiplus::RectF r(
        static_cast<Gdiplus::REAL>(rect.left) + inset,
        static_cast<Gdiplus::REAL>(rect.top) + inset,
        static_cast<Gdiplus::REAL>(rect.right - rect.left) - inset * 2.0f - (hasBorder ? 1.0f : 0.0f),
        static_cast<Gdiplus::REAL>(rect.bottom - rect.top) - inset * 2.0f - (hasBorder ? 1.0f : 0.0f));

    Gdiplus::GraphicsPath path;
    addRoundedRect(path, r, static_cast<float>(radius));

    Gdiplus::SolidBrush brush(gpColor(fill));
    g.FillPath(&brush, &path);
    if (hasBorder) {
        Gdiplus::Pen pen(gpColor(border), 1.0f);
        g.DrawPath(&pen, &path);
    }
}

void fillRectColor(HDC dc, const RECT& rect, COLORREF color) {
    HBRUSH brush = CreateSolidBrush(color);
    FillRect(dc, &rect, brush);
    DeleteObject(brush);
}

void drawOverlayShell(HDC dc, const RECT& panel, COLORREF accent = kAccent) {
    Gdiplus::Graphics g(dc);
    g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
    g.SetPixelOffsetMode(Gdiplus::PixelOffsetModeHalf);

    Gdiplus::RectF edge(
        static_cast<float>(panel.left) + 0.5f,
        static_cast<float>(panel.top) + 0.5f,
        static_cast<float>(panel.right - panel.left) - 1.0f,
        static_cast<float>(panel.bottom - panel.top) - 1.0f);
    Gdiplus::GraphicsPath path;
    addRoundedRect(path, edge, 17.0f);

    Gdiplus::Pen glow(gpColor(accent, 110), 1.4f);
    g.DrawPath(&glow, &path);
}

void drawTextLine(HDC dc, const std::wstring& text, RECT rect, HFONT font, COLORREF color, UINT align = DT_LEFT) {
    HGDIOBJ oldFont = SelectObject(dc, font);
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, color);
    DrawTextW(dc, text.c_str(), -1, &rect, align | DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS | DT_NOPREFIX);
    SelectObject(dc, oldFont);
}

// Draw a raised keycap pill background at the given rect (antialiased, top-light gradient).
void drawKeycap(HDC dc, const RECT& r) {
    Gdiplus::Graphics g(dc);
    g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
    g.SetPixelOffsetMode(Gdiplus::PixelOffsetModeHalf);

    const float radius = 6.0f;

    // Soft bottom shadow.
    Gdiplus::RectF shadow(static_cast<float>(r.left) + 0.5f, static_cast<float>(r.top) + 1.0f,
                          static_cast<float>(r.right - r.left) - 1.0f, static_cast<float>(r.bottom - r.top));
    Gdiplus::GraphicsPath shadowPath;
    addRoundedRect(shadowPath, shadow, radius);
    Gdiplus::SolidBrush shadowBrush(gpColor(RGB(8, 10, 16), 55));
    g.FillPath(&shadowBrush, &shadowPath);

    // Body with vertical top-light gradient.
    Gdiplus::RectF body(static_cast<float>(r.left) + 0.5f, static_cast<float>(r.top) + 0.5f,
                        static_cast<float>(r.right - r.left) - 1.0f, static_cast<float>(r.bottom - r.top) - 1.0f);
    Gdiplus::GraphicsPath bodyPath;
    addRoundedRect(bodyPath, body, radius);
    Gdiplus::LinearGradientBrush grad(
        Gdiplus::PointF(body.X, body.Y),
        Gdiplus::PointF(body.X, body.Y + body.Height),
        gpColor(RGB(58, 63, 82)), gpColor(RGB(38, 42, 58)));
    g.FillPath(&grad, &bodyPath);

    // Border: lighter top, darker bottom — draw full border then overlay a darker bottom arc.
    Gdiplus::Pen border(gpColor(RGB(73, 79, 104)), 1.0f);
    g.DrawPath(&border, &bodyPath);
}

// Glyph: two facing triangles (◀ ▶) representing prev/next navigation.
void drawGlyphNavPair(HDC dc, const RECT& r, COLORREF color) {
    Gdiplus::Graphics g(dc);
    g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
    Gdiplus::SolidBrush b(gpColor(color));
    const float cx = (r.left + r.right) / 2.0f;
    const float cy = (r.top + r.bottom) / 2.0f;
    Gdiplus::PointF leftTri[]{{cx - 8, cy}, {cx - 2, cy - 5}, {cx - 2, cy + 5}};
    g.FillPolygon(&b, leftTri, 3);
    Gdiplus::PointF rightTri[]{{cx + 8, cy}, {cx + 2, cy - 5}, {cx + 2, cy + 5}};
    g.FillPolygon(&b, rightTri, 3);
}

// Glyph: play triangle + pause bars (▶⏸).
void drawGlyphPlayPause(HDC dc, const RECT& r, COLORREF color) {
    Gdiplus::Graphics g(dc);
    g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
    g.SetPixelOffsetMode(Gdiplus::PixelOffsetModeHalf);
    Gdiplus::SolidBrush b(gpColor(color));
    const float cx = (r.left + r.right) / 2.0f;
    const float cy = (r.top + r.bottom) / 2.0f;
    Gdiplus::PointF tri[]{{cx - 7, cy - 5}, {cx - 7, cy + 5}, {cx - 1, cy}};
    g.FillPolygon(&b, tri, 3);
    g.FillRectangle(&b, Gdiplus::RectF(cx + 1.0f, cy - 5.0f, 2.0f, 10.0f));
    g.FillRectangle(&b, Gdiplus::RectF(cx + 5.0f, cy - 5.0f, 2.0f, 10.0f));
}

// Neutral artwork placeholder: simple music note (antialiased).
void drawMusicNote(HDC dc, const RECT& art, COLORREF color) {
    Gdiplus::Graphics g(dc);
    g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
    g.SetPixelOffsetMode(Gdiplus::PixelOffsetModeHalf);

    const float cx = (art.left + art.right) / 2.0f;
    const float cy = (art.top + art.bottom) / 2.0f;
    Gdiplus::SolidBrush b(gpColor(color));
    Gdiplus::Pen p(gpColor(color), 2.0f);
    p.SetStartCap(Gdiplus::LineCapRound);
    p.SetEndCap(Gdiplus::LineCapRound);
    p.SetLineJoin(Gdiplus::LineJoinRound);

    // Stem
    g.FillRectangle(&b, Gdiplus::RectF(cx, cy - 9.0f, 2.0f, 11.0f));
    // Flag
    Gdiplus::PointF flag[]{{cx + 2, cy - 9}, {cx + 8, cy - 6}, {cx + 8, cy - 3}};
    g.DrawLines(&p, flag, 3);
    // Note head
    g.FillEllipse(&b, Gdiplus::RectF(cx - 5.0f, cy, 7.0f, 5.0f));
}

enum class ShortcutGlyph { None, NavPair, PlayPause };

void drawShortcut(HDC dc, int x, int y, ShortcutGlyph glyph, const std::wstring& keyText,
                  const std::wstring& label, HFONT keyFont, HFONT labelFont) {
    const int keyH = 22;
    const int keyWidth = glyph != ShortcutGlyph::None ? 44
                       : keyText.size() <= 3          ? 40
                                                      : 54;
    RECT keyRect{x, y + 1, x + keyWidth, y + 1 + keyH};
    drawKeycap(dc, keyRect);

    constexpr COLORREF glyphColor = RGB(210, 216, 238);
    if (glyph == ShortcutGlyph::NavPair) {
        drawGlyphNavPair(dc, keyRect, glyphColor);
    } else if (glyph == ShortcutGlyph::PlayPause) {
        drawGlyphPlayPause(dc, keyRect, glyphColor);
    } else {
        drawTextLine(dc, keyText, keyRect, keyFont, RGB(228, 234, 250), DT_CENTER);
    }

    RECT labelRect{keyRect.right + 8, y, keyRect.right + 160, y + keyH + 2};
    drawTextLine(dc, label, labelRect, labelFont, RGB(144, 149, 166));
}

void drawPlaybackBadge(HDC dc, const RECT& art, bool playing) {
    Gdiplus::Graphics g(dc);
    g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
    g.SetPixelOffsetMode(Gdiplus::PixelOffsetModeHalf);

    const float radius = 9.0f;
    const float cx = art.right - 5.0f;
    const float cy = art.bottom - 5.0f;

    // Dark ring behind the badge so it reads against any artwork.
    Gdiplus::SolidBrush ring(gpColor(RGB(14, 16, 23)));
    g.FillEllipse(&ring, cx - radius - 1.5f, cy - radius - 1.5f, (radius + 1.5f) * 2.0f, (radius + 1.5f) * 2.0f);

    Gdiplus::SolidBrush body(gpColor(playing ? kAccent : RGB(78, 82, 94)));
    g.FillEllipse(&body, cx - radius, cy - radius, radius * 2.0f, radius * 2.0f);

    Gdiplus::SolidBrush mark(gpColor(playing ? RGB(12, 16, 28) : RGB(232, 236, 245)));
    if (playing) {
        Gdiplus::PointF tri[]{{cx - 2.5f, cy - 4.0f}, {cx - 2.5f, cy + 4.0f}, {cx + 4.0f, cy}};
        g.FillPolygon(&mark, tri, 3);
    } else {
        g.FillRectangle(&mark, Gdiplus::RectF(cx - 3.5f, cy - 4.0f, 2.5f, 8.0f));
        g.FillRectangle(&mark, Gdiplus::RectF(cx + 1.0f, cy - 4.0f, 2.5f, 8.0f));
    }
}

// Filled accent circle with a centered checkmark (antialiased, round caps).
void drawCheckBadge(HDC dc, const RECT& circle, COLORREF fill, COLORREF tick) {
    Gdiplus::Graphics g(dc);
    g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
    g.SetPixelOffsetMode(Gdiplus::PixelOffsetModeHalf);

    Gdiplus::SolidBrush brush(gpColor(fill));
    g.FillEllipse(&brush, static_cast<float>(circle.left), static_cast<float>(circle.top),
                  static_cast<float>(circle.right - circle.left), static_cast<float>(circle.bottom - circle.top));

    const float cx = (circle.left + circle.right) / 2.0f;
    const float cy = (circle.top + circle.bottom) / 2.0f;
    const float s = (circle.right - circle.left) / 18.0f; // scale relative to ~18px circle
    Gdiplus::Pen pen(gpColor(tick), 2.0f * s);
    pen.SetStartCap(Gdiplus::LineCapRound);
    pen.SetEndCap(Gdiplus::LineCapRound);
    pen.SetLineJoin(Gdiplus::LineJoinRound);
    Gdiplus::PointF check[]{
        {cx - 4.0f * s, cy + 0.3f * s},
        {cx - 1.2f * s, cy + 3.2f * s},
        {cx + 4.2f * s, cy - 3.0f * s}};
    g.DrawLines(&pen, check, 3);
}

void drawBgraImage(HDC dc, const RECT& rect, const std::vector<uint8_t>& bgra, int width, int height, float radius);

void drawArtwork(HDC dc, const RECT& art, const MediaTarget& target, bool selected) {
    fillRoundRect(dc, art, 9, artworkColor(target), selected ? kAccent : RGB(46, 49, 58));

    if (!target.artworkBgra.empty() && target.artworkWidth > 0 && target.artworkHeight > 0) {
        drawBgraImage(dc, art, target.artworkBgra, target.artworkWidth, target.artworkHeight, 9.0f);
    } else {
        // Neutral source glyph — no play/pause indicator on the thumbnail.
        drawMusicNote(dc, art, RGB(96, 102, 124));
    }

    drawPlaybackBadge(dc, art, target.playing);
}

void drawEqualizer(HDC dc, int x, int y, COLORREF color) {
    Gdiplus::Graphics g(dc);
    g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
    g.SetPixelOffsetMode(Gdiplus::PixelOffsetModeHalf);
    Gdiplus::SolidBrush brush(gpColor(color));
    const int heights[] = {13, 23, 17};
    for (int i = 0; i < 3; ++i) {
        Gdiplus::RectF bar(static_cast<float>(x + i * 5), static_cast<float>(y + 24 - heights[i]),
                           3.0f, static_cast<float>(heights[i]));
        Gdiplus::GraphicsPath path;
        addRoundedRect(path, bar, 1.5f);
        g.FillPath(&brush, &path);
    }
}

SIZE measureText(HDC dc, const std::wstring& text, HFONT font) {
    HGDIOBJ oldFont = SelectObject(dc, font);
    SIZE sz{};
    GetTextExtentPoint32W(dc, text.c_str(), static_cast<int>(text.size()), &sz);
    SelectObject(dc, oldFont);
    return sz;
}

void drawBgraImage(HDC dc, const RECT& rect, const std::vector<uint8_t>& bgra, int width, int height, float radius) {
    if (bgra.empty() || width <= 0 || height <= 0) return;

    Gdiplus::Graphics g(dc);
    g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
    g.SetInterpolationMode(Gdiplus::InterpolationModeHighQualityBicubic);
    g.SetPixelOffsetMode(Gdiplus::PixelOffsetModeHalf);

    Gdiplus::RectF clipRect(static_cast<float>(rect.left), static_cast<float>(rect.top),
                            static_cast<float>(rect.right - rect.left), static_cast<float>(rect.bottom - rect.top));
    Gdiplus::GraphicsPath clip;
    addRoundedRect(clip, clipRect, radius);
    g.SetClip(&clip);

    Gdiplus::Bitmap bmp(width, height, width * 4, PixelFormat32bppARGB, const_cast<BYTE*>(bgra.data()));
    g.DrawImage(&bmp, clipRect, 0.0f, 0.0f,
                static_cast<float>(width), static_cast<float>(height), Gdiplus::UnitPixel);
    g.ResetClip();
}

void drawSourceChip(HDC dc, int x, int y, const MediaTarget& target, HFONT font) {
    RECT favicon{x, y + 3, x + 13, y + 16};
    fillRoundRect(dc, favicon, 3, RGB(43, 47, 58), RGB(58, 62, 78));
    drawBgraImage(dc, favicon, target.favIconBgra, target.favIconWidth, target.favIconHeight, 3.0f);

    // Host/app name in muted gray — measure it so the tag sits right beside it.
    const std::wstring name = sourceName(target);
    const int nameX = x + 20;
    const int nameMaxRight = x + 188;
    SIZE nameSize = measureText(dc, name, font);
    int nameRight = (std::min)(nameX + static_cast<int>(nameSize.cx), nameMaxRight);
    RECT hostRect{nameX, y - 1, nameRight, y + 18};
    drawTextLine(dc, name, hostRect, font, RGB(119, 124, 140));
    if (target.kind != L"browser") return;

    // Lowercase "tab" or "app" tag — subdued, placed immediately after the name.
    const std::wstring tag = target.kind == L"browser" ? L"tab" : L"app";
    SIZE tagSize = measureText(dc, tag, font);
    const int chipPadX = 6;
    const int chipLeft = nameRight + 7;
    const int chipRight = chipLeft + static_cast<int>(tagSize.cx) + chipPadX * 2;
    RECT chip{chipLeft, y + 1, chipRight, y + 17};
    fillRoundRect(dc, chip, 4, RGB(35, 37, 46), RGB(47, 50, 61));
    drawTextLine(dc, tag, chip, font, RGB(102, 107, 124), DT_CENTER);
}

size_t firstVisibleIndex(size_t count, size_t selected, size_t visibleRows) {
    if (count <= visibleRows || selected < visibleRows) return 0;
    size_t first = selected - visibleRows + 1;
    return (std::min)(first, count - visibleRows);
}

void drawMediaRow(HDC dc, const RECT& row, const MediaTarget& target, bool selected, HFONT titleFont, HFONT bodyFont, HFONT metaFont) {
    if (selected) {
        fillRoundRect(dc, row, 10, RGB(42, 55, 82), kAccent);
        RECT edge{row.left, row.top + 8, row.left + 4, row.bottom - 8};
        fillRoundRect(dc, edge, 4, kAccent);
    }

    const int rowHeight = row.bottom - row.top;
    const int artSize = rowHeight >= 88 ? 58 : 46;
    const int artTop = row.top + (rowHeight - artSize) / 2;
    RECT art{row.left + 18, artTop, row.left + 18 + artSize, artTop + artSize};
    drawArtwork(dc, art, target, selected);

    const int textTop = art.top + (artSize >= 58 ? 0 : -1);
    RECT titleRect{art.right + 14, textTop, row.right - 52, textTop + 22};
    drawTextLine(dc, mediaTitle(target), titleRect, titleFont, RGB(240, 242, 248));

    RECT subtitleRect{titleRect.left, titleRect.bottom, row.right - 52, titleRect.bottom + 19};
    drawTextLine(dc, mediaSubtitle(target), subtitleRect, bodyFont, RGB(164, 168, 181));

    drawSourceChip(dc, titleRect.left, subtitleRect.bottom - 1, target, metaFont);

    if (target.playing) {
        drawEqualizer(dc, row.right - 30, row.top + 24, kAccent);
    }
}

void drawFooter(HDC dc, const RECT& footer, bool picker, bool hasControlledTarget, HFONT keyFont, HFONT labelFont) {
    RECT line{footer.left, footer.top, footer.right, footer.top + 1};
    fillRectColor(dc, line, RGB(43, 46, 56));

    int x = footer.left + 16;
    const int y = footer.top + 12;
    if (picker) {
        drawShortcut(dc, x, y, ShortcutGlyph::NavPair, L"", L"Move", keyFont, labelFont);
        x += 132;
        drawShortcut(dc, x, y, ShortcutGlyph::PlayPause, L"", L"Select", keyFont, labelFont);
        x += 138;
    } else if (hasControlledTarget) {
        drawShortcut(dc, x, y, ShortcutGlyph::PlayPause, L"", L"Play/pause", keyFont, labelFont);
        x += 156;
    }
    drawShortcut(dc, x, y, ShortcutGlyph::None, L"Esc", L"Dismiss", keyFont, labelFont);
}

void paintPickerOverlay(HDC dc, const RECT& bounds, const OverlayState& state, HFONT titleFont, HFONT bodyFont, HFONT metaFont, HFONT keyFont) {
    RECT panel{0, 0, bounds.right, bounds.bottom};
    fillRoundRect(dc, panel, 18, kPanel);
    drawOverlayShell(dc, panel);

    RECT accent{panel.left + 16, panel.top + 17, panel.left + 19, panel.top + 31};
    fillRoundRect(dc, accent, 2, state.targets.empty() ? RGB(81, 85, 98) : kAccent);

    RECT headerTitle{panel.left + 30, panel.top + 8, panel.right - 100, panel.top + 42};
    drawTextLine(dc, L"Choose media target", headerTitle, titleFont, RGB(244, 246, 250));

    size_t browserCount = 0;
    for (const MediaTarget& target : state.targets) {
        if (target.kind == L"browser") ++browserCount;
    }
    const size_t total = state.targets.size();
    const bool allBrowser = (browserCount == total);
    const std::wstring count = std::to_wstring(total)
        + (allBrowser
            ? (total == 1 ? L" tab"    : L" tabs")
            : (total == 1 ? L" target" : L" targets"));
    RECT countRect{panel.right - 92, panel.top + 8, panel.right - 18, panel.top + 42};
    drawTextLine(dc, count, countRect, metaFont, RGB(146, 150, 162), DT_RIGHT);

    RECT footer{panel.left + 2, panel.bottom - 48, panel.right - 2, panel.bottom - 2};
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
    const int availableRows = (std::max)(1, static_cast<int>((content.bottom - content.top + rowHeight - 1) / rowHeight));
    const size_t visibleRows = (std::min)(state.targets.size(), static_cast<size_t>(availableRows));
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
    fillRoundRect(dc, panel, 18, kPanel);
    drawOverlayShell(dc, panel);

    RECT banner{panel.left + 2, panel.top + 2, panel.right - 2, panel.top + 48};
    fillRoundRect(dc, banner, 16, RGB(42, 55, 82));

    // Accent check circle with a centered white tick (antialiased).
    RECT check{banner.left + 18, banner.top + 15, banner.left + 36, banner.top + 33};
    drawCheckBadge(dc, check, kAccent, RGB(240, 245, 255));

    RECT bannerText{banner.left + 46, banner.top, banner.right - 16, banner.bottom};
    drawTextLine(dc, L"NOW CONTROLLING", bannerText, metaFont, RGB(166, 194, 255));

    if (!state.targets.empty()) {
        RECT row{panel.left + 18, panel.top + 62, panel.right - 18, panel.top + 154};
        drawMediaRow(dc, row, state.targets[0], false, titleFont, bodyFont, metaFont);
    }

    RECT footer{panel.left + 2, panel.bottom - 48, panel.right - 2, panel.bottom - 2};
    drawFooter(dc, footer, false, true, keyFont, metaFont);
}

void paintMessageOverlay(HDC dc, const RECT& bounds, const OverlayState& state, HFONT titleFont) {
    RECT panel{0, 0, bounds.right, bounds.bottom};
    // Accent-tinted border matches the confirmation overlay vocabulary.
    fillRoundRect(dc, panel, 18, kPanel);

    // Accent check circle with a centered white tick (antialiased).
    RECT dot{18, 24, 38, 44};
    drawCheckBadge(dc, dot, kAccent, RGB(240, 245, 255));

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

RECT overlayWorkArea() {
    HWND foreground = GetForegroundWindow();
    HMONITOR monitor = foreground ? MonitorFromWindow(foreground, MONITOR_DEFAULTTONEAREST) : nullptr;
    if (!monitor) {
        POINT cursor{};
        GetCursorPos(&cursor);
        monitor = MonitorFromPoint(cursor, MONITOR_DEFAULTTONEAREST);
    }

    MONITORINFO info{};
    info.cbSize = sizeof(info);
    if (monitor && GetMonitorInfoW(monitor, &info)) {
        return info.rcWork;
    }

    RECT fallback{
        GetSystemMetrics(SM_XVIRTUALSCREEN),
        GetSystemMetrics(SM_YVIRTUALSCREEN),
        GetSystemMetrics(SM_XVIRTUALSCREEN) + GetSystemMetrics(SM_CXVIRTUALSCREEN),
        GetSystemMetrics(SM_YVIRTUALSCREEN) + GetSystemMetrics(SM_CYVIRTUALSCREEN),
    };
    return fallback;
}

POINT overlayPositionForSize(int width, int height) {
    RECT work = overlayWorkArea();
    int x = work.right - width - kOverlayMargin;
    int y = work.top + kOverlayTopMargin;

    if (x < work.left + 8) x = work.left + 8;
    if (y + height > work.bottom - 8) y = (std::max)(work.top + 8, work.bottom - height - 8);
    return {x, y};
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
        const POINT position = overlayPositionForSize(width, height);
        HRGN region = CreateRoundRectRgn(0, 0, width + 1, height + 1, 18, 18);
        SetWindowRgn(hwnd, region, TRUE);
        SetWindowPos(hwnd, HWND_TOPMOST, position.x, position.y, width, height, SWP_NOACTIVATE | SWP_SHOWWINDOW);
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
    ensureGdiplus();

    WNDCLASSW wc{};
    wc.lpfnWndProc = overlayWndProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"HotkeyToCommandMediaOverlay";
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    RegisterClassW(&wc);

    const int height = 68;
    const POINT position = overlayPositionForSize(kToastWidth, height);

    g_overlayWindow = CreateWindowExW(
        WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_LAYERED,
        wc.lpszClassName,
        L"Hotkey To Command Media Picker",
        WS_POPUP,
        position.x, position.y, kToastWidth, height,
        nullptr, nullptr, wc.hInstance, nullptr);

    if (g_overlayWindow) {
        SetLayeredWindowAttributes(g_overlayWindow, 0, 255, LWA_ALPHA);
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

            // Fetch album/thumbnail artwork from SMTC (best-effort).
            fetchSmtcThumbnail(session, target);

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

    replaceBrowserAppSessionsWhenTabsExist(targets, browserTargets, &hiddenBrowserAppSessions);

    targets.insert(targets.end(), browserTargets.begin(), browserTargets.end());
    hydrateBrowserArtwork(targets);
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

std::vector<MediaTarget> listMediaTargetsLive(std::wstring* report) {
    std::wstring windowsReport;
    std::wstring browserReport;
    std::vector<MediaTarget> targets = queryWindowsMediaTargets(&windowsReport);
    std::vector<MediaTarget> browserTargets = listBrowserMediaTargets(&browserReport);
    size_t hiddenBrowserAppSessions = 0;

    replaceBrowserAppSessionsWhenTabsExist(targets, browserTargets, &hiddenBrowserAppSessions);

    targets.insert(targets.end(), browserTargets.begin(), browserTargets.end());
    hydrateBrowserArtwork(targets);
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
    std::vector<MediaTarget> targets = listMediaTargetsLive(&queryReport);

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
