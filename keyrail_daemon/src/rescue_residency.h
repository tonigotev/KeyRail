#pragma once
// rescue_residency.h -- keep the display pipeline in RAM.
//
// Measured under a real thrash: our own path from keypress to pixels is well
// under a second, and SwitchDesktop then takes 25 seconds because Windows has
// to wait for dwm.exe, the compositor, to be paged back in from disk. Nothing
// we lock in our own process helps with that. What does help is giving DWM a
// hard working-set minimum, the same protection the daemon gives itself, so
// the memory manager never trims the compositor out under pressure. That
// needs an elevated daemon, and it is re-applied whenever DWM restarts.

#include "rescue_sampler.h"

#include <cstdint>
#include <string>

namespace rescue {

struct ResidencyStatus {
    bool attempted = false;
    bool pinned = false;
    uint32_t compositorPid = 0;
    uint64_t pinnedBytes = 0;
    wchar_t detail[120] = {};
};

// Called by the idle worker with each snapshot. Does nothing until the
// compositor's pid changes, so the cost is one string compare per tick.
void residencyTick(const Snapshot& snapshot);

ResidencyStatus residencyStatus();

} // namespace rescue
