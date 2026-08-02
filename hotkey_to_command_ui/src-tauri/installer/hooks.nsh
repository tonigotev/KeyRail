; NSIS hooks for the Hotkey To Command installer.
;
; These macros are injected into Tauri's generated script at four fixed points.
;
; The installer deliberately does NOT set up the browser or Discord integrations.
; Both need choices and follow-up the installer cannot handle: the browser
; extension ends in a Load unpacked click that Windows gives no way to automate,
; and the Discord bridge needs a toolchain download and a multi-minute build that
; has no business blocking an install. Both live in Setup inside the app, which
; can show progress, verify the result and be re-run. The installer's job here is
; to install files and get out of the way.

!include LogicLib.nsh

!macro NSIS_HOOK_PREINSTALL
  ; The daemon locks its own exe, so an upgrade over a running copy fails. Ask
  ; it to quit through its control pipe first, which lets it restore the mic
  ; mute state and release its devices, and only force it if that is ignored.
  DetailPrint "Stopping the Hotkey To Command daemon..."
  nsExec::ExecToLog 'cmd /c echo {"command":"quit"} > \\.\pipe\hotkeyd-control'
  Pop $0
  Sleep 1200
  nsExec::ExecToLog 'taskkill /F /IM hotkeyd.exe'
  Pop $0
  Sleep 300
!macroend

!macro NSIS_HOOK_POSTINSTALL
  ; Marks the install as fresh so the first launch opens Setup instead of
  ; dropping the user on the bindings list with no integrations configured. The
  ; app clears this the moment it reads it, so it fires once per install.
  WriteRegDWORD HKCU "Software\HotkeyToCommand" "PendingSetup" 1
  WriteRegStr HKCU "Software\HotkeyToCommand" "InstalledVersion" "${VERSION}"
!macroend

!macro NSIS_HOOK_PREUNINSTALL
  DetailPrint "Stopping the Hotkey To Command daemon..."
  nsExec::ExecToLog 'cmd /c echo {"command":"quit"} > \\.\pipe\hotkeyd-control'
  Pop $0
  Sleep 1200
  nsExec::ExecToLog 'taskkill /F /IM hotkeyd.exe'
  Pop $0

  ; Autostart is registered by the app, not the installer, so it has to be
  ; cleaned up here or the daemon keeps being launched after removal.
  DeleteRegValue HKCU "Software\Microsoft\Windows\CurrentVersion\Run" "HotkeyToCommandDaemon"
  nsExec::ExecToLog 'schtasks /Delete /TN "HotkeyToCommandDaemonElevated" /F'
  Pop $0
!macroend

!macro NSIS_HOOK_POSTUNINSTALL
  ; Only our own keys go. Bindings and presets under %APPDATA% are the user's
  ; work and survive an uninstall. Vencord is left patched on purpose: the user
  ; may rely on other plugins, so unpatching Discord is their call.
  DeleteRegKey HKCU "Software\HotkeyToCommand"
!macroend
