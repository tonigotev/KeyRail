#pragma once

#include <string>
#include <vector>

enum class MediaCommand {
    TogglePlayPause,
    Next,
    Previous,
};

struct MediaTarget {
    std::wstring id;
    std::wstring kind = L"windows";
    unsigned int sessionIndex = 0;
    std::wstring appId;
    std::wstring title;
    std::wstring artist;
    std::wstring sourceHost;
    std::wstring favIconUrl;
    std::wstring url;
    std::wstring documentTitle;
    std::wstring tabTitle;
    std::wstring mediaSessionPlaybackState;
    std::wstring playbackStatus;
    bool playing = false;
    bool canTogglePlayPause = false;
    bool canNext = false;
    bool canPrevious = false;
};

std::vector<MediaTarget> listMediaTargets(std::wstring* report = nullptr);

bool openMediaPicker(std::wstring* report = nullptr);
bool moveMediaPicker(int direction, std::wstring* report = nullptr);
bool confirmMediaPicker(std::wstring* report = nullptr);
bool cancelMediaPicker(std::wstring* report = nullptr);
bool mediaPickerIsOpen();

bool controlSelectedMedia(MediaCommand command, std::wstring* report = nullptr);
bool mediaNextContextual(std::wstring* report = nullptr);
bool mediaPreviousContextual(std::wstring* report = nullptr);
bool mediaPlayPauseContextual(std::wstring* report = nullptr);

std::wstring describeMediaTargets();
