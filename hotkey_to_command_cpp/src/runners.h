#pragma once
// Runner hierarchy -- the C++ port of runners.py.
// Script::run() is shared (Template Method); subclasses override only
// buildCommand(), the one thing that differs between .py/.bat/.ps1/.exe.

#include <windows.h>
#include <string>
#include <vector>
#include <functional>

// ---- contract the dispatcher sees ------------------------------------------
struct Runnable {
    virtual ~Runnable() = default;
    virtual void run() = 0;
};

// ---- shared process-launch plumbing ----------------------------------------
class Script : public Runnable {
public:
    Script(std::wstring path, std::vector<std::wstring> args = {}, bool showWindow = false)
        : path_(std::move(path)), args_(std::move(args)), showWindow_(showWindow) {}

    void run() override {
        std::wstring cmd = buildCommand();          // <- the only per-type bit
        std::vector<wchar_t> buf(cmd.begin(), cmd.end());
        buf.push_back(L'\0');                        // CreateProcessW needs mutable

        STARTUPINFOW si{}; si.cb = sizeof(si);
        PROCESS_INFORMATION pi{};
        DWORD flags = showWindow_ ? 0 : CREATE_NO_WINDOW;

        if (CreateProcessW(nullptr, buf.data(), nullptr, nullptr, FALSE,
                           flags, nullptr, nullptr, &si, &pi)) {
            CloseHandle(pi.hThread);
            CloseHandle(pi.hProcess);               // fire and forget
        }
    }

protected:
    virtual std::wstring buildCommand() = 0;
    std::wstring quoted(const std::wstring& s) const {
        std::wstring out = L"\"";
        for (wchar_t c : s) {
            if (c == L'"') out += L"\\\"";
            else out += c;
        }
        out += L"\"";
        return out;
    }
    std::wstring joinedArgs() const {
        std::wstring r;
        for (const auto& a : args_) r += L" " + quoted(a);
        return r;
    }
    std::wstring path_;
    std::vector<std::wstring> args_;
    bool showWindow_;
};

class PythonScript : public Script {
public:
    using Script::Script;
    std::wstring buildCommand() override {
        return L"py " + quoted(path_) + joinedArgs();   // the 'py' launcher
    }
};

class BatchScript : public Script {
public:
    using Script::Script;
    std::wstring buildCommand() override {
        return L"cmd.exe /c " + quoted(path_) + joinedArgs();
    }
};

class PowerShellScript : public Script {
public:
    using Script::Script;
    std::wstring buildCommand() override {
        return L"powershell.exe -NoProfile -ExecutionPolicy Bypass -File "
               + quoted(path_) + joinedArgs();
    }
};

class ExecutableScript : public Script {
public:
    using Script::Script;
    std::wstring buildCommand() override {
        return quoted(path_) + joinedArgs();
    }
};

// ---- raw command line (the one injection surface) --------------------------
class ShellCommand : public Runnable {
public:
    explicit ShellCommand(std::wstring cmd, bool showWindow = false)
        : cmd_(std::move(cmd)), showWindow_(showWindow) {}

    void run() override {
        std::wstring full = L"cmd.exe /c " + cmd_;
        std::vector<wchar_t> buf(full.begin(), full.end());
        buf.push_back(L'\0');
        STARTUPINFOW si{}; si.cb = sizeof(si);
        PROCESS_INFORMATION pi{};
        DWORD flags = showWindow_ ? 0 : CREATE_NO_WINDOW;
        if (CreateProcessW(nullptr, buf.data(), nullptr, nullptr, FALSE,
                           flags, nullptr, nullptr, &si, &pi)) {
            CloseHandle(pi.hThread);
            CloseHandle(pi.hProcess);
        }
    }
private:
    std::wstring cmd_;
    bool showWindow_;
};

// ---- in-process action (audio cycle, wifi toggle, ...) ---------------------
class BuiltinAction : public Runnable {
public:
    explicit BuiltinAction(std::function<void()> fn) : fn_(std::move(fn)) {}
    void run() override { fn_(); }
private:
    std::function<void()> fn_;
};
