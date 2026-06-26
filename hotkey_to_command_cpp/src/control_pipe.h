#pragma once

#include <windows.h>
#include <atomic>
#include <functional>
#include <string>
#include <thread>

class ControlPipe {
public:
    using StatusProvider = std::function<std::wstring()>;

    ControlPipe(DWORD targetThreadId, UINT reloadMessage, UINT quitMessage, StatusProvider statusProvider);
    ~ControlPipe();

    void start();
    void stop();

private:
    void run();

    DWORD targetThreadId_;
    UINT reloadMessage_;
    UINT quitMessage_;
    StatusProvider statusProvider_;
    std::atomic<bool> running_{false};
    std::thread worker_;
};
