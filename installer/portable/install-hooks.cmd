@echo off
setlocal
rem Register the pet's agent hooks (Codex / WorkBuddy / Claude) so the desktop
rem pet reacts to your coding agents. Points at THIS folder's hook helper, so
rem run it once after unzipping anywhere (and again if you move the folder).
set "HERE=%~dp0"
powershell -NoProfile -ExecutionPolicy Bypass -File "%HERE%tools\install-hooks.ps1" -HelperPath "%HERE%DesktopCompanionHook.exe"
echo.
pause
