#pragma once

#include <string>

void startClipboardHistory();
void stopClipboardHistory();

bool openClipboardHistoryPicker(std::wstring* report = nullptr);
bool moveClipboardHistoryPicker(int direction, std::wstring* report = nullptr);
bool confirmClipboardHistoryPicker(std::wstring* report = nullptr);
bool cancelClipboardHistoryPicker(std::wstring* report = nullptr);
bool clipboardHistoryPickerIsOpen();

std::wstring describeClipboardHistory();
