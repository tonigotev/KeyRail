@echo off
REM Double-clickable wrapper for build.ps1 so PowerShell's execution policy
REM does not get in the way. Any arguments are passed straight through, e.g.
REM   build.cmd -All
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0build.ps1" %*
set EXITCODE=%ERRORLEVEL%
if not "%1"=="-NoPause" pause
exit /b %EXITCODE%
