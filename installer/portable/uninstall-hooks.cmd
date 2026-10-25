@echo off
setlocal
rem Remove the pet's agent hooks (Codex / WorkBuddy / Claude / OpenCode). Only entries that
rem reference DesktopCompanionHook are removed; other hooks are left alone.
set "HERE=%~dp0"
powershell -NoProfile -ExecutionPolicy Bypass -File "%HERE%tools\install-hooks.ps1" -HelperPath "%HERE%DesktopCompanionHook.exe" -Remove
set "HOOK_RESULT=%ERRORLEVEL%"
powershell -NoProfile -ExecutionPolicy Bypass -File "%HERE%tools\seed-cowork-hooks.ps1" -Remove
if not "%ERRORLEVEL%"=="0" set "HOOK_RESULT=%ERRORLEVEL%"
echo.
pause
exit /b %HOOK_RESULT%
