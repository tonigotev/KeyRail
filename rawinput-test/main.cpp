/*
    Raw Input keyboard detector proof of concept.

    What the important Raw Input calls do:
    - GetRawInputDeviceList:
        Enumerates every raw input device Windows knows about. We use it on
        startup to print device handles and device paths so you can identify
        the keyboard or board you want to test.

    - GetRawInputDeviceInfoW:
        Retrieves information for one raw input device. This program uses
        RIDI_DEVICENAME to get the device path/name and RIDI_DEVICEINFO to get
        the device type, so the startup list is readable and the event filter
        can match a device-name substring.

    - RegisterRawInputDevices:
        Tells Windows this process wants raw keyboard input. The keyboard usage
        is usUsagePage = 0x01 and usUsage = 0x06. RIDEV_INPUTSINK is used so
        Windows sends WM_INPUT to our message-only window even when another app
        is focused.

    - WM_INPUT:
        The message Windows posts whenever raw input is available. We handle it
        in WndProc and call GetRawInputData to retrieve the RAWINPUT payload.

    - GetRawInputData:
        Copies the raw input payload for one WM_INPUT message. For keyboard
        input, the useful fields here are hDevice, VKey, MakeCode, and Flags.

    This program intentionally does not use SetWindowsHookEx, RegisterHotKey,
    SendInput, or any input injection API. It only observes WM_INPUT events.
*/

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <algorithm>
#include <cwctype>
#include <cstdint>
#include <iostream>
#include <string>
#include <unordered_map>
#include <vector>

static std::wstring g_filter;
static std::unordered_map<HANDLE, std::wstring> g_deviceNameCache;

static std::wstring toLower(std::wstring value) {
    std::transform(value.begin(), value.end(), value.begin(), [](wchar_t ch) {
        return static_cast<wchar_t>(towlower(ch));
    });
    return value;
}

static std::wstring argToWide(const char* value) {
    if (!value || !*value) return L"";

    int size = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value, -1, nullptr, 0);
    UINT codePage = CP_UTF8;
    DWORD flags = MB_ERR_INVALID_CHARS;
    if (size <= 0) {
        codePage = CP_ACP;
        flags = 0;
        size = MultiByteToWideChar(codePage, flags, value, -1, nullptr, 0);
    }
    if (size <= 0) return L"";

    std::wstring out(size, L'\0');
    MultiByteToWideChar(codePage, flags, value, -1, &out[0], size);
    while (!out.empty() && out.back() == L'\0') {
        out.pop_back();
    }
    return out;
}

static const wchar_t* deviceTypeName(DWORD type) {
    switch (type) {
    case RIM_TYPEMOUSE:
        return L"mouse";
    case RIM_TYPEKEYBOARD:
        return L"keyboard";
    case RIM_TYPEHID:
        return L"hid";
    default:
        return L"unknown";
    }
}

static std::wstring getDeviceName(HANDLE device) {
    auto cached = g_deviceNameCache.find(device);
    if (cached != g_deviceNameCache.end()) return cached->second;

    UINT size = 0;
    GetRawInputDeviceInfoW(device, RIDI_DEVICENAME, nullptr, &size);
    if (size == 0) {
        g_deviceNameCache[device] = L"";
        return L"";
    }

    std::wstring name(size, L'\0');
    UINT result = GetRawInputDeviceInfoW(device, RIDI_DEVICENAME, &name[0], &size);
    if (result == static_cast<UINT>(-1)) {
        g_deviceNameCache[device] = L"";
        return L"";
    }

    while (!name.empty() && name.back() == L'\0') {
        name.pop_back();
    }

    g_deviceNameCache[device] = name;
    return name;
}

static bool deviceMatchesFilter(HANDLE device) {
    if (g_filter.empty()) return true;
    std::wstring name = toLower(getDeviceName(device));
    return name.find(g_filter) != std::wstring::npos;
}

static void printDeviceList() {
    UINT count = 0;
    if (GetRawInputDeviceList(nullptr, &count, sizeof(RAWINPUTDEVICELIST)) == static_cast<UINT>(-1)) {
        std::wcerr << L"GetRawInputDeviceList failed: " << GetLastError() << L"\n";
        std::wcerr.flush();
        return;
    }

    std::vector<RAWINPUTDEVICELIST> devices(count);
    UINT result = GetRawInputDeviceList(devices.data(), &count, sizeof(RAWINPUTDEVICELIST));
    if (result == static_cast<UINT>(-1)) {
        std::wcerr << L"GetRawInputDeviceList failed: " << GetLastError() << L"\n";
        std::wcerr.flush();
        return;
    }

    std::wcout << L"Raw input devices: " << result << L"\n";
    for (UINT i = 0; i < result; ++i) {
        RID_DEVICE_INFO info{};
        info.cbSize = sizeof(info);
        UINT infoSize = sizeof(info);
        if (GetRawInputDeviceInfoW(devices[i].hDevice, RIDI_DEVICEINFO, &info, &infoSize) == static_cast<UINT>(-1)) {
            info.dwType = devices[i].dwType;
        }

        std::wstring name = getDeviceName(devices[i].hDevice);
        std::wcout << L"[" << i << L"] "
                   << L"type=" << deviceTypeName(devices[i].dwType)
                   << L" handle=0x" << std::hex << reinterpret_cast<std::uintptr_t>(devices[i].hDevice)
                   << std::dec << L"\n"
                   << L"    " << (name.empty() ? L"(no device name)" : name) << L"\n";
    }
    std::wcout.flush();
}

static bool registerKeyboardRawInput(HWND hwnd) {
    RAWINPUTDEVICE device{};
    device.usUsagePage = 0x01;
    device.usUsage = 0x06;
    device.dwFlags = RIDEV_INPUTSINK;
    device.hwndTarget = hwnd;

    if (!RegisterRawInputDevices(&device, 1, sizeof(device))) {
        std::wcerr << L"RegisterRawInputDevices failed: " << GetLastError() << L"\n";
        std::wcerr.flush();
        return false;
    }
    return true;
}

static void handleRawInput(HRAWINPUT inputHandle) {
    UINT size = 0;
    if (GetRawInputData(inputHandle, RID_INPUT, nullptr, &size, sizeof(RAWINPUTHEADER)) != 0 || size == 0) {
        return;
    }

    std::vector<BYTE> buffer(size);
    UINT copied = GetRawInputData(inputHandle, RID_INPUT, buffer.data(), &size, sizeof(RAWINPUTHEADER));
    if (copied == static_cast<UINT>(-1) || copied != size) {
        std::wcerr << L"GetRawInputData failed: " << GetLastError() << L"\n";
        std::wcerr.flush();
        return;
    }

    RAWINPUT* raw = reinterpret_cast<RAWINPUT*>(buffer.data());
    if (raw->header.dwType != RIM_TYPEKEYBOARD) return;
    if (!deviceMatchesFilter(raw->header.hDevice)) return;

    const RAWKEYBOARD& keyboard = raw->data.keyboard;
    bool isBreak = (keyboard.Flags & RI_KEY_BREAK) != 0;
    ULONGLONG timestamp = GetTickCount64();

    std::wcout << timestamp
               << L"ms device=0x" << std::hex << reinterpret_cast<std::uintptr_t>(raw->header.hDevice)
               << std::dec
               << L" vk=0x" << std::hex << keyboard.VKey
               << L" scan=0x" << keyboard.MakeCode
               << std::dec
               << L" " << (isBreak ? L"up" : L"down")
               << L"\n";
    std::wcout.flush();
}

static LRESULT CALLBACK WndProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
    (void)hwnd;
    (void)wParam;

    switch (message) {
    case WM_INPUT:
        handleRawInput(reinterpret_cast<HRAWINPUT>(lParam));
        return 0;
    default:
        return DefWindowProcW(hwnd, message, wParam, lParam);
    }
}

int main(int argc, char** argv) {
    if (argc > 1) {
        std::wstring filter = argToWide(argv[1]);
        g_filter = toLower(filter);
        std::wcout << L"Device-name filter: " << filter << L"\n";
    } else {
        std::wcout << L"Device-name filter: (none, logging all keyboards)\n";
    }

    printDeviceList();

    HINSTANCE instance = GetModuleHandleW(nullptr);
    const wchar_t className[] = L"RawInputKeyboardDetectorMessageOnlyWindow";

    WNDCLASSW wc{};
    wc.lpfnWndProc = WndProc;
    wc.hInstance = instance;
    wc.lpszClassName = className;

    if (!RegisterClassW(&wc)) {
        std::wcerr << L"RegisterClassW failed: " << GetLastError() << L"\n";
        std::wcerr.flush();
        return 1;
    }

    HWND hwnd = CreateWindowExW(
        0,
        className,
        L"Raw Input Keyboard Detector",
        0,
        0,
        0,
        0,
        0,
        HWND_MESSAGE,
        nullptr,
        instance,
        nullptr);

    if (!hwnd) {
        std::wcerr << L"CreateWindowExW(HWND_MESSAGE) failed: " << GetLastError() << L"\n";
        std::wcerr.flush();
        return 1;
    }

    if (!registerKeyboardRawInput(hwnd)) {
        DestroyWindow(hwnd);
        return 1;
    }

    std::wcout << L"\nListening for WM_INPUT keyboard events. Press Ctrl+C to stop.\n";
    std::wcout.flush();

    MSG msg{};
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    DestroyWindow(hwnd);
    return 0;
}
