#include "control_pipe.h"

#include "browser_bridge.h"
#include "clipboard_history.h"
#include "discord_bridge.h"
#include "media_sessions.h"

#include <nlohmann/json.hpp>

#include <sddl.h>

#include <string>
#include <utility>

using nlohmann::json;

static constexpr wchar_t kPipeName[] = L"\\\\.\\pipe\\keyrail-control";
static constexpr wchar_t kPipeSecurity[] =
    L"D:P(A;;GA;;;SY)(A;;GA;;;BA)(A;;GA;;;IU)S:(ML;;NW;;;LW)";

static std::string wideToUtf8(const std::wstring& value) {
    if (value.empty()) return {};
    int size = WideCharToMultiByte(CP_UTF8, 0, value.c_str(), -1, nullptr, 0, nullptr, nullptr);
    if (size <= 0) return {};
    std::string out(size, '\0');
    WideCharToMultiByte(CP_UTF8, 0, value.c_str(), -1, out.data(), size, nullptr, nullptr);
    out.pop_back();
    return out;
}

class LocalSecurityDescriptor {
public:
    LocalSecurityDescriptor() {
        ConvertStringSecurityDescriptorToSecurityDescriptorW(
            kPipeSecurity,
            SDDL_REVISION_1,
            &descriptor_,
            nullptr);
    }

    ~LocalSecurityDescriptor() {
        if (descriptor_) LocalFree(descriptor_);
    }

    SECURITY_ATTRIBUTES* attributes() {
        if (!descriptor_) return nullptr;
        attributes_.nLength = sizeof(attributes_);
        attributes_.lpSecurityDescriptor = descriptor_;
        attributes_.bInheritHandle = FALSE;
        return &attributes_;
    }

private:
    PSECURITY_DESCRIPTOR descriptor_ = nullptr;
    SECURITY_ATTRIBUTES attributes_{};
};

ControlPipe::ControlPipe(DWORD targetThreadId, UINT reloadMessage, UINT quitMessage, UINT suspendMessage, UINT resumeMessage, StatusProvider statusProvider)
    : targetThreadId_(targetThreadId),
      reloadMessage_(reloadMessage),
      quitMessage_(quitMessage),
      suspendMessage_(suspendMessage),
      resumeMessage_(resumeMessage),
      statusProvider_(std::move(statusProvider)) {}

ControlPipe::~ControlPipe() {
    stop();
}

void ControlPipe::start() {
    if (running_.exchange(true)) return;
    worker_ = std::thread([this] { run(); });
}

void ControlPipe::stop() {
    if (!running_.exchange(false)) return;

    HANDLE pipe = CreateFileW(kPipeName, GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);
    if (pipe != INVALID_HANDLE_VALUE) {
        const char text[] = "{}";
        DWORD written = 0;
        WriteFile(pipe, text, sizeof(text) - 1, &written, nullptr);
        CloseHandle(pipe);
    }

    if (worker_.joinable()) worker_.join();
}

// Handle one JSON control request and produce the JSON response.
std::string ControlPipe::dispatchCommand(const std::string& body) {
    std::string response = R"({"ok":false,"message":"invalid request"})";
    try {
        json request = json::parse(body);
        std::string command = request.value("command", "");

        if (command == "reload") {
            PostThreadMessageW(targetThreadId_, reloadMessage_, 0, 0);
            response = R"({"ok":true,"message":"reload requested"})";
        } else if (command == "suspend_hotkeys") {
            PostThreadMessageW(targetThreadId_, suspendMessage_, 0, 0);
            response = R"({"ok":true,"message":"hotkeys suspended"})";
        } else if (command == "resume_hotkeys") {
            PostThreadMessageW(targetThreadId_, resumeMessage_, 0, 0);
            response = R"({"ok":true,"message":"hotkeys resumed"})";
        } else if (command == "status") {
            json out;
            out["ok"] = true;
            out["status"] = wideToUtf8(statusProvider_ ? statusProvider_() : L"");
            response = out.dump();
        } else if (command == "media_status") {
            json out;
            out["ok"] = true;
            out["status"] = wideToUtf8(describeMediaTargets());
            response = out.dump();
        } else if (command == "discord_status") {
            json out;
            out["ok"] = true;
            out["clients"] = static_cast<int>(discordBridgeClientCount());
            response = out.dump();
        } else if (command == "browser_status") {
            json out;
            out["ok"] = true;
            out["status"] = wideToUtf8(describeBrowserBridgeStatus());
            response = out.dump();
        } else if (command == "clipboard_status") {
            json out;
            out["ok"] = true;
            out["status"] = wideToUtf8(describeClipboardHistory());
            response = out.dump();
        } else if (command == "trigger") {
            // Invoke an overlay action by name. Useful for local automation
            // (AutoHotkey, scripts) that drives the daemon over the pipe.
            const std::string target = request.value("target", "");
            std::wstring report;
            bool ok = false;
            if (target == "media_picker_open") ok = openMediaPicker(&report);
            else if (target == "media_next") ok = mediaNextContextual(&report);
            else if (target == "media_previous") ok = mediaPreviousContextual(&report);
            else if (target == "media_play_pause") ok = mediaPlayPauseContextual(&report);
            else if (target == "media_picker_confirm") ok = confirmMediaPicker(&report);
            else if (target == "media_picker_cancel") ok = cancelMediaPicker(&report);
            else if (target == "clipboard_quick") ok = openClipboardQuickPicker(&report);
            else if (target == "clipboard_history") ok = openClipboardHistoryPicker(&report);
            else report = L"unknown trigger target";

            json out;
            out["ok"] = ok;
            out["message"] = wideToUtf8(report);
            response = out.dump();
        } else if (command == "quit") {
            PostThreadMessageW(targetThreadId_, quitMessage_, 0, 0);
            response = R"({"ok":true,"message":"quit requested"})";
        }
    } catch (...) {
        response = R"({"ok":false,"message":"could not parse request"})";
    }
    return response;
}

void ControlPipe::run() {
    while (running_) {
        LocalSecurityDescriptor security;
        HANDLE pipe = CreateNamedPipeW(
            kPipeName,
            PIPE_ACCESS_DUPLEX,
            PIPE_TYPE_MESSAGE | PIPE_READMODE_MESSAGE | PIPE_WAIT,
            1,
            4096,
            4096,
            0,
            security.attributes());

        if (pipe == INVALID_HANDLE_VALUE && security.attributes()) {
            pipe = CreateNamedPipeW(
                kPipeName,
                PIPE_ACCESS_DUPLEX,
                PIPE_TYPE_MESSAGE | PIPE_READMODE_MESSAGE | PIPE_WAIT,
                1,
                4096,
                4096,
                0,
                nullptr);
        }

        if (pipe == INVALID_HANDLE_VALUE) {
            Sleep(500);
            continue;
        }

        BOOL connected = ConnectNamedPipe(pipe, nullptr)
            ? TRUE
            : (GetLastError() == ERROR_PIPE_CONNECTED);

        if (connected) {
            char buffer[4096]{};
            DWORD read = 0;
            std::string response = R"({"ok":false,"message":"invalid request"})";

            if (ReadFile(pipe, buffer, sizeof(buffer) - 1, &read, nullptr) && read > 0) {
                response = dispatchCommand(std::string(buffer, read));
            }

            DWORD written = 0;
            WriteFile(pipe, response.data(), static_cast<DWORD>(response.size()), &written, nullptr);
            FlushFileBuffers(pipe);
            DisconnectNamedPipe(pipe);
        }

        CloseHandle(pipe);
    }
}
