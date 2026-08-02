#include "app_audio.h"

#include <windows.h>
#include <audiopolicy.h>
#include <inspectable.h>
#include <mmdeviceapi.h>
#include <propvarutil.h>
#include <roapi.h>
#include <winstring.h>
#include <wrl/client.h>

#include <algorithm>
#include <filesystem>
#include <set>
#include <sstream>
#include <string>
#include <vector>

using Microsoft::WRL::ComPtr;

static const PROPERTYKEY kFriendlyName =
    {{0xa45c254e, 0xdf1c, 0x4efd, {0x80, 0x20, 0x67, 0xd1, 0x46, 0xa8, 0x50, 0xe0}}, 14};

static const GUID kAudioPolicyConfigFactory21H2 =
    {0xab3d4648, 0xe242, 0x459f, {0xb0, 0x2f, 0x54, 0x1c, 0x70, 0x30, 0x63, 0x24}};
static const GUID kAudioPolicyConfigFactoryDownlevel =
    {0x2a59116d, 0x6c4f, 0x45e0, {0xa7, 0x4f, 0x70, 0x7e, 0x3f, 0xef, 0x92, 0x58}};

static const wchar_t kMmdevapiToken[] = L"\\\\?\\SWD#MMDEVAPI#";
static const wchar_t kRenderInterface[] = L"#{e6327cad-dcec-4949-ae8a-991e976a79d2}";

struct IAudioPolicyConfigFactoryInternal : public IInspectable {
    virtual HRESULT STDMETHODCALLTYPE Unused1() = 0;
    virtual HRESULT STDMETHODCALLTYPE Unused2() = 0;
    virtual HRESULT STDMETHODCALLTYPE Unused3() = 0;
    virtual HRESULT STDMETHODCALLTYPE Unused4() = 0;
    virtual HRESULT STDMETHODCALLTYPE Unused5() = 0;
    virtual HRESULT STDMETHODCALLTYPE Unused6() = 0;
    virtual HRESULT STDMETHODCALLTYPE Unused7() = 0;
    virtual HRESULT STDMETHODCALLTYPE Unused8() = 0;
    virtual HRESULT STDMETHODCALLTYPE Unused9() = 0;
    virtual HRESULT STDMETHODCALLTYPE Unused10() = 0;
    virtual HRESULT STDMETHODCALLTYPE Unused11() = 0;
    virtual HRESULT STDMETHODCALLTYPE Unused12() = 0;
    virtual HRESULT STDMETHODCALLTYPE Unused13() = 0;
    virtual HRESULT STDMETHODCALLTYPE Unused14() = 0;
    virtual HRESULT STDMETHODCALLTYPE Unused15() = 0;
    virtual HRESULT STDMETHODCALLTYPE Unused16() = 0;
    virtual HRESULT STDMETHODCALLTYPE Unused17() = 0;
    virtual HRESULT STDMETHODCALLTYPE Unused18() = 0;
    virtual HRESULT STDMETHODCALLTYPE Unused19() = 0;
    virtual HRESULT STDMETHODCALLTYPE SetPersistedDefaultAudioEndpoint(UINT32 processId, EDataFlow flow, ERole role, HSTRING deviceId) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetPersistedDefaultAudioEndpoint(UINT32 processId, EDataFlow flow, ERole role, HSTRING* deviceId) = 0;
    virtual HRESULT STDMETHODCALLTYPE ClearAllPersistedApplicationDefaultEndpoints() = 0;
};

struct ComInit {
    bool ok;
    ComInit() { ok = SUCCEEDED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED)); }
    ~ComInit() { if (ok) CoUninitialize(); }
};

struct WinRtInit {
    HRESULT hr;
    WinRtInit() : hr(RoInitialize(RO_INIT_SINGLETHREADED)) {}
    ~WinRtInit() { if (SUCCEEDED(hr)) RoUninitialize(); }
    bool ok() const { return SUCCEEDED(hr) || hr == RPC_E_CHANGED_MODE; }
};

struct HString {
    HSTRING value = nullptr;
    explicit HString(const std::wstring& text) {
        WindowsCreateString(text.c_str(), static_cast<UINT32>(text.size()), &value);
    }
    ~HString() {
        if (value) WindowsDeleteString(value);
    }
};

struct FocusedProcess {
    DWORD pid = 0;
    std::wstring title;
    std::wstring exePath;
    std::wstring exeName;
};

struct SessionInfo {
    DWORD pid = 0;
    std::wstring processName;
    std::wstring displayName;
    std::wstring outputName;
    std::wstring outputId;
    AudioSessionState state = AudioSessionStateInactive;
};

struct OutputDevice {
    std::wstring id;
    std::wstring name;
};

static std::wstring lower(std::wstring value) {
    std::transform(value.begin(), value.end(), value.begin(), [](wchar_t c) {
        return static_cast<wchar_t>(towlower(c));
    });
    return value;
}

static std::wstring fileNameOf(const std::wstring& path) {
    if (path.empty()) return {};
    return std::filesystem::path(path).filename().wstring();
}

static std::wstring processPath(DWORD pid) {
    HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!process) return {};

    std::wstring path(32768, L'\0');
    DWORD size = static_cast<DWORD>(path.size());
    if (!QueryFullProcessImageNameW(process, 0, path.data(), &size)) {
        CloseHandle(process);
        return {};
    }

    CloseHandle(process);
    path.resize(size);
    return path;
}

static std::wstring windowTitle(HWND window) {
    int length = GetWindowTextLengthW(window);
    if (length <= 0) return {};

    std::wstring title(static_cast<size_t>(length) + 1, L'\0');
    int copied = GetWindowTextW(window, title.data(), length + 1);
    title.resize(copied > 0 ? static_cast<size_t>(copied) : 0);
    return title;
}

static FocusedProcess focusedProcess() {
    FocusedProcess focus;
    HWND window = GetForegroundWindow();
    if (!window) return focus;

    GetWindowThreadProcessId(window, &focus.pid);
    focus.title = windowTitle(window);
    focus.exePath = processPath(focus.pid);
    focus.exeName = fileNameOf(focus.exePath);
    return focus;
}

static std::wstring deviceId(IMMDevice* device) {
    LPWSTR id = nullptr;
    std::wstring out;
    if (SUCCEEDED(device->GetId(&id)) && id) {
        out = id;
        CoTaskMemFree(id);
    }
    return out;
}

static std::wstring friendlyName(IMMDevice* device, const std::wstring& fallback) {
    ComPtr<IPropertyStore> store;
    if (FAILED(device->OpenPropertyStore(STGM_READ, &store))) return fallback;

    PROPVARIANT value;
    PropVariantInit(&value);
    std::wstring name = fallback;
    if (SUCCEEDED(store->GetValue(kFriendlyName, &value)) && value.vt == VT_LPWSTR && value.pwszVal) {
        name = value.pwszVal;
    }
    PropVariantClear(&value);
    return name;
}

static std::wstring sessionDisplayName(IAudioSessionControl* session) {
    LPWSTR name = nullptr;
    std::wstring out;
    if (SUCCEEDED(session->GetDisplayName(&name)) && name) {
        out = name;
        CoTaskMemFree(name);
    }
    return out;
}

static std::wstring sessionStateName(AudioSessionState state) {
    if (state == AudioSessionStateActive) return L"active";
    if (state == AudioSessionStateInactive) return L"inactive";
    if (state == AudioSessionStateExpired) return L"expired";
    return L"unknown";
}

static std::wstring hresultText(HRESULT hr) {
    std::wstringstream text;
    text << L"0x" << std::hex << static_cast<unsigned long>(hr);
    if (hr == E_INVALIDARG) text << L" (invalid argument)";
    if (hr == E_ACCESSDENIED) text << L" (access denied)";
    if (hr == E_NOTIMPL) text << L" (not implemented)";
    if (hr == E_NOINTERFACE) text << L" (interface not available)";
    if (hr == REGDB_E_CLASSNOTREG) text << L" (class not registered)";
    return text.str();
}

static void collectDeviceSessions(IMMDevice* device, const OutputDevice& output, std::vector<SessionInfo>& sessions) {
    ComPtr<IAudioSessionManager2> manager;
    if (FAILED(device->Activate(__uuidof(IAudioSessionManager2), CLSCTX_ALL, nullptr, &manager))) return;

    ComPtr<IAudioSessionEnumerator> enumerator;
    if (FAILED(manager->GetSessionEnumerator(&enumerator))) return;

    int count = 0;
    if (FAILED(enumerator->GetCount(&count))) return;

    for (int i = 0; i < count; ++i) {
        ComPtr<IAudioSessionControl> session;
        if (FAILED(enumerator->GetSession(i, &session))) continue;

        AudioSessionState state;
        if (SUCCEEDED(session->GetState(&state)) && state == AudioSessionStateExpired) continue;

        ComPtr<IAudioSessionControl2> session2;
        if (FAILED(session.As(&session2))) continue;

        DWORD pid = 0;
        if (FAILED(session2->GetProcessId(&pid)) || pid == 0) continue;

        std::wstring path = processPath(pid);
        sessions.push_back({
            pid,
            fileNameOf(path),
            sessionDisplayName(session.Get()),
            output.name,
            output.id,
            state,
        });
    }
}

static std::vector<OutputDevice> activeRenderOutputs(IMMDeviceEnumerator* enumerator) {
    std::vector<OutputDevice> outputs;

    ComPtr<IMMDeviceCollection> devices;
    if (FAILED(enumerator->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE, &devices))) return outputs;

    UINT count = 0;
    if (FAILED(devices->GetCount(&count))) return outputs;

    for (UINT i = 0; i < count; ++i) {
        ComPtr<IMMDevice> device;
        if (FAILED(devices->Item(i, &device))) continue;

        std::wstring id = deviceId(device.Get());
        if (id.empty()) continue;
        outputs.push_back({id, friendlyName(device.Get(), id)});
    }

    return outputs;
}

static std::vector<SessionInfo> audioSessionsByOutput(IMMDeviceEnumerator* enumerator) {
    std::vector<SessionInfo> sessions;

    ComPtr<IMMDeviceCollection> devices;
    if (FAILED(enumerator->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE, &devices))) return sessions;

    UINT count = 0;
    if (FAILED(devices->GetCount(&count))) return sessions;

    for (UINT i = 0; i < count; ++i) {
        ComPtr<IMMDevice> device;
        if (FAILED(devices->Item(i, &device))) continue;

        std::wstring id = deviceId(device.Get());
        if (id.empty()) continue;
        collectDeviceSessions(device.Get(), {id, friendlyName(device.Get(), id)}, sessions);
    }

    return sessions;
}

static HRESULT createAudioPolicyFactory(IAudioPolicyConfigFactoryInternal** factory) {
    *factory = nullptr;

    HString className(L"Windows.Media.Internal.AudioPolicyConfig");
    HRESULT hr = RoGetActivationFactory(className.value, kAudioPolicyConfigFactory21H2, reinterpret_cast<void**>(factory));
    if (SUCCEEDED(hr)) return hr;

    return RoGetActivationFactory(className.value, kAudioPolicyConfigFactoryDownlevel, reinterpret_cast<void**>(factory));
}

static std::wstring persistedRenderDeviceId(const std::wstring& endpointId) {
    return std::wstring(kMmdevapiToken) + endpointId + kRenderInterface;
}

static HRESULT setPersistedOutputForPid(IAudioPolicyConfigFactoryInternal* factory, DWORD pid, const std::wstring& endpointId) {
    HString hDeviceId(persistedRenderDeviceId(endpointId));
    HRESULT hr = factory->SetPersistedDefaultAudioEndpoint(pid, eRender, eConsole, hDeviceId.value);
    if (FAILED(hr)) return hr;

    return factory->SetPersistedDefaultAudioEndpoint(pid, eRender, eMultimedia, hDeviceId.value);
}

static bool sessionMatchesFocus(const SessionInfo& session, const FocusedProcess& focus, const std::wstring& focusedExe) {
    if (session.pid == focus.pid) return true;
    return !focusedExe.empty() && lower(session.processName) == focusedExe;
}

std::wstring focusedAppExeName() {
    return focusedProcess().exeName;
}

bool describeFocusedAppAudio(std::wstring* outReport) {
    if (!outReport) return false;

    ComInit com;
    if (!com.ok) {
        *outReport = L"audio session inspect failed: could not initialize COM";
        return false;
    }

    FocusedProcess focus = focusedProcess();
    if (focus.pid == 0) {
        *outReport = L"audio session inspect failed: no focused window";
        return false;
    }

    ComPtr<IMMDeviceEnumerator> enumerator;
    if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&enumerator)))) {
        *outReport = L"audio session inspect failed: could not create device enumerator";
        return false;
    }

    std::vector<SessionInfo> sessions = audioSessionsByOutput(enumerator.Get());
    std::wstring focusedExe = lower(focus.exeName);

    std::wstringstream report;
    report << L"focused app\n";
    report << L"  pid: " << focus.pid << L"\n";
    report << L"  exe: " << (focus.exeName.empty() ? L"(unknown)" : focus.exeName) << L"\n";
    if (!focus.title.empty()) report << L"  title: " << focus.title << L"\n";
    if (!focus.exePath.empty()) report << L"  path: " << focus.exePath << L"\n";

    report << L"\naudio session matches\n";
    int exactMatches = 0;
    int appMatches = 0;
    auto writeMatchesForState = [&](AudioSessionState state) {
        for (const SessionInfo& session : sessions) {
            if (session.state != state) continue;

            bool exact = session.pid == focus.pid;
            bool sameExe = !focusedExe.empty() && lower(session.processName) == focusedExe;
            if (!exact && !sameExe) continue;

            if (exact) ++exactMatches;
            else ++appMatches;

            report << L"  " << sessionStateName(session.state) << L" "
                   << (exact ? L"exact" : L"same app") << L": "
                   << (session.processName.empty() ? L"(unknown process)" : session.processName)
                   << L" [" << session.pid << L"] -> " << session.outputName;
            if (!session.displayName.empty()) report << L" (" << session.displayName << L")";
            report << L"\n";
        }
    };
    writeMatchesForState(AudioSessionStateActive);
    writeMatchesForState(AudioSessionStateInactive);

    if (exactMatches == 0 && appMatches == 0) {
        report << L"  none found";
        if (!focus.exeName.empty()) report << L" for " << focus.exeName;
        report << L"\n";
    }

    report << L"\nall visible audio sessions\n";
    if (sessions.empty()) {
        report << L"  none\n";
    } else {
        for (const SessionInfo& session : sessions) {
            report << L"  " << sessionStateName(session.state) << L": "
                   << (session.processName.empty() ? L"(unknown process)" : session.processName)
                   << L" [" << session.pid << L"] -> " << session.outputName;
            if (!session.displayName.empty()) report << L" (" << session.displayName << L")";
            report << L"\n";
        }
    }

    *outReport = report.str();
    return true;
}

bool cycleFocusedAppAudioOutput(std::wstring* outReport) {
    if (!outReport) return false;

    ComInit com;
    if (!com.ok) {
        *outReport = L"focused app output switch failed: could not initialize COM";
        return false;
    }

    WinRtInit winrt;
    if (!winrt.ok()) {
        *outReport = L"focused app output switch failed: could not initialize Windows Runtime";
        return false;
    }

    FocusedProcess focus = focusedProcess();
    if (focus.pid == 0) {
        *outReport = L"focused app output switch failed: no focused window";
        return false;
    }

    ComPtr<IMMDeviceEnumerator> enumerator;
    if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&enumerator)))) {
        *outReport = L"focused app output switch failed: could not create device enumerator";
        return false;
    }

    std::vector<OutputDevice> outputs = activeRenderOutputs(enumerator.Get());
    if (outputs.size() < 2) {
        *outReport = L"focused app output switch failed: fewer than two active output devices";
        return false;
    }

    std::vector<SessionInfo> sessions = audioSessionsByOutput(enumerator.Get());
    std::wstring focusedExe = lower(focus.exeName);
    std::vector<SessionInfo> matches;

    for (AudioSessionState wantedState : {AudioSessionStateActive, AudioSessionStateInactive}) {
        for (const SessionInfo& session : sessions) {
            if (session.state != wantedState) continue;
            if (!sessionMatchesFocus(session, focus, focusedExe)) continue;
            matches.push_back(session);
        }
        if (!matches.empty()) break;
    }

    if (matches.empty()) {
        std::wstringstream report;
        report << L"focused app output switch failed: no audio session found for ";
        report << (focus.exeName.empty() ? L"focused app" : focus.exeName);
        *outReport = report.str();
        return false;
    }

    std::wstring currentOutputId = matches.front().outputId;
    size_t currentIndex = 0;
    bool foundCurrent = false;
    for (size_t i = 0; i < outputs.size(); ++i) {
        if (outputs[i].id == currentOutputId) {
            currentIndex = i;
            foundCurrent = true;
            break;
        }
    }
    if (!foundCurrent) currentIndex = 0;

    size_t nextIndex = (currentIndex + 1) % outputs.size();
    const OutputDevice& target = outputs[nextIndex];

    ComPtr<IAudioPolicyConfigFactoryInternal> policy;
    HRESULT factoryHr = createAudioPolicyFactory(&policy);
    if (FAILED(factoryHr)) {
        *outReport = L"focused app output switch failed: could not open Windows audio policy API (" + hresultText(factoryHr) + L")";
        return false;
    }

    std::set<DWORD> pids;
    for (const SessionInfo& session : matches) {
        pids.insert(session.pid);
    }

    std::wstringstream report;
    report << L"focused app output switch\n";
    report << L"  app: " << (focus.exeName.empty() ? L"(unknown)" : focus.exeName) << L"\n";
    report << L"  focused pid: " << focus.pid << L"\n";
    report << L"  from: " << matches.front().outputName << L"\n";
    report << L"  to: " << target.name << L"\n";
    if (!pids.count(focus.pid)) {
        report << L"  note: focused pid is not the audio-session pid, so only audio-session pids were changed\n";
    }
    report << L"\nwindows policy calls\n";

    bool anySucceeded = false;
    for (DWORD pid : pids) {
        HRESULT hr = setPersistedOutputForPid(policy.Get(), pid, target.id);
        if (SUCCEEDED(hr)) anySucceeded = true;

        report << L"  pid " << pid << L": ";
        if (SUCCEEDED(hr)) {
            report << L"ok";
        } else {
            report << L"failed " << hresultText(hr);
        }
        report << L"\n";
    }

    report << L"\nmatched sessions used\n";
    for (const SessionInfo& session : matches) {
        report << L"  " << sessionStateName(session.state) << L": "
               << (session.processName.empty() ? L"(unknown process)" : session.processName)
               << L" [" << session.pid << L"] -> " << session.outputName << L"\n";
    }

    report << L"\nIf the app keeps using the old output, it may ignore Windows per-app routing or need its audio stream restarted.";

    *outReport = report.str();
    return anySucceeded;
}
