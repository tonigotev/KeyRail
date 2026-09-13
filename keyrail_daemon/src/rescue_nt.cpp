#include "rescue_nt.h"

namespace rescue {

const NtApi& ntApi() {
    static const NtApi api = [] {
        NtApi resolved;
        // ntdll is always mapped; GetModuleHandle never loads anything.
        HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
        if (ntdll) {
            resolved.querySystemInformation = reinterpret_cast<NtQuerySystemInformationFn>(
                GetProcAddress(ntdll, "NtQuerySystemInformation"));
            resolved.queryInformationProcess = reinterpret_cast<NtQueryInformationProcessFn>(
                GetProcAddress(ntdll, "NtQueryInformationProcess"));
            resolved.suspendProcess = reinterpret_cast<NtSuspendProcessFn>(GetProcAddress(ntdll, "NtSuspendProcess"));
            resolved.resumeProcess = reinterpret_cast<NtSuspendProcessFn>(GetProcAddress(ntdll, "NtResumeProcess"));
        }
        return resolved;
    }();
    return api;
}

} // namespace rescue
