#pragma once

#include <cstdint>
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
    std::wstring artworkUrl;
    std::wstring url;
    std::wstring documentTitle;
    std::wstring tabTitle;
    std::wstring mediaSessionPlaybackState;
    std::wstring playbackStatus;
    bool playing = false;
    bool canTogglePlayPause = false;
    bool canNext = false;
    bool canPrevious = false;
    // Pre-decoded artwork pixels (BGRA, top-down). Empty when unavailable.
    std::vector<uint8_t> artworkBgra;
    int artworkWidth = 0;
    int artworkHeight = 0;
    std::vector<uint8_t> favIconBgra;
    int favIconWidth = 0;
    int favIconHeight = 0;
};

// Creates the overlay window and starts GDI+ up front. Without this the first
// overlay of the session pays that setup, which is a visible delay.
void warmMediaOverlay();

// Refreshes the media target list on a timer so the picker can open from a warm
// cache instead of querying at keypress time.
void startMediaTargetPolling();
void stopMediaTargetPolling();

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
