@echo off
rem Reusable CMO3 round-trip through the pinned PSD2Live 2.0.2 public API.
rem Imports an isolated .cmo3 copy and re-exports the model file family. It never
rem touches the source archive or the user's workspace (see
rem tools/PSD2LiveCmoImportProbe.java for the guarantees).
rem
rem Usage:  tools\psd2live-roundtrip.cmd <input.cmo3> <output-dir> [store-dir]
rem   <input.cmo3>  an ISOLATED COPY (never the production archive)
rem   <output-dir>  new or empty directory for the exported model family
rem   [store-dir]   new/empty isolated agent store (default under build\)
rem
rem Requires a JDK 21. Override with JAVA21 (java.exe) / JAVAC21 (javac.exe) env
rem vars; otherwise the Microsoft OpenJDK 21 install is used.
setlocal
set "ROOT=%~dp0.."
set "INSTALL=D:\tool\psd2live"
set "JDK=C:\Progra~1\Microsoft\jdk-21.0.12.101-hotspot"
set "JAVAC=%JAVAC21%"
if "%JAVAC%"=="" set "JAVAC=%JDK%\bin\javac.exe"
set "JAVAE=%JAVA21%"
if "%JAVAE%"=="" set "JAVAE=%JDK%\bin\java.exe"
set "WORK=%ROOT%\build\psd2live-roundtrip"
set "CLASSES=%WORK%\classes"
set "IN=%~1"
set "OUT=%~2"
set "STORE=%~3"
if "%STORE%"=="" set "STORE=%WORK%\store"
if "%IN%"=="" exit /b 2
if "%OUT%"=="" exit /b 2
rem The probe requires absolute, resolved paths; tmp/classes/store must exist.
for %%I in ("%IN%") do set "IN=%%~fI"
for %%I in ("%OUT%") do set "OUT=%%~fI"
for %%I in ("%STORE%") do set "STORE=%%~fI"
mkdir "%WORK%\tmp" 2>nul
mkdir "%CLASSES%" 2>nul
mkdir "%STORE%" 2>nul
mkdir "%OUT%" 2>nul
echo [javac] %JAVAC%
"%JAVAC%" -encoding UTF-8 -cp "%INSTALL%/app/*" -d "%CLASSES%" "%ROOT%\tools\PSD2LiveCmoImportProbe.java" || exit /b 1
echo [java]  %JAVAE%
"%JAVAE%" -Dpsd2live.agent.store="%STORE%" -Djava.io.tmpdir="%WORK%\tmp" -cp "%CLASSES%;%INSTALL%/app/*" PSD2LiveCmoImportProbe --install "%INSTALL%" --input "%IN%" --output "%OUT%" --store "%STORE%" --timeout-seconds 120 2>&1
echo [roundtrip-exit] %ERRORLEVEL%
endlocal
