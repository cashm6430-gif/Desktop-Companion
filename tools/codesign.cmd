@echo off
REM Re-sign DesktopCompanion binaries with the local self-signed code-signing
REM cert ("DesktopCompanion Dev", thumbprint 1425E9059052C49BD37AFB28B8C6162017C32643,
REM trusted in LocalMachine Root + TrustedPeople). Run after every rebuild:
REM   tools\codesign.cmd build\DesktopCompanion.exe build\DesktopCompanionHook.exe
setlocal
set "SIGNTOOL=C:\Program Files (x86)\Windows Kits\10\bin\10.0.26100.0\x64\signtool.exe"
if not exist "%SIGNTOOL%" (
    for /f "delims=" %%i in ('dir /b /ad "C:\Program Files (x86)\Windows Kits\10\bin"') do (
        if exist "C:\Program Files (x86)\Windows Kits\10\bin\%%i\x64\signtool.exe" set "SIGNTOOL=C:\Program Files (x86)\Windows Kits\10\bin\%%i\x64\signtool.exe"
    )
)
"%SIGNTOOL%" sign /fd SHA256 /n "DesktopCompanion Dev" /tr http://timestamp.digicert.com /td SHA256 %*
endlocal
