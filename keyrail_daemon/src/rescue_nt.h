#pragma once
// rescue_nt.h -- the NT structures the rescue sampler walks.
//
// winternl.h ships these with most fields named Reserved. These are the full
// x64 layouts, stable since Vista and identical on every Windows 10/11 build,
// under distinct names so both headers can coexist in one translation unit.
// The static_asserts at the bottom pin the layout; if one ever fires, the
// sampler would be reading garbage and must not run.

#include <windows.h>
#include <winternl.h>

#include <cstddef>
#include <cstdint>

namespace rescue {

constexpr ULONG kSystemProcessInformation = 5;
constexpr ULONG kProcessBreakOnTermination = 29;
constexpr ULONG kProcessProtectionInformation = 61;
constexpr NTSTATUS kStatusInfoLengthMismatch = static_cast<NTSTATUS>(0xC0000004L);

struct ClientId {
    HANDLE UniqueProcess;
    HANDLE UniqueThread;
};

// KTHREAD_STATE. Ready/Running/Standby/DeferredReady are "wants a CPU".
enum ThreadState : ULONG {
    kThreadInitialized = 0,
    kThreadReady = 1,
    kThreadRunning = 2,
    kThreadStandby = 3,
    kThreadTerminated = 4,
    kThreadWaiting = 5,
    kThreadTransition = 6,
    kThreadDeferredReady = 7,
};

// KWAIT_REASON, only the values the sampler interprets. WrUserRequest is the
// wait a healthy GUI thread sits in inside GetMessage; a thread that owns a
// window but is not in that wait is not pumping messages.
enum WaitReason : ULONG {
    kWaitExecutive = 0,
    kWaitFreePage = 1,
    kWaitPageIn = 2,
    kWaitDelayExecution = 4,
    kWaitSuspended = 5,
    kWaitUserRequest = 6,
    kWaitWrFreePage = 8,
    kWaitWrPageIn = 9,
    kWaitWrDelayExecution = 11,
    kWaitWrSuspended = 12,
    kWaitWrUserRequest = 13,
    kWaitWrVirtualMemory = 18,
    kWaitWrPageOut = 19,
};

struct ThreadInfo {
    LARGE_INTEGER KernelTime;
    LARGE_INTEGER UserTime;
    LARGE_INTEGER CreateTime;
    ULONG WaitTime;
    PVOID StartAddress;
    ClientId ClientId;
    LONG Priority;
    LONG BasePriority;
    ULONG ContextSwitches;
    ULONG ThreadState;
    ULONG WaitReason;
};

struct ProcessInfo {
    ULONG NextEntryOffset;
    ULONG NumberOfThreads;
    LARGE_INTEGER WorkingSetPrivateSize;
    ULONG HardFaultCount;
    ULONG NumberOfThreadsHighWatermark;
    ULONGLONG CycleTime;
    LARGE_INTEGER CreateTime;
    LARGE_INTEGER UserTime;
    LARGE_INTEGER KernelTime;
    UNICODE_STRING ImageName;
    LONG BasePriority;
    HANDLE UniqueProcessId;
    HANDLE InheritedFromUniqueProcessId;
    ULONG HandleCount;
    ULONG SessionId;
    ULONG_PTR UniqueProcessKey;
    SIZE_T PeakVirtualSize;
    SIZE_T VirtualSize;
    ULONG PageFaultCount;
    SIZE_T PeakWorkingSetSize;
    SIZE_T WorkingSetSize;
    SIZE_T QuotaPeakPagedPoolUsage;
    SIZE_T QuotaPagedPoolUsage;
    SIZE_T QuotaPeakNonPagedPoolUsage;
    SIZE_T QuotaNonPagedPoolUsage;
    SIZE_T PagefileUsage;
    SIZE_T PeakPagefileUsage;
    SIZE_T PrivatePageCount;
    LARGE_INTEGER ReadOperationCount;
    LARGE_INTEGER WriteOperationCount;
    LARGE_INTEGER OtherOperationCount;
    LARGE_INTEGER ReadTransferCount;
    LARGE_INTEGER WriteTransferCount;
    LARGE_INTEGER OtherTransferCount;
    ThreadInfo Threads[1];
};

// PS_PROTECTION, returned by ProcessProtectionInformation.
struct ProtectionInfo {
    UCHAR Level;   // Type in bits 0-2, Audit bit 3, Signer in bits 4-7.
};

static_assert(sizeof(void*) == 8, "rescue sampler layouts are x64 only");
static_assert(sizeof(ThreadInfo) == 0x50, "SYSTEM_THREAD_INFORMATION layout");
static_assert(offsetof(ProcessInfo, Threads) == 0x100, "SYSTEM_PROCESS_INFORMATION layout");

using NtQuerySystemInformationFn = NTSTATUS(NTAPI*)(ULONG, PVOID, ULONG, PULONG);
using NtQueryInformationProcessFn = NTSTATUS(NTAPI*)(HANDLE, ULONG, PVOID, ULONG, PULONG);
using NtSuspendProcessFn = NTSTATUS(NTAPI*)(HANDLE);

struct NtApi {
    NtQuerySystemInformationFn querySystemInformation = nullptr;
    NtQueryInformationProcessFn queryInformationProcess = nullptr;
    // Freeze / thaw every thread of a process in one kernel call. Reversible,
    // needs no display, and stops a fault storm at its source.
    NtSuspendProcessFn suspendProcess = nullptr;
    NtSuspendProcessFn resumeProcess = nullptr;
};

// Resolved once through GetProcAddress on the first call and cached. Call it
// during init so the panic path never triggers the lookup.
const NtApi& ntApi();

} // namespace rescue
