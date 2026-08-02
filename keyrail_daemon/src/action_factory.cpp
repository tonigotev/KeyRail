#include "action_factory.h"

#include "app_audio.h"
#include "audio.h"
#include "clipboard_history.h"
#include "discord_bridge.h"
#include "media_sessions.h"

#include <windows.h>

#include <algorithm>
#include <array>
#include <cstdio>
#include <cwctype>
#include <filesystem>
#include <sstream>

static std::wstring lowerCopy(std::wstring value) {
    std::transform(value.begin(), value.end(), value.begin(), [](wchar_t c) {
        return static_cast<wchar_t>(towlower(c));
    });
    return value;
}

static std::wstring extensionOf(const std::wstring& path) {
    return lowerCopy(std::filesystem::path(path).extension().wstring());
}

static std::unique_ptr<Runnable> makeScriptAction(const ActionSpec& spec) {
    const std::wstring interpreter = lowerCopy(spec.interpreter);
    if (interpreter == L"direct") {
        return std::make_unique<ExecutableScript>(spec.path, spec.args, spec.showWindow, spec.workingDir);
    }
    if (!spec.interpreter.empty() && interpreter != L"auto") {
        return std::make_unique<PlannedScript>(
            spec.interpreter,
            std::vector<std::wstring>{},
            spec.path,
            spec.args,
            spec.showWindow,
            spec.workingDir);
    }

    const std::wstring ext = extensionOf(spec.path);
    if (ext == L".exe" || ext == L".com") {
        return std::make_unique<ExecutableScript>(spec.path, spec.args, spec.showWindow, spec.workingDir);
    }
    if (ext == L".py" || ext == L".pyw") {
        return std::make_unique<PlannedScript>(L"py.exe", std::vector<std::wstring>{L"-3"}, spec.path, spec.args, spec.showWindow, spec.workingDir);
    }
    if (ext == L".ps1") {
        return std::make_unique<PlannedScript>(
            L"powershell.exe",
            std::vector<std::wstring>{L"-NoProfile", L"-ExecutionPolicy", L"Bypass", L"-File"},
            spec.path,
            spec.args,
            spec.showWindow,
            spec.workingDir);
    }
    if (ext == L".bat" || ext == L".cmd") {
        return std::make_unique<PlannedScript>(L"cmd.exe", std::vector<std::wstring>{L"/c"}, spec.path, spec.args, spec.showWindow, spec.workingDir);
    }
    if (ext == L".js" || ext == L".mjs" || ext == L".cjs") {
        return std::make_unique<PlannedScript>(L"node.exe", std::vector<std::wstring>{}, spec.path, spec.args, spec.showWindow, spec.workingDir);
    }
    if (ext == L".vbs" || ext == L".wsf") {
        return std::make_unique<PlannedScript>(L"wscript.exe", std::vector<std::wstring>{}, spec.path, spec.args, spec.showWindow, spec.workingDir);
    }
    if (ext == L".ahk") {
        return std::make_unique<PlannedScript>(L"AutoHotkey.exe", std::vector<std::wstring>{}, spec.path, spec.args, spec.showWindow, spec.workingDir);
    }

    return std::make_unique<ExecutableScript>(spec.path, spec.args, spec.showWindow, spec.workingDir);
}

static std::wstring focusedProcessName(DWORD pid) {
    HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!process) return {};

    wchar_t path[MAX_PATH]{};
    DWORD size = MAX_PATH;
    std::wstring name;
    if (QueryFullProcessImageNameW(process, 0, path, &size)) {
        name = lowerCopy(std::filesystem::path(std::wstring(path, size)).filename().wstring());
    }

    CloseHandle(process);
    return name;
}

static bool isProtectedProcess(const std::wstring& name) {
    static constexpr std::array<const wchar_t*, 12> protectedNames = {
        L"csrss.exe",
        L"dwm.exe",
        L"explorer.exe",
        L"keyrail_ui.exe",
        L"keyraild.exe",
        L"lsass.exe",
        L"services.exe",
        L"smss.exe",
        L"system",
        L"system idle process",
        L"wininit.exe",
        L"winlogon.exe",
    };

    return std::any_of(protectedNames.begin(), protectedNames.end(), [&](const wchar_t* protectedName) {
        return name == protectedName;
    });
}

static bool isDiscordProcess(const std::wstring& exeName) {
    const std::wstring name = lowerCopy(exeName);
    return name == L"discord.exe"
        || name == L"discordptb.exe"
        || name == L"discordcanary.exe"
        || name == L"discorddevelopment.exe"
        || name == L"discordsystemhelper.exe";
}

static bool focusedWindowProcess(DWORD* pid, std::wstring* name, std::wstring* report) {
    HWND hwnd = GetForegroundWindow();
    if (!hwnd) {
        if (report) *report = L"No focused window was found.";
        return false;
    }

    DWORD focusedPid = 0;
    GetWindowThreadProcessId(hwnd, &focusedPid);
    if (focusedPid == 0) {
        if (report) *report = L"Focused window does not expose a process id.";
        return false;
    }

    std::wstring exeName = focusedProcessName(focusedPid);
    if (exeName.empty()) {
        if (report) *report = L"Could not read the focused app process name.";
        return false;
    }

    if (pid) *pid = focusedPid;
    if (name) *name = exeName;
    return true;
}

static bool closeFocusedApp(bool strong, std::wstring* report) {
    DWORD pid = 0;
    std::wstring name;
    if (!focusedWindowProcess(&pid, &name, report)) return false;

    if (isProtectedProcess(name)) {
        if (report) *report = L"Blocked close request for protected app: " + name;
        return false;
    }

    HWND hwnd = GetForegroundWindow();
    if (!hwnd) {
        if (report) *report = L"No focused window was found.";
        return false;
    }

    if (strong) {
        DWORD_PTR result = 0;
        LRESULT sent = SendMessageTimeoutW(hwnd, WM_CLOSE, 0, 0, SMTO_ABORTIFHUNG, 1500, &result);
        if (!sent) {
            if (report) *report = L"Focused app did not respond to the close request: " + name;
            return false;
        }
        if (report) *report = L"Sent stronger close request to " + name + L" (pid " + std::to_wstring(pid) + L")";
        return true;
    }

    if (!PostMessageW(hwnd, WM_CLOSE, 0, 0)) {
        std::wstringstream out;
        out << L"Could not send close request to " << name << L" (Windows error " << GetLastError() << L")";
        if (report) *report = out.str();
        return false;
    }

    if (report) *report = L"Sent polite close request to " + name + L" (pid " + std::to_wstring(pid) + L")";
    return true;
}

static bool killFocusedApp(std::wstring* report) {
    DWORD pid = 0;
    std::wstring name;
    if (!focusedWindowProcess(&pid, &name, report)) return false;

    if (pid == GetCurrentProcessId() || isProtectedProcess(name)) {
        if (report) *report = L"Blocked force-end for protected app: " + name;
        return false;
    }

    HANDLE process = OpenProcess(PROCESS_TERMINATE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!process) {
        std::wstringstream out;
        out << L"Could not open " << name << L" for force-end (Windows error " << GetLastError() << L")";
        if (report) *report = out.str();
        return false;
    }

    BOOL terminated = TerminateProcess(process, 1);
    DWORD error = terminated ? 0 : GetLastError();
    CloseHandle(process);

    if (!terminated) {
        std::wstringstream out;
        out << L"Could not force-end " << name << L" (Windows error " << error << L")";
        if (report) *report = out.str();
        return false;
    }

    if (report) *report = L"Force-ended " + name + L" (pid " + std::to_wstring(pid) + L")";
    return true;
}

ActionBuildResult makeAction(const ActionSpec& spec) {
    if (spec.type == L"builtin") {
        if (spec.name == L"cycle_audio_output") {
            return {std::make_unique<BuiltinAction>([] {
                        std::wstring name;
                        if (cycleAudioDevice(&name)) {
                            wprintf(L"  -> audio output: %s\n", name.c_str());
                        } else {
                            wprintf(L"  ! audio switch failed\n");
                        }
                    }),
                    L""};
        }
        if (spec.name == L"cycle_microphone_input") {
            return {std::make_unique<BuiltinAction>([] {
                        std::wstring name;
                        if (cycleMicrophoneDevice(&name)) {
                            wprintf(L"  -> microphone input: %s\n", name.c_str());
                        } else {
                            wprintf(L"  ! microphone switch failed\n");
                        }
                    }),
                    L""};
        }
        if (spec.name == L"inspect_focused_app_audio") {
            return {std::make_unique<BuiltinAction>([] {
                        std::wstring report;
                        if (describeFocusedAppAudio(&report)) {
                            wprintf(L"\n%s\n", report.c_str());
                            MessageBoxW(nullptr, report.c_str(), L"Focused App Audio", MB_OK | MB_SETFOREGROUND);
                        } else {
                            wprintf(L"  ! %s\n", report.c_str());
                            MessageBoxW(nullptr, report.c_str(), L"Focused App Audio", MB_OK | MB_ICONWARNING | MB_SETFOREGROUND);
                        }
                    }),
                    L""};
        }
        if (spec.name == L"cycle_focused_app_audio_output") {
            return {std::make_unique<BuiltinAction>([] {
                        std::wstring report;
                        bool ok = false;
                        if (isDiscordProcess(focusedAppExeName())) {
                            ok = sendDiscordBridgeCommand("cycle_output_device", &report);
                        } else {
                            ok = cycleFocusedAppAudioOutput(&report);
                        }

                        if (ok) {
                            wprintf(L"  -> %s\n", report.c_str());
                        } else {
                            wprintf(L"  ! %s\n", report.c_str());
                        }
                    }),
                    L""};
        }
        if (spec.name == L"cycle_focused_app_microphone_input") {
            return {std::make_unique<BuiltinAction>([] {
                        std::wstring report;
                        bool ok = false;
                        if (isDiscordProcess(focusedAppExeName())) {
                            ok = sendDiscordBridgeCommand("cycle_input_device", &report);
                        } else {
                            std::wstring name;
                            ok = cycleMicrophoneDevice(&name);
                            report = ok ? L"microphone input: " + name : L"microphone switch failed";
                        }

                        if (ok) {
                            wprintf(L"  -> %s\n", report.c_str());
                        } else {
                            wprintf(L"  ! %s\n", report.c_str());
                        }
                    }),
                    L""};
        }
        if (spec.name == L"cycle_discord_output_device") {
            return {std::make_unique<BuiltinAction>([] {
                        std::wstring report;
                        bool ok = sendDiscordBridgeCommand("cycle_output_device", &report);
                        wprintf(L"  -> %s\n", report.c_str());
                        if (!ok) {
                            MessageBoxW(nullptr, report.c_str(), L"Discord Output Bridge", MB_OK | MB_ICONWARNING | MB_SETFOREGROUND);
                        }
                    }),
                    L""};
        }
        if (spec.name == L"cycle_discord_microphone_input") {
            return {std::make_unique<BuiltinAction>([] {
                        std::wstring report;
                        bool ok = sendDiscordBridgeCommand("cycle_input_device", &report);
                        wprintf(L"  -> %s\n", report.c_str());
                        if (!ok) {
                            MessageBoxW(nullptr, report.c_str(), L"Discord Microphone Bridge", MB_OK | MB_ICONWARNING | MB_SETFOREGROUND);
                        }
                    }),
                    L""};
        }
        if (spec.name == L"media_picker_open") {
            return {std::make_unique<BuiltinAction>([] {
                        std::wstring report;
                        if (openMediaPicker(&report)) {
                            wprintf(L"  -> %s\n", report.c_str());
                        } else {
                            wprintf(L"  ! %s\n", report.c_str());
                        }
                    }),
                    L""};
        }
        if (spec.name == L"media_picker_next") {
            return {std::make_unique<BuiltinAction>([] {
                        std::wstring report;
                        if (moveMediaPicker(1, &report)) {
                            wprintf(L"  -> %s\n", report.c_str());
                        } else {
                            wprintf(L"  ! %s\n", report.c_str());
                        }
                    }),
                    L""};
        }
        if (spec.name == L"media_picker_previous") {
            return {std::make_unique<BuiltinAction>([] {
                        std::wstring report;
                        if (moveMediaPicker(-1, &report)) {
                            wprintf(L"  -> %s\n", report.c_str());
                        } else {
                            wprintf(L"  ! %s\n", report.c_str());
                        }
                    }),
                    L""};
        }
        if (spec.name == L"media_picker_confirm") {
            return {std::make_unique<BuiltinAction>([] {
                        std::wstring report;
                        if (confirmMediaPicker(&report)) {
                            wprintf(L"  -> %s\n", report.c_str());
                        } else {
                            wprintf(L"  ! %s\n", report.c_str());
                        }
                    }),
                    L""};
        }
        if (spec.name == L"media_picker_cancel") {
            return {std::make_unique<BuiltinAction>([] {
                        std::wstring report;
                        cancelMediaPicker(&report);
                        wprintf(L"  -> %s\n", report.c_str());
                    }),
                    L""};
        }
        if (spec.name == L"media_selected_play_pause") {
            return {std::make_unique<BuiltinAction>([] {
                        std::wstring report;
                        if (controlSelectedMedia(MediaCommand::TogglePlayPause, &report)) {
                            wprintf(L"  -> %s\n", report.c_str());
                        } else {
                            wprintf(L"  ! %s\n", report.c_str());
                        }
                    }),
                    L""};
        }
        if (spec.name == L"media_selected_next") {
            return {std::make_unique<BuiltinAction>([] {
                        std::wstring report;
                        if (controlSelectedMedia(MediaCommand::Next, &report)) {
                            wprintf(L"  -> %s\n", report.c_str());
                        } else {
                            wprintf(L"  ! %s\n", report.c_str());
                        }
                    }),
                    L""};
        }
        if (spec.name == L"media_selected_previous") {
            return {std::make_unique<BuiltinAction>([] {
                        std::wstring report;
                        if (controlSelectedMedia(MediaCommand::Previous, &report)) {
                            wprintf(L"  -> %s\n", report.c_str());
                        } else {
                            wprintf(L"  ! %s\n", report.c_str());
                        }
                    }),
                    L""};
        }
        if (spec.name == L"media_next_contextual") {
            return {std::make_unique<BuiltinAction>([] {
                        std::wstring report;
                        if (mediaNextContextual(&report)) {
                            wprintf(L"  -> %s\n", report.c_str());
                        } else {
                            wprintf(L"  ! %s\n", report.c_str());
                        }
                    }),
                    L""};
        }
        if (spec.name == L"media_previous_contextual") {
            return {std::make_unique<BuiltinAction>([] {
                        std::wstring report;
                        if (mediaPreviousContextual(&report)) {
                            wprintf(L"  -> %s\n", report.c_str());
                        } else {
                            wprintf(L"  ! %s\n", report.c_str());
                        }
                    }),
                    L""};
        }
        if (spec.name == L"media_play_pause_contextual") {
            return {std::make_unique<BuiltinAction>([] {
                        std::wstring report;
                        if (mediaPlayPauseContextual(&report)) {
                            wprintf(L"  -> %s\n", report.c_str());
                        } else {
                            wprintf(L"  ! %s\n", report.c_str());
                        }
                    }),
                    L""};
        }
        if (spec.name == L"inspect_media_sessions") {
            return {std::make_unique<BuiltinAction>([] {
                        std::wstring report = describeMediaTargets();
                        wprintf(L"\n%s\n", report.c_str());
                        MessageBoxW(nullptr, report.c_str(), L"Media Sessions", MB_OK | MB_SETFOREGROUND);
                    }),
                    L""};
        }
        if (spec.name == L"clipboard_history_open") {
            return {std::make_unique<BuiltinAction>([] {
                        std::wstring report;
                        if (openClipboardHistoryPicker(&report)) {
                            wprintf(L"  -> %s\n", report.c_str());
                        } else {
                            wprintf(L"  ! %s\n", report.c_str());
                        }
                    }),
                    L""};
        }
        if (spec.name == L"clipboard_quick_open") {
            return {std::make_unique<BuiltinAction>([] {
                        std::wstring report;
                        if (openClipboardQuickPicker(&report)) {
                            wprintf(L"  -> %s\n", report.c_str());
                        } else {
                            wprintf(L"  ! %s\n", report.c_str());
                        }
                    }),
                    L""};
        }
        if (spec.name == L"clipboard_history_next") {
            return {std::make_unique<BuiltinAction>([] {
                        std::wstring report;
                        if (moveClipboardHistoryPicker(1, &report)) {
                            wprintf(L"  -> %s\n", report.c_str());
                        } else {
                            wprintf(L"  ! %s\n", report.c_str());
                        }
                    }),
                    L""};
        }
        if (spec.name == L"clipboard_history_previous") {
            return {std::make_unique<BuiltinAction>([] {
                        std::wstring report;
                        if (moveClipboardHistoryPicker(-1, &report)) {
                            wprintf(L"  -> %s\n", report.c_str());
                        } else {
                            wprintf(L"  ! %s\n", report.c_str());
                        }
                    }),
                    L""};
        }
        if (spec.name == L"clipboard_history_confirm") {
            return {std::make_unique<BuiltinAction>([] {
                        std::wstring report;
                        if (confirmClipboardHistoryPicker(&report)) {
                            wprintf(L"  -> %s\n", report.c_str());
                        } else {
                            wprintf(L"  ! %s\n", report.c_str());
                        }
                    }),
                    L""};
        }
        if (spec.name == L"clipboard_history_cancel") {
            return {std::make_unique<BuiltinAction>([] {
                        std::wstring report;
                        cancelClipboardHistoryPicker(&report);
                        wprintf(L"  -> %s\n", report.c_str());
                    }),
                    L""};
        }
        if (spec.name == L"inspect_clipboard_history") {
            return {std::make_unique<BuiltinAction>([] {
                        std::wstring report = describeClipboardHistory();
                        wprintf(L"\n%s\n", report.c_str());
                        MessageBoxW(nullptr, report.c_str(), L"Clipboard History", MB_OK | MB_SETFOREGROUND);
                    }),
                    L""};
        }
        if (spec.name == L"close_focused_app") {
            return {std::make_unique<BuiltinAction>([strong = spec.strongClose] {
                        std::wstring report;
                        if (closeFocusedApp(strong, &report)) {
                            wprintf(L"  -> %s\n", report.c_str());
                        } else {
                            wprintf(L"  ! %s\n", report.c_str());
                        }
                    }),
                    L""};
        }
        if (spec.name == L"kill_focused_app") {
            return {std::make_unique<BuiltinAction>([] {
                        std::wstring report;
                        if (killFocusedApp(&report)) {
                            wprintf(L"  -> %s\n", report.c_str());
                        } else {
                            wprintf(L"  ! %s\n", report.c_str());
                        }
                    }),
                    L""};
        }
        return {nullptr, L"unknown builtin action"};
    }

    if (spec.type == L"open_app") {
        if (spec.path.empty()) return {nullptr, L"open_app action is missing path"};
        return {std::make_unique<ExecutableScript>(spec.path, spec.args, spec.showWindow), L""};
    }

    if (spec.type == L"run_script") {
        if (spec.path.empty()) return {nullptr, L"run_script action is missing path"};
        return {makeScriptAction(spec), L""};
    }

    if (spec.type == L"run_command") {
        if (spec.command.empty()) return {nullptr, L"run_command action is missing command"};
        return {std::make_unique<ShellCommand>(spec.command, spec.showWindow), L""};
    }

    return {nullptr, L"unknown action type"};
}
