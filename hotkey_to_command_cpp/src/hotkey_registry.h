#pragma once

#include "config.h"
#include "runners.h"

#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

class HotkeyRegistry {
public:
    ~HotkeyRegistry();

    std::wstring apply(const AppConfig& config);
    bool dispatch(WPARAM hotkeyId) const;
    void clear();

private:
    std::vector<int> registeredIds_;
    std::vector<std::unique_ptr<Runnable>> actions_;
    std::unordered_map<int, Runnable*> dispatch_;
};
