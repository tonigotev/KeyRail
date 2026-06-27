#include "action_factory.h"

#include "app_audio.h"
#include "audio.h"
#include "discord_bridge.h"

#include <windows.h>

#include <cstdio>

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
                        if (_wcsicmp(focusedAppExeName().c_str(), L"Discord.exe") == 0) {
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
        return {nullptr, L"unknown builtin action"};
    }

    if (spec.type == L"open_app") {
        if (spec.path.empty()) return {nullptr, L"open_app action is missing path"};
        return {std::make_unique<ExecutableScript>(spec.path, spec.args, spec.showWindow), L""};
    }

    if (spec.type == L"run_command") {
        if (spec.command.empty()) return {nullptr, L"run_command action is missing command"};
        return {std::make_unique<ShellCommand>(spec.command, spec.showWindow), L""};
    }

    return {nullptr, L"unknown action type"};
}
