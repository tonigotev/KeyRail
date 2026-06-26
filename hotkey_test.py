#!/usr/bin/env python3
"""
hotkey_test.py -- first-run smoke test for the RegisterHotKey path.

Asks you for a hotkey combo, registers it with the Windows kernel via
RegisterHotKey, then sleeps inside GetMessage (zero CPU) until the combo is
pressed ANYWHERE in the system -- at which point it flashes a DETECTED banner.

Pure ctypes. No pip installs. Windows only.

    python hotkey_test.py

Examples of combos it accepts:  ctrl+shift+h   |   alt+f9   |   ctrl+alt+space
"""

import ctypes
import sys
import time
from ctypes import wintypes

try:
    import winsound  # stdlib on Windows; audible cue on detection
    HAVE_BEEP = True
except ImportError:
    HAVE_BEEP = False

user32 = ctypes.WinDLL("user32", use_last_error=True)
kernel32 = ctypes.WinDLL("kernel32", use_last_error=True)

# --- Win32 signatures (set explicitly; handle truncation is a real 64-bit bug) ---
user32.RegisterHotKey.argtypes = (wintypes.HWND, ctypes.c_int, wintypes.UINT, wintypes.UINT)
user32.RegisterHotKey.restype = wintypes.BOOL
user32.UnregisterHotKey.argtypes = (wintypes.HWND, ctypes.c_int)
user32.UnregisterHotKey.restype = wintypes.BOOL
user32.GetMessageW.argtypes = (ctypes.POINTER(wintypes.MSG), wintypes.HWND,
                               wintypes.UINT, wintypes.UINT)
user32.GetMessageW.restype = ctypes.c_int  # returns -1 on error, 0 on WM_QUIT
kernel32.GetStdHandle.argtypes = (wintypes.DWORD,)
kernel32.GetStdHandle.restype = wintypes.HANDLE  # must NOT truncate on 64-bit
kernel32.GetConsoleMode.argtypes = (wintypes.HANDLE, ctypes.POINTER(wintypes.DWORD))
kernel32.SetConsoleMode.argtypes = (wintypes.HANDLE, wintypes.DWORD)

# --- constants ----------------------------------------------------------------
WM_HOTKEY = 0x0312
MOD = {"ctrl": 0x0002, "control": 0x0002, "alt": 0x0001,
       "shift": 0x0004, "win": 0x0008, "super": 0x0008}
MOD_NOREPEAT = 0x4000
ERROR_HOTKEY_ALREADY_REGISTERED = 1409

# named non-character keys -> virtual-key code
NAMED_VK = {
    "space": 0x20, "enter": 0x0D, "return": 0x0D, "tab": 0x09,
    "esc": 0x1B, "escape": 0x1B, "backspace": 0x08, "delete": 0x2E,
    "insert": 0x2D, "home": 0x24, "end": 0x23, "pageup": 0x21,
    "pagedown": 0x22, "up": 0x26, "down": 0x28, "left": 0x25, "right": 0x27,
}
for _n in range(1, 25):              # F1..F24 = 0x70..0x87
    NAMED_VK[f"f{_n}"] = 0x70 + (_n - 1)


def parse_combo(text):
    """'ctrl+shift+h' -> (modifier_flags, vk, pretty_name). Raises ValueError."""
    parts = [p for p in text.replace(" ", "").lower().split("+") if p]
    if not parts:
        raise ValueError("empty combo")

    mods, key_token = 0, None
    for p in parts:
        if p in MOD:
            mods |= MOD[p]
        elif key_token is None:
            key_token = p
        else:
            raise ValueError("only one non-modifier key is allowed (RegisterHotKey limit)")

    if key_token is None:
        raise ValueError("you must include one non-modifier key, e.g. ctrl+shift+h")

    if key_token in NAMED_VK:
        vk = NAMED_VK[key_token]
    elif len(key_token) == 1 and key_token.isalnum():
        vk = ord(key_token.upper())  # letters/digits: VK == uppercase ASCII
    else:
        raise ValueError(f"don't recognise key '{key_token}'")

    names = [m for m in ("ctrl", "alt", "shift", "win") if mods & MOD[m]]
    pretty = "+".join(names + [key_token]).title()
    return mods, vk, pretty


def enable_ansi():
    """Turn on VT processing so the color codes render in cmd.exe. Cosmetic."""
    try:
        ENABLE_VT, STD_OUTPUT = 0x0004, -11
        h = kernel32.GetStdHandle(ctypes.c_uint(STD_OUTPUT & 0xFFFFFFFF))
        mode = wintypes.DWORD()
        kernel32.GetConsoleMode(h, ctypes.byref(mode))
        kernel32.SetConsoleMode(h, mode.value | ENABLE_VT)
        return True
    except Exception:
        return False


def main():
    if sys.platform != "win32":
        print("This uses Windows-only APIs (user32 RegisterHotKey). Run it on Windows.")
        return 1

    ansi = enable_ansi()
    G = "\033[92m" if ansi else ""
    Y = "\033[93m" if ansi else ""
    R = "\033[0m" if ansi else ""

    print("RegisterHotKey smoke test")
    print("Examples:  ctrl+shift+h   |   alt+f9   |   ctrl+alt+space\n")

    try:
        mods, vk, pretty = parse_combo(input("Hotkey to listen for > ").strip())
    except ValueError as e:
        print(f"Bad combo: {e}")
        return 1

    HOTKEY_ID, QUIT_ID = 1, 2

    # hWnd = None -> hotkey is bound to THIS thread; WM_HOTKEY lands in our
    # thread queue and GetMessage(None, ...) below retrieves it. No window needed.
    if not user32.RegisterHotKey(None, HOTKEY_ID, mods | MOD_NOREPEAT, vk):
        err = ctypes.get_last_error()
        if err == ERROR_HOTKEY_ALREADY_REGISTERED:
            print(f"'{pretty}' is already owned by another app (error 1409). Try another.")
        else:
            print(f"RegisterHotKey failed (error {err}).")
        return 1

    # second hotkey: a clean way out of the blocking GetMessage loop
    user32.RegisterHotKey(None, QUIT_ID, MOD["ctrl"] | MOD["alt"] | MOD_NOREPEAT, ord("Q"))

    print(f"\nArmed. Listening for {Y}{pretty}{R} system-wide.")
    print("Press it anywhere -- even inside another app. Ctrl+Alt+Q to quit.\n")

    msg = wintypes.MSG()
    count = 0
    try:
        while True:
            ret = user32.GetMessageW(ctypes.byref(msg), None, 0, 0)  # sleeps here, 0% CPU
            if ret == 0:        # WM_QUIT
                break
            if ret == -1:       # GetMessage error
                print(f"GetMessage error {ctypes.get_last_error()}")
                break
            if msg.message == WM_HOTKEY:
                if msg.wParam == QUIT_ID:
                    break
                if msg.wParam == HOTKEY_ID:        # dispatch by id -> action
                    count += 1
                    ts = time.strftime("%H:%M:%S")
                    print(f"{G}#{count:<3} [{ts}]  DETECTED  {pretty}{R}")
                    if HAVE_BEEP:
                        winsound.Beep(880, 80)
    finally:
        user32.UnregisterHotKey(None, HOTKEY_ID)
        user32.UnregisterHotKey(None, QUIT_ID)

    print(f"\nDone. Detected {count} press(es).")
    return 0


if __name__ == "__main__":
    sys.exit(main())
