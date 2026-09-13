#include "rescue_meta.h"

#include "rescue_log.h"
#include "rescue_residency.h"

#include <algorithm>
#include <atomic>
#include <cstring>
#include <new>

namespace rescue {
namespace {

constexpr uint32_t kMetaSlots = 4096;          // power of two
constexpr uint32_t kResolvesPerTick = 5;       // rate limit: no disk spike of our own
constexpr DWORD kTickMs = 1000;

MetaEntry* g_table = nullptr;                  // open addressed on pid; VirtualLock'd
SRWLOCK g_tableLock = SRWLOCK_INIT;            // writer: worker; readers: overlay
HANDLE g_thread = nullptr;
HANDLE g_stop = nullptr;
std::atomic<uint32_t> g_resolved{0};
std::atomic<uint32_t> g_misses{0};
std::atomic<uint32_t> g_evicted{0};

uint32_t slotFor(uint32_t pid) {
    return ((pid >> 2) * 2654435761u) & (kMetaSlots - 1);
}

MetaEntry* find(uint32_t pid) {
    uint32_t slot = slotFor(pid);
    for (uint32_t probe = 0; probe < kMetaSlots; ++probe) {
        MetaEntry& entry = g_table[slot];
        if (entry.pid == 0) return nullptr;
        if (entry.pid == pid) return &entry;
        slot = (slot + 1) & (kMetaSlots - 1);
    }
    return nullptr;
}

MetaEntry* insert(uint32_t pid, int64_t createTime) {
    uint32_t slot = slotFor(pid);
    for (uint32_t probe = 0; probe < kMetaSlots; ++probe) {
        MetaEntry& entry = g_table[slot];
        // A tombstone (createTime 0) is reusable; find() probes past it
        // because its pid stays non-zero, so chains survive.
        if (entry.pid == 0 || entry.pid == pid || entry.createTime == 0) {
            entry = MetaEntry{};
            entry.pid = pid;
            entry.createTime = createTime;
            return &entry;
        }
        slot = (slot + 1) & (kMetaSlots - 1);
    }
    return nullptr;
}

// FileDescription from the version resource, the string Task Manager shows.
// This is the disk read; it only ever runs here, on the worker.
bool readDescription(const wchar_t* path, wchar_t (&out)[kFriendlyChars]) {
    DWORD handle = 0;
    const DWORD size = GetFileVersionInfoSizeW(path, &handle);
    if (size == 0 || size > (1u << 20)) return false;

    // Heap use is fine on this thread; it is the only allocation in the module.
    void* block = HeapAlloc(GetProcessHeap(), 0, size);
    if (!block) return false;
    bool ok = false;
    if (GetFileVersionInfoW(path, 0, size, block)) {
        struct LangCodepage { WORD language; WORD codepage; };
        LangCodepage* translations = nullptr;
        UINT translationBytes = 0;
        if (VerQueryValueW(block, L"\\VarFileInfo\\Translation", reinterpret_cast<void**>(&translations), &translationBytes)
            && translationBytes >= sizeof(LangCodepage)) {
            const UINT count = translationBytes / sizeof(LangCodepage);
            for (UINT i = 0; i < count && !ok; ++i) {
                wchar_t query[64];
                swprintf_s(query, L"\\StringFileInfo\\%04x%04x\\FileDescription",
                    translations[i].language, translations[i].codepage);
                wchar_t* value = nullptr;
                UINT valueChars = 0;
                if (VerQueryValueW(block, query, reinterpret_cast<void**>(&value), &valueChars) && value && valueChars > 1) {
                    wcsncpy_s(out, value, _TRUNCATE);
                    ok = out[0] != L'\0';
                }
            }
        }
    }
    HeapFree(GetProcessHeap(), 0, block);
    return ok;
}

bool resolve(uint32_t pid, wchar_t (&friendly)[kFriendlyChars]) {
    HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!process) return false;
    wchar_t path[MAX_PATH * 2];
    DWORD chars = static_cast<DWORD>(MAX_PATH * 2);
    const bool havePath = QueryFullProcessImageNameW(process, 0, path, &chars) != 0;
    CloseHandle(process);
    if (!havePath) return false;
    return readDescription(path, friendly);
}

// One pass: evict entries whose (pid, createTime) no longer match, then
// resolve a handful of unresolved processes from the latest snapshot.
void tick() {
    const Snapshot* snapshot = snapshotAcquire();
    if (!snapshot) return;

    // Same idle worker keeps the compositor resident; see rescue_residency.h.
    residencyTick(*snapshot);

    AcquireSRWLockExclusive(&g_tableLock);
    for (uint32_t i = 0; i < kMetaSlots; ++i) {
        MetaEntry& entry = g_table[i];
        if (entry.pid == 0 || entry.createTime == 0) continue;   // empty or already a tombstone
        const ProcRecord* record = findRecord(*snapshot, entry.pid);
        if (!record || record->createTime != entry.createTime) {
            // Cannot simply zero a slot in an open-addressed table without
            // breaking probe chains; mark it dead and let insert reuse it.
            entry.resolved = false;
            entry.createTime = 0;
            entry.friendly[0] = L'\0';
            g_evicted.fetch_add(1);
        }
    }
    ReleaseSRWLockExclusive(&g_tableLock);

    uint32_t budget = kResolvesPerTick;
    for (uint32_t i = 0; i < snapshot->count && budget > 0; ++i) {
        const ProcRecord& record = snapshot->records[snapshot->ranked[i]];

        AcquireSRWLockShared(&g_tableLock);
        const MetaEntry* existing = find(record.pid);
        const bool done = existing && existing->createTime == record.createTime && existing->resolved;
        ReleaseSRWLockShared(&g_tableLock);
        if (done) continue;

        wchar_t friendly[kFriendlyChars] = {};
        const bool found = resolve(record.pid, friendly);
        --budget;

        AcquireSRWLockExclusive(&g_tableLock);
        MetaEntry* entry = insert(record.pid, record.createTime);
        if (entry) {
            entry->resolved = true;
            if (found) wcscpy_s(entry->friendly, friendly);
        }
        ReleaseSRWLockExclusive(&g_tableLock);

        g_resolved.fetch_add(1);
        if (!found) g_misses.fetch_add(1);
    }

    snapshotRelease(snapshot);
}

DWORD WINAPI metaThread(LPVOID) {
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_LOWEST);
    SetThreadDescription(GetCurrentThread(), L"keyrail-rescue-meta");
    // Background mode also lowers I/O and memory priority, so the version
    // resource reads never compete with whatever the user is waiting on.
    SetThreadPriority(GetCurrentThread(), THREAD_MODE_BACKGROUND_BEGIN);
    while (WaitForSingleObject(g_stop, kTickMs) == WAIT_TIMEOUT) {
        tick();
    }
    SetThreadPriority(GetCurrentThread(), THREAD_MODE_BACKGROUND_END);
    return 0;
}

} // namespace

bool metaStart(std::wstring* report) {
    if (g_thread) return true;
    const SIZE_T bytes = sizeof(MetaEntry) * kMetaSlots;
    g_table = static_cast<MetaEntry*>(VirtualAlloc(nullptr, bytes, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
    if (!g_table) {
        if (report) *report = L"rescue name cache unavailable: allocation failed\n";
        return false;
    }
    const bool locked = VirtualLock(g_table, bytes) != 0;
    for (uint32_t i = 0; i < kMetaSlots; ++i) new (&g_table[i]) MetaEntry();

    g_stop = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    g_thread = CreateThread(nullptr, 0, metaThread, nullptr, 0, nullptr);
    if (!g_thread) {
        if (report) *report = L"rescue name cache unavailable: could not start thread\n";
        return false;
    }
    if (report) {
        *report = std::wstring(L"rescue name cache ready: ") + std::to_wstring(bytes / 1024) + L" KB"
            + (locked ? L" locked" : L" NOT locked") + L", " + std::to_wstring(kResolvesPerTick) + L" lookups/tick\n";
    }
    return true;
}

void metaStop() {
    if (g_thread) {
        SetEvent(g_stop);
        WaitForSingleObject(g_thread, 5000);
        CloseHandle(g_thread);
        CloseHandle(g_stop);
        g_thread = nullptr;
        g_stop = nullptr;
    }
    if (g_table) {
        VirtualFree(g_table, 0, MEM_RELEASE);
        g_table = nullptr;
    }
}

bool metaLookup(uint32_t pid, int64_t createTime, wchar_t (&out)[kFriendlyChars]) {
    out[0] = 0;
    if (!g_table) return false;
    // Shared lock: the worker holds the exclusive side only for a few slot
    // writes per second, never across the disk read, so the panic path does
    // not wait on I/O here.
    AcquireSRWLockShared(&g_tableLock);
    const MetaEntry* entry = find(pid);
    const bool hit = entry && entry->createTime == createTime && entry->resolved && entry->friendly[0];
    if (hit) wcscpy_s(out, entry->friendly);
    ReleaseSRWLockShared(&g_tableLock);
    return hit;
}

MetaStatus metaStatus() {
    MetaStatus status;
    status.resolved = g_resolved.load();
    status.misses = g_misses.load();
    status.evicted = g_evicted.load();
    return status;
}

} // namespace rescue
