# Raw Input Keyboard Detector

Small standalone Win32 proof of concept that observes keyboard input using only
the Raw Input API (`WM_INPUT`). It creates a message-only window, registers for
keyboard raw input with `RIDEV_INPUTSINK`, prints raw input devices at startup,
and logs live keyboard make/break events.

This is for testing whether your own keyboard/device can still be observed while
another app, such as a game, is focused.

## Build With MSVC

Open **Developer PowerShell for VS 2022**, then run:

```powershell
cd "C:\Users\boris\Documents\hotkeys stuff\rawinput-test"
cl /EHsc /W4 /DUNICODE /D_UNICODE main.cpp /Fe:rawinput-test.exe user32.lib
```

If you are in normal PowerShell and `cl` says it is not recognized, run the
same build through Visual Studio's environment script:

```powershell
cd "C:\Users\boris\Documents\hotkeys stuff\rawinput-test"
cmd /c "`"C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat`" && cl /EHsc /W4 /DUNICODE /D_UNICODE main.cpp /Fe:rawinput-test.exe user32.lib"
```

## Build With MinGW

From a shell where `g++` is available:

```powershell
cd "C:\Users\boris\Documents\hotkeys stuff\rawinput-test"
g++ -std=c++17 -Wall -Wextra main.cpp -o rawinput-test.exe -luser32
```

## Run

Log every keyboard device:

```powershell
.\rawinput-test.exe
```

Log only devices whose raw input device name contains a substring:

```powershell
.\rawinput-test.exe "VID_1234"
```

or:

```powershell
.\rawinput-test.exe "HID#VID_046D"
```

The startup output lists all raw input devices with their handles and names.
Copy a unique substring from your board's path and pass it as the filter.

## Expected Output

Startup prints device entries like:

```text
[2] type=keyboard handle=0x0000000000010047
    \\?\HID#VID_1234&PID_5678&MI_00#...
```

Then key presses print live events:

```text
123456789ms device=0x10047 vk=0x41 scan=0x1e down
123456850ms device=0x10047 vk=0x41 scan=0x1e up
```

Leave this console running, focus the game or target app, and press keys on the
device. If events continue appearing, Windows is still delivering Raw Input to
this background process. Stop with `Ctrl+C`.

## Notes

- Uses Raw Input only.
- Does not use `SetWindowsHookEx`.
- Does not use `RegisterHotKey`.
- Does not use `SendInput`.
- Does not inject into or modify any other process.
