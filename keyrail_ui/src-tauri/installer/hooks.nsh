; NSIS hooks for the KeyRail installer.
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
  DetailPrint "Stopping the KeyRail daemon..."
  nsExec::ExecToLog 'cmd /c echo {"command":"quit"} > \\.\pipe\keyrail-control'
  Pop $0
  Sleep 1200
  nsExec::ExecToLog 'taskkill /F /IM keyraild.exe'
  Pop $0
  Sleep 300

  ; Clean up the app's previous identity. Before the rename it ran as
  ; hotkeyd.exe under its own autostart entry and its own single-instance
  ; mutex, so nothing stops the two builds running side by side: the old one
  ; keeps the browser bridge port and the new one silently reports no
  ; extension connected. The old uninstaller cannot do this because upgraders
  ; never run it.
  DetailPrint "Removing the previous version's autostart entry..."
  nsExec::ExecToLog 'taskkill /F /IM hotkeyd.exe'
  Pop $0
  DeleteRegValue HKCU "Software\Microsoft\Windows\CurrentVersion\Run" "HotkeyToCommandDaemon"
  nsExec::ExecToLog 'schtasks /Delete /TN "HotkeyToCommandDaemonElevated" /F'
  Pop $0
!macroend

!macro NSIS_HOOK_POSTINSTALL
  ; Marks the install as fresh so the first launch opens Setup instead of
  ; dropping the user on the bindings list with no integrations configured. The
  ; app clears this the moment it reads it, so it fires once per install.
  WriteRegDWORD HKCU "Software\KeyRail" "PendingSetup" 1
  WriteRegStr HKCU "Software\KeyRail" "InstalledVersion" "${VERSION}"
!macroend

!macro NSIS_HOOK_PREUNINSTALL
  DetailPrint "Stopping the KeyRail daemon..."
  nsExec::ExecToLog 'cmd /c echo {"command":"quit"} > \\.\pipe\keyrail-control'
  Pop $0
  Sleep 1200
  nsExec::ExecToLog 'taskkill /F /IM keyraild.exe'
  Pop $0

  ; Autostart is registered by the app, not the installer, so it has to be
  ; cleaned up here or the daemon keeps being launched after removal.
  DeleteRegValue HKCU "Software\Microsoft\Windows\CurrentVersion\Run" "KeyRailDaemon"
  nsExec::ExecToLog 'schtasks /Delete /TN "KeyRailDaemonElevated" /F'
  Pop $0
!macroend

!macro NSIS_HOOK_POSTUNINSTALL
  ; Only our own keys go. Bindings and presets under %APPDATA% are the user's
  ; work and survive an uninstall. Vencord is left patched on purpose: the user
  ; may rely on other plugins, so unpatching Discord is their call.
  DeleteRegKey HKCU "Software\KeyRail"
!macroend
