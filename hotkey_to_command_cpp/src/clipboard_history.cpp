#include "clipboard_history.h"

#include <windows.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <cstring>
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
constexpr size_t kMaxItems = 40;

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
};

std::mutex g_mutex;
std::vector<ClipItem> g_items;
size_t g_selected = 0;
bool g_pickerOpen = false;
unsigned long long g_nextId = 1;
bool g_internalWrite = false;

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

std::wstring appDataPath() {
    wchar_t appdata[MAX_PATH]{};
    DWORD size = GetEnvironmentVariableW(L"APPDATA", appdata, MAX_PATH);
    if (size == 0 || size >= MAX_PATH) return L"clipboard_history.json";
    return std::wstring(appdata) + L"\\HotkeyToCommand\\clipboard_history.json";
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
                {"text", wideToUtf8(item.text)}
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

void paintText(HDC dc, const std::wstring& text, RECT rect, COLORREF color, int size, int weight = FW_NORMAL) {
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, color);
    HFONT font = CreateFontW(size, 0, 0, 0, weight, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
        OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_SWISS, L"Segoe UI");
    HFONT old = static_cast<HFONT>(SelectObject(dc, font));
    DrawTextW(dc, text.c_str(), -1, &rect, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    SelectObject(dc, old);
    DeleteObject(font);
}

void paintPicker(HDC dc, const RECT& rect) {
    HBRUSH bg = CreateSolidBrush(RGB(18, 24, 32));
    FillRect(dc, &rect, bg);
    DeleteObject(bg);

    std::vector<ClipItem> items;
    size_t selected = 0;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        items = g_items;
        selected = g_selected;
    }

    RECT title{18, 10, rect.right - 18, 34};
    paintText(dc, L"Clipboard history", title, RGB(238, 246, 250), 18, FW_BOLD);

    if (items.empty()) {
        RECT empty{18, 46, rect.right - 18, 82};
        paintText(dc, L"No clipboard items yet", empty, RGB(166, 182, 196), 16);
        return;
    }

    const int rowHeight = 42;
    const int startY = 44;
    const size_t count = (std::min)(items.size(), static_cast<size_t>(6));
    for (size_t i = 0; i < count; ++i) {
        RECT row{12, startY + static_cast<int>(i) * rowHeight, rect.right - 12, startY + static_cast<int>(i + 1) * rowHeight - 6};
        const bool active = i == selected;
        HBRUSH rowBrush = CreateSolidBrush(active ? RGB(28, 70, 76) : RGB(25, 33, 43));
        HBRUSH oldBrush = static_cast<HBRUSH>(SelectObject(dc, rowBrush));
        HPEN pen = CreatePen(PS_SOLID, 1, active ? RGB(35, 210, 189) : RGB(48, 62, 76));
        HPEN oldPen = static_cast<HPEN>(SelectObject(dc, pen));
        RoundRect(dc, row.left, row.top, row.right, row.bottom, 10, 10);
        SelectObject(dc, oldPen);
        DeleteObject(pen);
        SelectObject(dc, oldBrush);
        DeleteObject(rowBrush);

        RECT badge{24, row.top + 7, 62, row.bottom - 7};
        paintText(dc, items[i].kind == ClipKind::Image ? L"IMG" : L"TXT", badge, active ? RGB(218, 255, 250) : RGB(156, 197, 255), 12, FW_BOLD);
        RECT label{68, row.top, row.right - 14, row.bottom};
        paintText(dc, itemTitle(items[i]), label, active ? RGB(245, 255, 255) : RGB(214, 225, 235), 15, active ? FW_BOLD : FW_NORMAL);
    }
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

LRESULT CALLBACK clipboardWndProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
    (void)lParam;
    switch (message) {
    case WM_CLIPBOARDUPDATE:
        captureClipboardNow();
        return 0;
    case WM_CLIP_HISTORY_SHOW:
        g_pickerOpen = true;
        SetWindowPos(hwnd, HWND_TOPMOST,
            GetSystemMetrics(SM_CXSCREEN) - 520,
            72,
            490,
            318,
            SWP_NOACTIVATE | SWP_SHOWWINDOW);
        ShowWindow(hwnd, SW_SHOWNOACTIVATE);
        InvalidateRect(hwnd, nullptr, TRUE);
        SetTimer(hwnd, kPickerTimer, static_cast<UINT>(wParam == 0 ? 6500 : wParam), nullptr);
        return 0;
    case WM_CLIP_HISTORY_HIDE:
        g_pickerOpen = false;
        ShowWindow(hwnd, SW_HIDE);
        KillTimer(hwnd, kPickerTimer);
        return 0;
    case WM_TIMER:
        if (wParam == kPickerTimer) {
            g_pickerOpen = false;
            ShowWindow(hwnd, SW_HIDE);
            KillTimer(hwnd, kPickerTimer);
        }
        return 0;
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(hwnd, &ps);
        RECT rect{};
        GetClientRect(hwnd, &rect);
        paintPicker(dc, rect);
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
    wc.lpszClassName = L"HotkeyToCommandClipboardHistory";
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    RegisterClassW(&wc);

    g_window = CreateWindowExW(
        WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_LAYERED,
        wc.lpszClassName,
        L"Hotkey To Command Clipboard History",
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
        SetLayeredWindowAttributes(g_window, 0, 242, LWA_ALPHA);
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
        g_pickerOpen = true;
        if (report) *report = L"Clipboard history items: " + std::to_wstring(g_items.size());
    }
    showPicker();
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
