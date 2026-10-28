@echo off
rem Compatibility entry point: use the same dependency checks as PowerShell.
setlocal
if /i "%~1"=="build" (
    call pwsh -NoLogo -NoProfile -NonInteractive -File "%~dp0build-release.ps1" || exit /b 1
) else (
    call pwsh -NoLogo -NoProfile -NonInteractive -File "%~dp0build-release.ps1" -Test || exit /b 1
)
rem Preserve the local convenience signature when the machine has the dev cert.
if exist "%~dp0..\build\DesktopCompanion.exe" (
    call "%~dp0codesign.cmd" "%~dp0..\build\DesktopCompanion.exe" >nul 2>&1 || echo [dev] warning: DesktopCompanion.exe signing failed
)
if exist "%~dp0..\build\DesktopCompanionHook.exe" (
    call "%~dp0codesign.cmd" "%~dp0..\build\DesktopCompanionHook.exe" >nul 2>&1 || echo [dev] warning: DesktopCompanionHook.exe signing failed
)
