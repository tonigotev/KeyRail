#pragma once

#include <string>
#include <vector>

struct ActionSpec {
    std::wstring type;
    std::wstring name;
    std::wstring path;
    std::wstring command;
    std::wstring workingDir;
    std::wstring interpreter;
    std::vector<std::wstring> args;
    bool showWindow = false;
    bool strongClose = false;
};

struct BindingSpec {
    std::wstring id;
    bool enabled = true;
    std::wstring hotkey;
    ActionSpec action;
};

struct AppSettings {
    std::wstring hotkeyMode = L"global";
    std::wstring commandHotkey = L"ctrl+alt+space";
    int commandTimeoutMs = 4000;
};

struct AppConfig {
    int version = 1;
    AppSettings settings;
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
