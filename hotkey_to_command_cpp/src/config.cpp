#include "config.h"

#include <windows.h>
#include <nlohmann/json.hpp>
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <sstream>

using nlohmann::json;

static std::string wideToUtf8(const std::wstring& value) {
    if (value.empty()) return {};
    int size = WideCharToMultiByte(CP_UTF8, 0, value.c_str(), -1, nullptr, 0, nullptr, nullptr);
    if (size <= 0) return {};
    std::string out(size, '\0');
    WideCharToMultiByte(CP_UTF8, 0, value.c_str(), -1, out.data(), size, nullptr, nullptr);
    out.pop_back();
    return out;
}

static std::wstring utf8ToWide(const std::string& value) {
    if (value.empty()) return {};
    int size = MultiByteToWideChar(CP_UTF8, 0, value.c_str(), -1, nullptr, 0);
    if (size <= 0) return {};
    std::wstring out(size, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, value.c_str(), -1, out.data(), size);
    out.pop_back();
    return out;
}

static std::wstring appDataPath() {
    DWORD needed = GetEnvironmentVariableW(L"APPDATA", nullptr, 0);
    if (needed == 0) return L".";
    std::wstring path(needed - 1, L'\0');
    GetEnvironmentVariableW(L"APPDATA", path.data(), needed);
    return path;
}

std::wstring configPath() {
    return appDataPath() + L"\\HotkeyToCommand\\config.json";
}

std::wstring defaultConfigText() {
    return LR"({
  "version": 1,
  "settings": {
    "hotkey_mode": "global",
    "command_hotkey": "ctrl+alt+space",
    "command_timeout_ms": 4000
  },
  "onboarding": {
    "completed": false,
    "version": 1,
    "seen_media_picker_extension": false,
    "seen_discord_bridge": false
  },
  "bindings": [
    {
      "id": "cycle-audio",
      "enabled": true,
      "hotkey": "ctrl+alt+o",
      "action": {
        "type": "builtin",
        "name": "cycle_audio_output"
      }
    },
    {
      "id": "open-notepad",
      "enabled": true,
      "hotkey": "ctrl+alt+t",
      "action": {
        "type": "open_app",
        "path": "C:\\Windows\\System32\\notepad.exe",
        "args": [],
        "show_window": true
      }
    },
    {
      "id": "media-picker",
      "enabled": true,
      "hotkey": "ctrl+alt+m",
      "action": {
        "type": "builtin",
        "name": "media_picker_open"
      }
    },
    {
      "id": "media-next",
      "enabled": true,
      "hotkey": "media_next",
      "action": {
        "type": "builtin",
        "name": "media_next_contextual"
      }
    },
    {
      "id": "media-previous",
      "enabled": true,
      "hotkey": "media_previous",
      "action": {
        "type": "builtin",
        "name": "media_previous_contextual"
      }
    },
    {
      "id": "media-play-pause",
      "enabled": true,
      "hotkey": "media_play_pause",
      "action": {
        "type": "builtin",
        "name": "media_play_pause_contextual"
      }
    },
    {
      "id": "media-picker-cancel",
      "enabled": true,
      "hotkey": "media_stop",
      "action": {
        "type": "builtin",
        "name": "media_picker_cancel"
      }
    }
  ]
}
)";
}

static bool ensureDefaultConfig(const std::wstring& path, std::wstring* error) {
    std::filesystem::path fsPath(path);
    std::error_code ec;
    std::filesystem::create_directories(fsPath.parent_path(), ec);
    if (ec) {
        if (error) *error = L"could not create config directory";
        return false;
    }
    if (std::filesystem::exists(fsPath)) return true;

    std::ofstream out(fsPath, std::ios::binary);
    if (!out) {
        if (error) *error = L"could not create default config";
        return false;
    }
    std::string text = wideToUtf8(defaultConfigText());
    out.write(text.data(), static_cast<std::streamsize>(text.size()));
    return true;
}

static std::wstring readString(const json& obj, const char* key) {
    auto it = obj.find(key);
    if (it == obj.end() || !it->is_string()) return {};
    return utf8ToWide(it->get<std::string>());
}

static ActionSpec parseAction(const json& obj) {
    ActionSpec action;
    if (!obj.is_object()) return action;
    action.type = readString(obj, "type");
    action.name = readString(obj, "name");
    action.path = readString(obj, "path");
    action.command = readString(obj, "command");
    action.workingDir = readString(obj, "working_dir");
    action.interpreter = readString(obj, "interpreter");
    auto show = obj.find("show_window");
    if (show != obj.end() && show->is_boolean()) action.showWindow = show->get<bool>();
    auto strongClose = obj.find("strong_close");
    if (strongClose != obj.end() && strongClose->is_boolean()) action.strongClose = strongClose->get<bool>();

    auto args = obj.find("args");
    if (args != obj.end() && args->is_array()) {
        for (const auto& arg : *args) {
            if (arg.is_string()) action.args.push_back(utf8ToWide(arg.get<std::string>()));
        }
    }
    return action;
}

static BindingSpec parseBinding(const json& obj) {
    BindingSpec binding;
    binding.id = readString(obj, "id");
    binding.hotkey = readString(obj, "hotkey");
    auto enabled = obj.find("enabled");
    if (enabled != obj.end() && enabled->is_boolean()) binding.enabled = enabled->get<bool>();
    auto action = obj.find("action");
    if (action != obj.end()) binding.action = parseAction(*action);
    return binding;
}

static AppSettings parseSettings(const json& obj) {
    AppSettings settings;
    if (!obj.is_object()) return settings;

    std::wstring mode = readString(obj, "hotkey_mode");
    if (mode == L"global" || mode == L"command") settings.hotkeyMode = mode;

    std::wstring commandHotkey = readString(obj, "command_hotkey");
    if (!commandHotkey.empty()) settings.commandHotkey = commandHotkey;

    auto timeout = obj.find("command_timeout_ms");
    if (timeout != obj.end() && timeout->is_number_integer()) {
        settings.commandTimeoutMs = (std::max)(1000, (std::min)(timeout->get<int>(), 15000));
    }

    return settings;
}

ConfigLoadResult loadConfig() {
    ConfigLoadResult result;
    result.path = configPath();

    std::wstring setupError;
    if (!ensureDefaultConfig(result.path, &setupError)) {
        result.message = setupError;
        return result;
    }

    try {
        std::ifstream in(std::filesystem::path(result.path), std::ios::binary);
        if (!in) {
            result.message = L"could not open config";
            return result;
        }

        json root = json::parse(in);
        if (!root.is_object()) {
            result.message = L"config root must be an object";
            return result;
        }

        AppConfig config;
        auto version = root.find("version");
        if (version != root.end() && version->is_number_integer()) config.version = version->get<int>();

        auto settings = root.find("settings");
        if (settings != root.end()) config.settings = parseSettings(*settings);

        auto bindings = root.find("bindings");
        if (bindings == root.end() || !bindings->is_array()) {
            result.message = L"config must contain a bindings array";
            return result;
        }

        for (const auto& item : *bindings) {
            if (item.is_object()) config.bindings.push_back(parseBinding(item));
        }

        result.ok = true;
        result.config = std::move(config);
        result.message = L"loaded config";
        return result;
    } catch (const std::exception& ex) {
        result.message = L"config parse failed: " + utf8ToWide(ex.what());
        return result;
    }
}
