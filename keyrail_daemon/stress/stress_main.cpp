// stress_main.cpp -- synthetic stressors for testing the rescue menu.
//
// One exe, one failure mode per run. Each prints its own PID so it can be
// matched against rescue_probe / the overlay.
//
//   rescue_stress spin        [--threads N] [--realtime]
//   rescue_stress balloon     [--mb N] [--trim]
//   rescue_stress hang
//   rescue_stress leak        [--rate N] [--max N]
//   rescue_stress fullscreen
//
// balloon without --mb targets physical RAM + 25%, which is meant to thrash
// the machine. Run it in a VM or Windows Sandbox unless you mean it.

#include <windows.h>
#include <d3d11.h>
#include <dxgi.h>

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

static volatile bool g_stop = false;

static BOOL WINAPI onConsoleCtrl(DWORD) {
    g_stop = true;
    return TRUE;
}

static int argInt(int argc, wchar_t** argv, const wchar_t* name, int fallback) {
    for (int i = 2; i + 1 < argc; ++i) {
        if (!wcscmp(argv[i], name)) return _wtoi(argv[i + 1]);
    }
    return fallback;
}

static bool argFlag(int argc, wchar_t** argv, const wchar_t* name) {
    for (int i = 2; i < argc; ++i) {
        if (!wcscmp(argv[i], name)) return true;
    }
    return false;
}

// ---- spin -----------------------------------------------------------------------

static DWORD WINAPI spinThread(LPVOID) {
    volatile unsigned long long counter = 0;
    while (!g_stop) ++counter;
    return 0;
}

static int runSpin(int argc, wchar_t** argv) {
    const int threads = argInt(argc, argv, L"--threads", 1);
    const bool realtime = argFlag(argc, argv, L"--realtime");

    if (realtime) {
        // Needs SeIncreaseBasePriorityPrivilege (an elevated console). This is
        // the "spinning at realtime priority" failure mode: on a machine with
        // as many spinners as cores nothing else gets scheduled.
        if (!SetPriorityClass(GetCurrentProcess(), REALTIME_PRIORITY_CLASS)) {
            wprintf(L"REALTIME_PRIORITY_CLASS refused (error %lu); running as HIGH instead\n", GetLastError());
            SetPriorityClass(GetCurrentProcess(), HIGH_PRIORITY_CLASS);
        }
    }

    wprintf(L"spin: pid %lu, %d thread(s)%ls. Ctrl+C to stop.\n",
        GetCurrentProcessId(), threads, realtime ? L", time-critical" : L"");
    for (int i = 0; i < threads; ++i) {
        HANDLE thread = CreateThread(nullptr, 0, spinThread, nullptr, 0, nullptr);
        if (thread && realtime) SetThreadPriority(thread, THREAD_PRIORITY_TIME_CRITICAL);
    }
    while (!g_stop) Sleep(200);
    return 0;
}

// ---- balloon ----------------------------------------------------------------------

// Every page gets a full 4 KB of pseudo-random bytes. Windows compresses
// evicted pages before it writes them to the pagefile, so a page that is
// mostly zero costs the compressor nothing and the disk never gets involved;
// the storm only starts when the pages are incompressible, like a real hog's.
static void fillIncompressible(BYTE* base, SIZE_T bytes, unsigned long long seed) {
    unsigned long long x = seed * 0x9E3779B97F4A7C15ull + 0x2545F4914F6CDD1Dull;
    auto* words = reinterpret_cast<unsigned long long*>(base);
    const SIZE_T count = bytes / sizeof(unsigned long long);
    for (SIZE_T i = 0; i < count && !g_stop; ++i) {
        x ^= x << 13;
        x ^= x >> 7;
        x ^= x << 17;
        words[i] = x;
    }
}

static int runBalloon(int argc, wchar_t** argv) {
    MEMORYSTATUSEX memory{};
    memory.dwLength = sizeof(memory);
    GlobalMemoryStatusEx(&memory);
    const int physicalMb = static_cast<int>(memory.ullTotalPhys >> 20);
    const int targetMb = argInt(argc, argv, L"--mb", physicalMb + physicalMb / 4);
    const bool trim = argFlag(argc, argv, L"--trim");

    wprintf(L"balloon: pid %lu, target %d MB (physical %d MB)%ls. Ctrl+C to stop.\n",
        GetCurrentProcessId(), targetMb, physicalMb, trim ? L", trimming working set each pass" : L"");
    if (!trim && targetMb > physicalMb) {
        wprintf(L"this will thrash the machine once it passes physical RAM.\n");
    }

    const SIZE_T chunkBytes = 64u << 20;
    std::vector<BYTE*> chunks;
    SIZE_T allocatedMb = 0;
    unsigned long long seed = 1;

    // Allocate and touch every page. Committing alone changes nothing; the
    // fault storm starts when the pages are written.
    while (!g_stop && allocatedMb < static_cast<SIZE_T>(targetMb)) {
        BYTE* chunk = static_cast<BYTE*>(VirtualAlloc(nullptr, chunkBytes, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
        if (!chunk) {
            wprintf(L"VirtualAlloc failed at %llu MB (error %lu); holding what we have\n",
                static_cast<unsigned long long>(allocatedMb), GetLastError());
            break;
        }
        fillIncompressible(chunk, chunkBytes, seed++);
        chunks.push_back(chunk);
        allocatedMb += chunkBytes >> 20;
        if ((allocatedMb % 1024) == 0) wprintf(L"  %llu MB touched\n", static_cast<unsigned long long>(allocatedMb));
    }
    wprintf(L"holding %llu MB; sweeping to keep faulting\n", static_cast<unsigned long long>(allocatedMb));

    // Keep rewriting pages in order so whatever got evicted is faulted back in.
    unsigned pass = 0;
    while (!g_stop) {
        if (trim) SetProcessWorkingSetSize(GetCurrentProcess(), static_cast<SIZE_T>(-1), static_cast<SIZE_T>(-1));
        for (BYTE* chunk : chunks) {
            if (g_stop) break;
            fillIncompressible(chunk, chunkBytes, seed++);
        }
        ++pass;
    }

    for (BYTE* chunk : chunks) VirtualFree(chunk, 0, MEM_RELEASE);
    return 0;
}

// ---- hang ----------------------------------------------------------------------------

static LRESULT CALLBACK hangWndProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
    switch (message) {
    case WM_TIMER:
        KillTimer(hwnd, 1);
        wprintf(L"window thread now sleeping forever inside WndProc\n");
        Sleep(INFINITE);
        return 0;
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(hwnd, &ps);
        RECT rect;
        GetClientRect(hwnd, &rect);
        FillRect(dc, &rect, static_cast<HBRUSH>(GetStockObject(WHITE_BRUSH)));
        DrawTextW(dc, L"KeyRail stress: this window hangs in 2 seconds", -1, &rect, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
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

static int runHang() {
    WNDCLASSW wc{};
    wc.lpfnWndProc = hangWndProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"KeyRailStressHang";
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    RegisterClassW(&wc);

    HWND hwnd = CreateWindowExW(0, wc.lpszClassName, L"KeyRail stress: hang", WS_OVERLAPPEDWINDOW | WS_VISIBLE,
        CW_USEDEFAULT, CW_USEDEFAULT, 520, 200, nullptr, nullptr, wc.hInstance, nullptr);
    if (!hwnd) return 1;

    wprintf(L"hang: pid %lu, window up; it stops pumping in 2s. Kill it from the rescue menu.\n", GetCurrentProcessId());
    SetTimer(hwnd, 1, 2000, nullptr);

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    return 0;
}

// ---- leak -------------------------------------------------------------------------------

static HANDLE g_never = nullptr;

static DWORD WINAPI leakThread(LPVOID) {
    WaitForSingleObject(g_never, INFINITE);
    return 0;
}

static int runLeak(int argc, wchar_t** argv) {
    const int rate = argInt(argc, argv, L"--rate", 20);
    const int maximum = argInt(argc, argv, L"--max", 2000);
    g_never = CreateEventW(nullptr, TRUE, FALSE, nullptr);

    wprintf(L"leak: pid %lu, %d threads/s up to %d. Ctrl+C to stop.\n", GetCurrentProcessId(), rate, maximum);
    int created = 0;
    const DWORD sleepMs = rate > 0 ? static_cast<DWORD>(1000 / rate) : 1000;
    while (!g_stop && created < maximum) {
        // Small reserved stacks so a few thousand threads fit in address space
        // and the leak signature is thread count, not memory.
        HANDLE thread = CreateThread(nullptr, 64 * 1024, leakThread, nullptr, STACK_SIZE_PARAM_IS_A_RESERVATION, nullptr);
        if (thread) {
            CloseHandle(thread);
            ++created;
            if (created % 200 == 0) wprintf(L"  %d threads\n", created);
        }
        Sleep(sleepMs);
    }
    wprintf(L"holding %d threads\n", created);
    while (!g_stop) Sleep(200);
    return 0;
}

// ---- fullscreen ---------------------------------------------------------------------

static LRESULT CALLBACK fullscreenWndProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
    if (message == WM_KEYDOWN && wParam == VK_ESCAPE) {
        DestroyWindow(hwnd);
        return 0;
    }
    if (message == WM_DESTROY) {
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, message, wParam, lParam);
}

static int runFullscreen() {
    WNDCLASSW wc{};
    wc.lpfnWndProc = fullscreenWndProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"KeyRailStressFullscreen";
    RegisterClassW(&wc);

    const int width = GetSystemMetrics(SM_CXSCREEN);
    const int height = GetSystemMetrics(SM_CYSCREEN);
    HWND hwnd = CreateWindowExW(0, wc.lpszClassName, L"KeyRail stress: exclusive fullscreen", WS_POPUP | WS_VISIBLE,
        0, 0, width, height, nullptr, nullptr, wc.hInstance, nullptr);
    if (!hwnd) return 1;

    DXGI_SWAP_CHAIN_DESC desc{};
    desc.BufferCount = 2;
    desc.BufferDesc.Width = width;
    desc.BufferDesc.Height = height;
    desc.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.BufferDesc.RefreshRate.Numerator = 0;
    desc.BufferDesc.RefreshRate.Denominator = 1;
    desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    desc.OutputWindow = hwnd;
    desc.SampleDesc.Count = 1;
    desc.Windowed = FALSE;   // exclusive fullscreen: the compositor is out of the path
    desc.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;
    desc.Flags = DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH;

    ID3D11Device* device = nullptr;
    ID3D11DeviceContext* context = nullptr;
    IDXGISwapChain* swapChain = nullptr;
    HRESULT hr = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, nullptr, 0,
        D3D11_SDK_VERSION, &desc, &swapChain, &device, nullptr, &context);
    if (FAILED(hr)) {
        wprintf(L"D3D11CreateDeviceAndSwapChain failed: 0x%08lx\n", hr);
        return 1;
    }

    ID3D11Texture2D* backBuffer = nullptr;
    ID3D11RenderTargetView* target = nullptr;
    swapChain->GetBuffer(0, __uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&backBuffer));
    if (backBuffer) {
        device->CreateRenderTargetView(backBuffer, nullptr, &target);
        backBuffer->Release();
    }

    wprintf(L"fullscreen: pid %lu, exclusive D3D11 fullscreen. Esc quits. Trigger the rescue menu now.\n", GetCurrentProcessId());

    MSG msg{};
    float t = 0.0f;
    while (msg.message != WM_QUIT) {
        if (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
            continue;
        }
        t += 0.01f;
        const float color[4] = {0.5f + 0.5f * sinf(t), 0.2f, 0.5f + 0.5f * cosf(t), 1.0f};
        if (target) context->ClearRenderTargetView(target, color);
        swapChain->Present(1, 0);
    }

    swapChain->SetFullscreenState(FALSE, nullptr);
    if (target) target->Release();
    swapChain->Release();
    context->Release();
    device->Release();
    return 0;
}

// ---- main ---------------------------------------------------------------------------------

int wmain(int argc, wchar_t** argv) {
    SetConsoleCtrlHandler(onConsoleCtrl, TRUE);
    if (argc < 2) {
        wprintf(L"usage: rescue_stress spin|balloon|hang|leak|fullscreen [options]\n");
        return 2;
    }
    const wchar_t* mode = argv[1];
    if (!wcscmp(mode, L"spin")) return runSpin(argc, argv);
    if (!wcscmp(mode, L"balloon")) return runBalloon(argc, argv);
    if (!wcscmp(mode, L"hang")) return runHang();
    if (!wcscmp(mode, L"leak")) return runLeak(argc, argv);
    if (!wcscmp(mode, L"fullscreen")) return runFullscreen();
    wprintf(L"unknown mode: %ls\n", mode);
    return 2;
}
