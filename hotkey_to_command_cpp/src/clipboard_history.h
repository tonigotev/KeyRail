#pragma once

#include <string>

void startClipboardHistory();
void stopClipboardHistory();

bool openClipboardHistoryPicker(std::wstring* report = nullptr);
bool moveClipboardHistoryPicker(int direction, std::wstring* report = nullptr);
bool confirmClipboardHistoryPicker(std::wstring* report = nullptr);
bool cancelClipboardHistoryPicker(std::wstring* report = nullptr);
bool clipboardHistoryPickerIsOpen();

// Mouse-driven variant: opens a clickable list; clicking an item copies it to the
// clipboard and immediately pastes it (Ctrl+V) into the previously focused window.
bool openClipboardQuickPicker(std::wstring* report = nullptr);

std::wstring describeClipboardHistory();
