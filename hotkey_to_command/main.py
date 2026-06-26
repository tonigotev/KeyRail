#!/usr/bin/env python3
"""
main.py -- ugly first merge: hotkey detection + script runners + a real
audio-output-cycle action, all in one file.

  python main.py

Default bindings (edit CONFIG below, or drop a config.json next to this file):
  Ctrl+Alt+O  -> cycle the default audio OUTPUT device   (the real one)
  Ctrl+Alt+T  -> launch notepad                          (demo command)
  Ctrl+Alt+Q  -> quit

Hotkey + runner layers are pure stdlib (ctypes). The audio action needs pycaw:
  pip install pycaw
Windows only.
"""

from __future__ import annotations

import ctypes
import json
import subprocess
import sys
import threading
import time
from abc import ABC, abstractmethod
from ctypes import wintypes
from pathlib import Path

# ============================================================================
# 1. HOTKEY LAYER  (RegisterHotKey + GetMessage -- the "interrupt" + WFI)
# ============================================================================
user32 = ctypes.WinDLL("user32", use_last_error=True)
kernel32 = ctypes.WinDLL("kernel32", use_last_error=True)

user32.RegisterHotKey.argtypes = (wintypes.HWND, ctypes.c_int, wintypes.UINT, wintypes.UINT)
user32.RegisterHotKey.restype = wintypes.BOOL
user32.UnregisterHotKey.argtypes = (wintypes.HWND, ctypes.c_int)
user32.UnregisterHotKey.restype = wintypes.BOOL
user32.GetMessageW.argtypes = (ctypes.POINTER(wintypes.MSG), wintypes.HWND,
                               wintypes.UINT, wintypes.UINT)
user32.GetMessageW.restype = ctypes.c_int

WM_HOTKEY = 0x0312
MOD_NOREPEAT = 0x4000
ERROR_HOTKEY_ALREADY_REGISTERED = 1409
MOD = {"ctrl": 0x0002, "control": 0x0002, "alt": 0x0001,
       "shift": 0x0004, "win": 0x0008, "super": 0x0008}

NAMED_VK = {"space": 0x20, "enter": 0x0D, "return": 0x0D, "tab": 0x09,
            "esc": 0x1B, "escape": 0x1B, "backspace": 0x08, "delete": 0x2E,
            "insert": 0x2D, "home": 0x24, "end": 0x23, "pageup": 0x21,
            "pagedown": 0x22, "up": 0x26, "down": 0x28, "left": 0x25, "right": 0x27}
for _n in range(1, 25):
    NAMED_VK[f"f{_n}"] = 0x70 + (_n - 1)


def parse_combo(text: str):
    """'ctrl+alt+o' -> (modifier_flags, vk, pretty_name)."""
    parts = [p for p in text.replace(" ", "").lower().split("+") if p]
    mods, key = 0, None
    for p in parts:
        if p in MOD:
            mods |= MOD[p]
        elif key is None:
            key = p
        else:
            raise ValueError("only one non-modifier key allowed (RegisterHotKey limit)")
    if key is None:
        raise ValueError("need one non-modifier key")
    if key in NAMED_VK:
        vk = NAMED_VK[key]
    elif len(key) == 1 and key.isalnum():
        vk = ord(key.upper())
    else:
        raise ValueError(f"unknown key '{key}'")
    names = [m for m in ("ctrl", "alt", "shift", "win") if mods & MOD[m]]
    return mods, vk, "+".join(names + [key]).title()


# ============================================================================
# 2. RUNNER HIERARCHY  (what a hotkey actually triggers)
# ============================================================================
CREATE_NO_WINDOW = 0x08000000


class Runnable(ABC):
    @abstractmethod
    def run(self, wait: bool = False, timeout: float | None = None):
        ...


class Script(Runnable):
    """Base for file-backed external processes. run() is shared (Template
    Method); subclasses implement only build_command()."""

    _by_ext: "dict[str, type[Script]]" = {}

    def __init__(self, path, args=None, cwd=None, env=None,
                 show_window=False, singleton=False):
        self.path = Path(path)
        if not self.path.exists():
            raise FileNotFoundError(self.path)
        self.args = list(args or [])
        self.cwd = cwd or str(self.path.parent)
        self.env = env
        self.show_window = show_window
        self.singleton = singleton
        self._proc = None

    @abstractmethod
    def build_command(self) -> "list[str]":
        ...

    def run(self, wait=False, timeout=None):
        if self.singleton and self._proc and self._proc.poll() is None:
            return self._proc
        argv = self.build_command()
        flags = 0 if self.show_window else CREATE_NO_WINDOW
        self._proc = subprocess.Popen(
            argv, cwd=self.cwd, env=self.env, creationflags=flags,
            stdout=subprocess.PIPE if wait else None,
            stderr=subprocess.PIPE if wait else None, text=True)
        if wait:
            out, err = self._proc.communicate(timeout=timeout)
            return subprocess.CompletedProcess(argv, self._proc.returncode, out, err)
        return self._proc

    @classmethod
    def from_path(cls, path, **kw):
        ext = Path(path).suffix.lower()
        try:
            return cls._by_ext[ext](path, **kw)
        except KeyError:
            raise ValueError(f"no Script type registered for '{ext}'")

    @classmethod
    def register_ext(cls, *exts):
        def deco(sub):
            for e in exts:
                cls._by_ext[e] = sub
            return sub
        return deco


@Script.register_ext(".py", ".pyw")
class PythonScript(Script):
    def __init__(self, path, interpreter=None, **kw):
        super().__init__(path, **kw)
        self.interpreter = interpreter or sys.executable or "python"

    def build_command(self):
        return [self.interpreter, str(self.path), *self.args]


@Script.register_ext(".bat", ".cmd")
class BatchScript(Script):
    def build_command(self):
        return ["cmd.exe", "/c", str(self.path), *self.args]


@Script.register_ext(".ps1")
class PowerShellScript(Script):
    def __init__(self, path, pwsh=False, bypass=True, **kw):
        super().__init__(path, **kw)
        self.exe = "pwsh.exe" if pwsh else "powershell.exe"
        self.bypass = bypass

    def build_command(self):
        cmd = [self.exe, "-NoProfile"]
        if self.bypass:
            cmd += ["-ExecutionPolicy", "Bypass"]
        return cmd + ["-File", str(self.path), *self.args]


@Script.register_ext(".exe")
class ExecutableScript(Script):
    def build_command(self):
        return [str(self.path), *self.args]


class ShellCommand(Runnable):
    """Raw command line (shell=True) -- the one injection surface; keep it
    out of any untrusted/imported config."""

    def __init__(self, command, cwd=None, show_window=False):
        self.command, self.cwd, self.show_window = command, cwd, show_window

    def run(self, wait=False, timeout=None):
        flags = 0 if self.show_window else CREATE_NO_WINDOW
        proc = subprocess.Popen(self.command, shell=True, cwd=self.cwd,
                                creationflags=flags,
                                stdout=subprocess.PIPE if wait else None,
                                stderr=subprocess.PIPE if wait else None, text=True)
        if wait:
            out, err = proc.communicate(timeout=timeout)
            return subprocess.CompletedProcess(self.command, proc.returncode, out, err)
        return proc


class BuiltinAction(Runnable):
    """In-process action (audio cycle, etc.), same Runnable contract."""

    def __init__(self, fn, *a, **kw):
        self.fn, self.a, self.kw = fn, a, kw

    def run(self, wait=False, timeout=None):
        return self.fn(*self.a, **self.kw)


# ============================================================================
# 3. THE REAL AUDIO ACTION  (cycle default output device via IPolicyConfig)
# ============================================================================
try:
    import comtypes
    from pycaw.utils import AudioUtilities
    from pycaw.constants import EDataFlow, DEVICE_STATE, ERole
    _PYCAW = True
except Exception:
    _PYCAW = False


def cycle_audio_device():
    """Switch the default render device to the next active output. Runs on a
    worker thread, so it initialises COM on that thread itself."""
    if not _PYCAW:
        print("  ! audio action needs pycaw:  pip install pycaw")
        return
    comtypes.CoInitialize()
    try:
        devices = AudioUtilities.GetAllDevices(
            EDataFlow.eRender.value, DEVICE_STATE.ACTIVE.value)
        devices = [d for d in devices if d.id]
        if not devices:
            print("  ! no active output devices found")
            return
        current = AudioUtilities.GetSpeakers().id     # GetSpeakers -> AudioDevice
        ids = [d.id for d in devices]
        idx = ids.index(current) if current in ids else -1
        nxt = devices[(idx + 1) % len(devices)]
        AudioUtilities.SetDefaultDevice(
            nxt.id, roles=[ERole.eConsole, ERole.eMultimedia, ERole.eCommunications])
        print(f"  -> audio output: {nxt.FriendlyName or nxt.id}")
    finally:
        comtypes.CoUninitialize()


BUILTINS = {"cycle_audio": BuiltinAction(cycle_audio_device)}


def make_runnable(spec: dict) -> Runnable:
    kind = spec.get("type", "script")
    if kind == "script":
        return Script.from_path(spec["path"], args=spec.get("args"),
                                show_window=spec.get("show_window", False),
                                singleton=spec.get("singleton", False))
    if kind == "command":
        return ShellCommand(spec["command"], show_window=spec.get("show_window", False))
    if kind == "builtin":
        return BUILTINS[spec["name"]]
    raise ValueError(f"unknown runnable type '{kind}'")


# ============================================================================
# 4. CONFIG  (bindings are data; the UI will edit this later)
# ============================================================================
CONFIG = [
    {"hotkey": "ctrl+alt+o", "action": {"type": "builtin", "name": "cycle_audio"}},
    {"hotkey": "ctrl+alt+t", "action": {"type": "command", "command": "notepad.exe"}},
]


def load_config():
    cfg = Path(__file__).with_name("config.json")
    if cfg.exists():
        return json.loads(cfg.read_text())["bindings"]
    return CONFIG


# ============================================================================
# 5. DISPATCH + MESSAGE LOOP
# ============================================================================
def make_handler(pretty, runnable):
    # the message thread is the "ISR": hand work to a worker, return fast.
    def handler():
        def work():
            try:
                runnable.run()
            except Exception as e:
                print(f"  ! '{pretty}' failed: {e}")
        threading.Thread(target=work, daemon=True).start()
    return handler


def main():
    if sys.platform != "win32":
        print("Windows only (user32 RegisterHotKey).")
        return 1

    dispatch = {}        # hotkey id -> handler
    next_id = 1

    # --- register every binding -------------------------------------------
    for b in load_config():
        try:
            mods, vk, pretty = parse_combo(b["hotkey"])
        except ValueError as e:
            print(f"  skip '{b['hotkey']}': {e}")
            continue
        if not user32.RegisterHotKey(None, next_id, mods | MOD_NOREPEAT, vk):
            err = ctypes.get_last_error()
            why = "already in use" if err == ERROR_HOTKEY_ALREADY_REGISTERED else f"err {err}"
            print(f"  skip {pretty}: {why}")
            continue
        dispatch[next_id] = make_handler(pretty, make_runnable(b["action"]))
        print(f"  armed  {pretty:<16} -> {b['action']}")
        next_id += 1

    # quit hotkey
    QUIT_ID = next_id
    user32.RegisterHotKey(None, QUIT_ID, MOD["ctrl"] | MOD["alt"] | MOD_NOREPEAT, ord("Q"))
    print(f"  armed  Ctrl+Alt+Q       -> quit\n  listening...\n")

    # --- the WFI loop ------------------------------------------------------
    msg = wintypes.MSG()
    try:
        while True:
            ret = user32.GetMessageW(ctypes.byref(msg), None, 0, 0)
            if ret in (0, -1):
                break
            if msg.message == WM_HOTKEY:
                if msg.wParam == QUIT_ID:
                    break
                h = dispatch.get(msg.wParam)
                if h:
                    print(f"[{time.strftime('%H:%M:%S')}] hotkey fired")
                    h()
    finally:
        for hid in list(dispatch) + [QUIT_ID]:
            user32.UnregisterHotKey(None, hid)
    print("bye")
    return 0


if __name__ == "__main__":
    sys.exit(main())
