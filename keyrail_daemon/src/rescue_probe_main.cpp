// rescue_probe_main.cpp -- console dump of the rescue sampler's ranked table.
//
// This is the Phase 1 gate for the rescue menu: run the stressors from
// rescue_stress.exe and check that each one lands at the top for the right
// reason. It links the same sampler the daemon uses, so what it prints is what
// the overlay will show.
//
//   rescue_probe.exe [--rows N] [--interval MS] [--all] [--once | --ticks N]

#include "rescue_sampler.h"

#include <windows.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <io.h>
#include <string>

static volatile bool g_stop = false;

static BOOL WINAPI onConsoleCtrl(DWORD) {
    g_stop = true;
    return TRUE;
}

static void formatBytes(uint64_t bytes, wchar_t* out, size_t chars) {
    if (bytes >= (1ull << 30)) swprintf_s(out, chars, L"%.1fG", bytes / 1073741824.0);
    else if (bytes >= (1ull << 20)) swprintf_s(out, chars, L"%lluM", bytes >> 20);
    else swprintf_s(out, chars, L"%lluK", bytes >> 10);
}

static void formatFlags(const rescue::ProcRecord& record, wchar_t* out, size_t chars) {
    std::wstring flags;
    if (record.flags & rescue::kFlagHungWindow) flags += L"HUNG ";
    else if (record.flags & rescue::kFlagHungByState) flags += L"HUNG? ";
    if (record.flags & rescue::kFlagPaging) flags += L"PAGING ";
    if (record.flags & rescue::kFlagOwnsWindow) flags += L"WIN ";
    if (record.flags & rescue::kFlagNew) flags += L"NEW ";
    if (record.flags & rescue::kFlagSelf) flags += L"SELF ";
    if (record.flags & rescue::kFlagRealtime) flags += L"RT ";
    if (record.flags & rescue::kFlagIoStorm) flags += L"IO ";
    if (record.flags & rescue::kFlagSpawnStorm) flags += L"SPAWN ";
    if (record.flags & rescue::kFlagRealtime) flags += L"RT ";
    if (record.flags & rescue::kFlagIoStorm) flags += L"IO ";
    if (record.flags & rescue::kFlagSpawnStorm) flags += L"SPAWN ";
    if (!flags.empty()) flags.pop_back();
    wcsncpy_s(out, chars, flags.c_str(), _TRUNCATE);
}

static void printSnapshot(const rescue::Snapshot& snapshot, uint32_t rows) {
    const rescue::SamplerStatus status = rescue::samplerStatus();

    wprintf(L"\n=== tick %u  procs %u  threads %u  cores %u  elapsed %.3fs  sample %uus  windows %uus%s  missed %u  buffer %uKB%s ===\n",
        snapshot.sequence, snapshot.count, snapshot.threadCount, snapshot.cores, snapshot.elapsedSec,
        snapshot.tickMicros, snapshot.windowPassMicros,
        snapshot.windowPassTruncated ? L" (TRUNCATED)" : L"",
        status.missedTicks, status.bufferBytes / 1024,
        status.memoryLocked ? L" locked" : L" UNLOCKED");
    wprintf(L"norm bases: hard faults %.0f/s  priv growth %.1fMB/s  thread growth %.1f/s  io %.1fMB/s   totals: hard %.0f/s  io %.1fMB/s  cpu %.2f cores\n",
        snapshot.maxHardFaults, snapshot.maxPrivGrowth / 1048576.0, snapshot.maxThreadGrowth, snapshot.maxIoBytes / 1048576.0,
        snapshot.totalHardFaults, snapshot.totalIoBytes / 1048576.0, snapshot.totalCpuCores);
    rescue::Culprit why = rescue::Culprit::None;
    const rescue::ProcRecord* culprit = rescue::findCulprit(snapshot, &why);
    if (culprit) wprintf(L"CULPRIT: %u %ls - %ls\n", culprit->pid, culprit->image, rescue::culpritName(why));
    else wprintf(L"culprit: none (menu only)\n");
    wprintf(L"%-6ls %-22ls %7ls %8ls %8ls %8ls %8ls %-11ls %-17ls %6ls  %ls\n",
        L"PID", L"IMAGE", L"CPU%", L"PRIV", L"HFLT/s", L"SFLT/s", L"IO MB/s", L"THR R/W/P", L"FLAGS", L"SCORE", L"PARENT");

    const uint32_t limit = rows < snapshot.count ? rows : snapshot.count;
    for (uint32_t i = 0; i < limit; ++i) {
        const rescue::ProcRecord& record = snapshot.records[snapshot.ranked[i]];
        wchar_t priv[16];
        wchar_t flags[32];
        wchar_t threads[16];
        formatBytes(record.privateBytes, priv, 16);
        formatFlags(record, flags, 32);
        swprintf_s(threads, L"%u/%u/%u", record.runnable, record.waiting, record.paging);

        const rescue::ProcRecord* parent = rescue::findRecord(snapshot, record.parentPid);
        const wchar_t* parentName = L"";
        if (parent && parent->createTime <= record.createTime) parentName = parent->image;

        wprintf(L"%-6u %-22ls %7.1f %8ls %8.0f %8.0f %8.1f %-11ls %-17ls %6.2f  %ls\n",
            record.pid, record.image, record.cpuCores * 100.0, priv,
            record.hardFaultsPerSec, record.softFaultsPerSec, record.ioBytesPerSec / 1048576.0, threads, flags, record.score, parentName);
    }
}

int wmain(int argc, wchar_t** argv) {
    _setmode(_fileno(stdout), _O_U16TEXT);

    uint32_t rows = 15;
    int ticks = 0;   // 0 = until Ctrl+C
    RescueSettings settings;

    for (int i = 1; i < argc; ++i) {
        if (!wcscmp(argv[i], L"--rows") && i + 1 < argc) rows = static_cast<uint32_t>(_wtoi(argv[++i]));
        else if (!wcscmp(argv[i], L"--interval") && i + 1 < argc) settings.sampleIntervalMs = _wtoi(argv[++i]);
        else if (!wcscmp(argv[i], L"--all")) rows = rescue::kMaxRecords;
        else if (!wcscmp(argv[i], L"--once")) ticks = 1;
        else if (!wcscmp(argv[i], L"--ticks") && i + 1 < argc) ticks = _wtoi(argv[++i]);
        else {
            wprintf(L"usage: rescue_probe [--rows N] [--interval MS] [--all] [--once | --ticks N]\n");
            return 2;
        }
    }
    if (settings.sampleIntervalMs < 250) settings.sampleIntervalMs = 250;

    std::wstring report;
    if (!rescue::samplerStart(settings, &report)) {
        wprintf(L"%ls", report.c_str());
        return 1;
    }
    wprintf(L"%ls", report.c_str());
    wprintf(L"CPU%% is percent of one core (100 = one core pegged). Ctrl+C to stop.\n");

    SetConsoleCtrlHandler(onConsoleCtrl, TRUE);

    // The first snapshot has no deltas; wait for the second before printing.
    uint32_t lastSequence = 0;
    int printed = 0;
    while (!g_stop) {
        Sleep(100);
        const rescue::Snapshot* snapshot = rescue::snapshotAcquire();
        if (!snapshot) continue;
        if (snapshot->sequence != lastSequence && snapshot->sequence >= 2) {
            lastSequence = snapshot->sequence;
            printSnapshot(*snapshot, rows);
            ++printed;
        }
        rescue::snapshotRelease(snapshot);
        if (ticks > 0 && printed >= ticks) break;
    }

    rescue::samplerStop();
    return 0;
}
