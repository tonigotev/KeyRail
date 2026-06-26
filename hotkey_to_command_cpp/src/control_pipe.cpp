#include "control_pipe.h"

#include <nlohmann/json.hpp>

#include <utility>

using nlohmann::json;

static constexpr wchar_t kPipeName[] = L"\\\\.\\pipe\\hotkeyd-control";

static std::string wideToUtf8(const std::wstring& value) {
    if (value.empty()) return {};
    int size = WideCharToMultiByte(CP_UTF8, 0, value.c_str(), -1, nullptr, 0, nullptr, nullptr);
    if (size <= 0) return {};
    std::string out(size, '\0');
    WideCharToMultiByte(CP_UTF8, 0, value.c_str(), -1, out.data(), size, nullptr, nullptr);
    out.pop_back();
    return out;
}

ControlPipe::ControlPipe(DWORD targetThreadId, UINT reloadMessage, UINT quitMessage, StatusProvider statusProvider)
    : targetThreadId_(targetThreadId),
      reloadMessage_(reloadMessage),
      quitMessage_(quitMessage),
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

void ControlPipe::run() {
    while (running_) {
        HANDLE pipe = CreateNamedPipeW(
            kPipeName,
            PIPE_ACCESS_DUPLEX,
            PIPE_TYPE_MESSAGE | PIPE_READMODE_MESSAGE | PIPE_WAIT,
            1,
            4096,
            4096,
            0,
            nullptr);

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
                try {
                    json request = json::parse(std::string(buffer, read));
                    std::string command = request.value("command", "");

                    if (command == "reload") {
                        PostThreadMessageW(targetThreadId_, reloadMessage_, 0, 0);
                        response = R"({"ok":true,"message":"reload requested"})";
                    } else if (command == "status") {
                        json out;
                        out["ok"] = true;
                        out["status"] = wideToUtf8(statusProvider_ ? statusProvider_() : L"");
                        response = out.dump();
                    } else if (command == "quit") {
                        PostThreadMessageW(targetThreadId_, quitMessage_, 0, 0);
                        response = R"({"ok":true,"message":"quit requested"})";
                    }
                } catch (...) {
                    response = R"({"ok":false,"message":"could not parse request"})";
                }
            }

            DWORD written = 0;
            WriteFile(pipe, response.data(), static_cast<DWORD>(response.size()), &written, nullptr);
            FlushFileBuffers(pipe);
            DisconnectNamedPipe(pipe);
        }

        CloseHandle(pipe);
    }
}
