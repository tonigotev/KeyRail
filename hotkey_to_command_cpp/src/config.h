#pragma once

#include <string>
#include <vector>

struct ActionSpec {
    std::wstring type;
    std::wstring name;
    std::wstring path;
    std::wstring command;
    std::vector<std::wstring> args;
    bool showWindow = false;
};

struct BindingSpec {
    std::wstring id;
    bool enabled = true;
    std::wstring hotkey;
    ActionSpec action;
};

struct AppConfig {
    int version = 1;
    std::vector<BindingSpec> bindings;
};

struct ConfigLoadResult {
    bool ok = false;
    AppConfig config;
    std::wstring path;
    std::wstring message;
};

std::wstring configPath();
std::wstring defaultConfigText();
ConfigLoadResult loadConfig();
