#pragma once
// rescue_meta.h -- Tier 2: names that live on disk.
//
// "Google Chrome" instead of chrome.exe costs a file open and a version
// resource read per process. That is exactly the work Task Manager does per
// row and exactly what queues behind a saturated disk, so it is never done on
// the panic path. A low-priority worker fills a cache in idle time, a few
// processes per tick; the overlay does a hash lookup and shows the image name
// on a miss. A miss is the correct result, not a bug.

#include "rescue_sampler.h"

#include <windows.h>

#include <cstdint>
#include <string>

namespace rescue {

constexpr uint32_t kFriendlyChars = 40;

struct MetaEntry {
    uint32_t pid = 0;
    int64_t createTime = 0;
    bool resolved = false;              // looked up, possibly with nothing found
    wchar_t friendly[kFriendlyChars] = {};
};

bool metaStart(std::wstring* report);
void metaStop();

// Hash lookup only; copies the friendly name out. False on a miss.
bool metaLookup(uint32_t pid, int64_t createTime, wchar_t (&out)[kFriendlyChars]);

struct MetaStatus {
    uint32_t resolved = 0;
    uint32_t misses = 0;      // resolved with no description
    uint32_t evicted = 0;
};
MetaStatus metaStatus();

} // namespace rescue
