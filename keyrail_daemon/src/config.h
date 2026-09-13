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
    bool pushToTalkOverlay = false;
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

// Weights for the rescue menu's ranking. Hung state and hard page faults carry
// the most weight because those are the two things that actually wedge a
// desktop; CPU, private-bytes growth and thread growth refine the order.
struct RescueWeights {
    float hung = 5.0f;
    float faults = 4.0f;
    float cpu = 3.0f;
    float priv = 2.0f;
    float threads = 1.0f;
    float io = 2.0f;        // disk bytes/s: a storm starves everyone else's page-ins
};

struct RescueSettings {
    bool enabled = true;
    std::wstring hotkey = L"ctrl+alt+shift+end";
    int sampleIntervalMs = 1000;
    int rowCount = 15;
    bool usePrivateDesktop = true;
    int titleDeadlineMs = 50;
    // On the chord, pause a process the sampler is sure is freezing the
    // machine before showing the menu; Esc resumes it. See findCulprit.
    bool autoPause = true;
    RescueWeights weights;
};

struct AppConfig {
    int version = 1;
    AppSettings settings;
    RescueSettings rescue;
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
