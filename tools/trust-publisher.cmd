@echo off
REM One-time trust setup for the self-signed "DesktopCompanion Dev" code-signing
REM cert (thumbprint 1425E9059052C49BD37AFB28B8C6162017C32643). Root membership
REM alone is not enough: Explorer suppresses the "Do you want to run this file?"
REM prompt only when the signer is also in the TrustedPublisher store. Run this
REM once per machine (as Administrator), before or after tools\codesign.cmd.
setlocal
set "CER=%~dp0..\art\authoring\desktopcompanion-dev.cer"
if not exist "%CER%" (
    echo Missing %CER%
    echo Export it from the cert store first:
    echo   powershell -c "Get-ChildItem Cert:\LocalMachine\Root\1425E9059052C49BD37AFB28B8C6162017C32643 ^| Export-Certificate -FilePath '%CER%'"
    exit /b 1
)
powershell -NoProfile -Command "Import-Certificate -FilePath '%CER%' -CertStoreLocation Cert:\LocalMachine\TrustedPublisher; Import-Certificate -FilePath '%CER%' -CertStoreLocation Cert:\CurrentUser\TrustedPublisher"
endlocal
