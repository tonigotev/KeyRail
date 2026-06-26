// audio.cpp -- enumerate active render endpoints and switch the default via
// the undocumented IPolicyConfig. RAII (ComInit + ComPtr) means no manual
// CoUninitialize/Release calls, so no leak risk even on early returns.

#include <windows.h>
#include <mmdeviceapi.h>
#include <wrl/client.h>
#include <propvarutil.h>
#include <vector>
#include <string>
#include "audio.h"

using Microsoft::WRL::ComPtr;

// ---- the undocumented interface --------------------------------------------
// 10 reserved slots, then SetDefaultEndpoint. Only the vtable POSITION matters
// for the methods we don't call, so they're declared as no-arg placeholders.
struct __declspec(uuid("f8679f50-850a-41cf-9c72-430f290290c8"))
IPolicyConfig : public IUnknown {
    virtual HRESULT STDMETHODCALLTYPE Unused1()  = 0;  // GetMixFormat
    virtual HRESULT STDMETHODCALLTYPE Unused2()  = 0;  // GetDeviceFormat
    virtual HRESULT STDMETHODCALLTYPE Unused3()  = 0;  // ResetDeviceFormat
    virtual HRESULT STDMETHODCALLTYPE Unused4()  = 0;  // SetDeviceFormat
    virtual HRESULT STDMETHODCALLTYPE Unused5()  = 0;  // GetProcessingPeriod
    virtual HRESULT STDMETHODCALLTYPE Unused6()  = 0;  // SetProcessingPeriod
    virtual HRESULT STDMETHODCALLTYPE Unused7()  = 0;  // GetShareMode
    virtual HRESULT STDMETHODCALLTYPE Unused8()  = 0;  // SetShareMode
    virtual HRESULT STDMETHODCALLTYPE Unused9()  = 0;  // GetPropertyValue
    virtual HRESULT STDMETHODCALLTYPE Unused10() = 0;  // SetPropertyValue
    virtual HRESULT STDMETHODCALLTYPE SetDefaultEndpoint(PCWSTR id, ERole role) = 0;
    // (SetEndpointVisibility, slot 12, not needed)
};

static const CLSID CLSID_CPolicyConfigClient =
    {0x870af99c, 0x171d, 0x4f9e, {0xaf,0x0d,0xe6,0x3d,0xf4,0x0c,0x2b,0xc9}};

// PKEY_Device_FriendlyName, defined locally to avoid GUID-link fuss:
//   {a45c254e-df1c-4efd-8020-67d146a850e0}, pid 14
static const PROPERTYKEY kFriendlyName =
    {{0xa45c254e,0xdf1c,0x4efd,{0x80,0x20,0x67,0xd1,0x46,0xa8,0x50,0xe0}}, 14};

// RAII COM apartment for the calling (worker) thread.
struct ComInit {
    bool ok;
    ComInit()  { ok = SUCCEEDED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED)); }
    ~ComInit() { if (ok) CoUninitialize(); }
};

static std::wstring deviceId(IMMDevice* dev) {
    LPWSTR id = nullptr;
    std::wstring out;
    if (SUCCEEDED(dev->GetId(&id)) && id) { out = id; CoTaskMemFree(id); }
    return out;
}

static std::wstring friendlyName(IMMDevice* dev, const std::wstring& fallback) {
    ComPtr<IPropertyStore> store;
    if (FAILED(dev->OpenPropertyStore(STGM_READ, &store))) return fallback;
    PROPVARIANT pv; PropVariantInit(&pv);
    std::wstring name = fallback;
    if (SUCCEEDED(store->GetValue(kFriendlyName, &pv)) && pv.vt == VT_LPWSTR && pv.pwszVal)
        name = pv.pwszVal;
    PropVariantClear(&pv);
    return name;
}

bool cycleAudioDevice(std::wstring* outName) {
    ComInit com;

    ComPtr<IMMDeviceEnumerator> enumr;
    if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr,
                                CLSCTX_ALL, IID_PPV_ARGS(&enumr))))
        return false;

    ComPtr<IMMDeviceCollection> coll;
    if (FAILED(enumr->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE, &coll)))
        return false;

    UINT count = 0;
    if (FAILED(coll->GetCount(&count)) || count == 0) return false;

    std::vector<std::wstring> ids, names;
    ids.reserve(count); names.reserve(count);
    for (UINT i = 0; i < count; ++i) {
        ComPtr<IMMDevice> dev;
        if (FAILED(coll->Item(i, &dev))) continue;
        std::wstring id = deviceId(dev.Get());
        if (id.empty()) continue;
        ids.push_back(id);
        names.push_back(friendlyName(dev.Get(), id));
    }
    if (ids.empty()) return false;

    // current default
    std::wstring current;
    ComPtr<IMMDevice> def;
    if (SUCCEEDED(enumr->GetDefaultAudioEndpoint(eRender, eMultimedia, &def)))
        current = deviceId(def.Get());

    size_t idx = 0; bool found = false;
    for (size_t i = 0; i < ids.size(); ++i)
        if (ids[i] == current) { idx = i; found = true; break; }
    size_t next = found ? (idx + 1) % ids.size() : 0;

    ComPtr<IPolicyConfig> policy;
    if (FAILED(CoCreateInstance(CLSID_CPolicyConfigClient, nullptr,
                                CLSCTX_ALL, IID_PPV_ARGS(&policy))))
        return false;

    for (ERole role : {eConsole, eMultimedia, eCommunications})
        policy->SetDefaultEndpoint(ids[next].c_str(), role);

    if (outName) *outName = names[next];
    return true;
}
