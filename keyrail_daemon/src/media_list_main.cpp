#include "media_sessions.h"
#include "browser_bridge.h"

#include <windows.h>

#include <cstdio>
#include <string>

static const wchar_t* yesNo(bool value) {
    return value ? L"yes" : L"no";
}

static std::string wideToUtf8(const std::wstring& value) {
    if (value.empty()) return {};
    int size = WideCharToMultiByte(CP_UTF8, 0, value.c_str(), -1, nullptr, 0, nullptr, nullptr);
    if (size <= 0) return {};
    std::string out(size, '\0');
    WideCharToMultiByte(CP_UTF8, 0, value.c_str(), -1, out.data(), size, nullptr, nullptr);
    out.pop_back();
    return out;
}

static void printLine(const std::wstring& value = L"") {
    std::string text = wideToUtf8(value + L"\n");
    fwrite(text.data(), 1, text.size(), stdout);
}

static void printField(const wchar_t* label, const std::wstring& value) {
    printLine(std::wstring(L"  ") + label + value);
}

int wmain() {
    SetConsoleOutputCP(CP_UTF8);

    startBrowserMediaBridge();
    printLine(L"Waiting 40 seconds for browser extension targets...");
    printLine(L"If this stays at zero, reload the extension or open its service worker console while this is waiting.");
    Sleep(40000);

    std::wstring report;
    std::vector<MediaTarget> targets = listMediaTargets(&report);

    printLine(report);
    printLine(L"visible media targets: " + std::to_wstring(targets.size()));
    printLine();

    if (targets.empty()) {
        printLine(L"No sessions were exposed by Windows GSMTC.");
        stopBrowserMediaBridge();
        return 0;
    }

    for (size_t i = 0; i < targets.size(); ++i) {
        const MediaTarget& target = targets[i];
        printLine(L"[" + std::to_wstring(i) + L"]");
        printField(L"kind:          ", target.kind.empty() ? L"(empty)" : target.kind);
        printField(L"id:            ", target.id.empty() ? L"(empty)" : target.id);
        printField(L"session index: ", std::to_wstring(target.sessionIndex));
        printField(L"app id:        ", target.appId.empty() ? L"(empty)" : target.appId);
        printField(L"title:         ", target.title.empty() ? L"(empty)" : target.title);
        printField(L"artist:        ", target.artist.empty() ? L"(empty)" : target.artist);
        printField(L"source host:   ", target.sourceHost.empty() ? L"(empty)" : target.sourceHost);
        printField(L"url:           ", target.url.empty() ? L"(empty)" : target.url);
        printField(L"tab title:     ", target.tabTitle.empty() ? L"(empty)" : target.tabTitle);
        printField(L"document:      ", target.documentTitle.empty() ? L"(empty)" : target.documentTitle);
        printField(L"media session: ", target.mediaSessionPlaybackState.empty() ? L"(empty)" : target.mediaSessionPlaybackState);
        printField(L"status:        ", target.playbackStatus.empty() ? L"(unknown)" : target.playbackStatus);
        printField(L"playing:       ", yesNo(target.playing));
        printField(L"play/pause:    ", yesNo(target.canTogglePlayPause));
        printField(L"next:          ", yesNo(target.canNext));
        printField(L"previous:      ", yesNo(target.canPrevious));
        printLine();
    }

    stopBrowserMediaBridge();
    return 0;
}
