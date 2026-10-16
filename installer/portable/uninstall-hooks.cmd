@echo off
setlocal
rem Remove the pet's agent hooks (Codex / WorkBuddy / Claude). Only entries that
rem reference DesktopCompanionHook are removed; other hooks are left alone.
set "HERE=%~dp0"
powershell -NoProfile -ExecutionPolicy Bypass -File "%HERE%tools\install-hooks.ps1" -HelperPath "%HERE%DesktopCompanionHook.exe" -Remove
echo.
pause
