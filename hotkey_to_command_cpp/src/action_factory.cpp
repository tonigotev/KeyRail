#include "action_factory.h"

#include "audio.h"

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
