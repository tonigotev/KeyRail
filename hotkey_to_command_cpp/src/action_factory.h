#pragma once

#include "config.h"
#include "runners.h"

#include <memory>
#include <string>

struct ActionBuildResult {
    std::unique_ptr<Runnable> action;
    std::wstring error;
};

ActionBuildResult makeAction(const ActionSpec& spec);
