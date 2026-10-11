@echo off
rem Reusable public-API arm author for the whale-girl model (identity probe mode).
rem Decodes an ISOLATED .cmo3 copy and re-exports .moc3 candidates. Never touches
rem the production archive, the default store, or any texture.
rem
rem Usage:  tools\psd2live-arm-author.cmd <input.cmo3> <output-dir> [store-dir]
setlocal
set "ROOT=%~dp0.."
set "INSTALL=D:\tool\psd2live"
set "JDK=C:\Progra~1\Microsoft\jdk-21.0.12.101-hotspot"
set "JAVAC=%JAVAC21%"
if "%JAVAC%"=="" set "JAVAC=%JDK%\bin\javac.exe"
set "JAVAE=%JAVA21%"
if "%JAVAE%"=="" set "JAVAE=%JDK%\bin\java.exe"
set "WORK=%ROOT%\build\psd2live-arm-author"
set "CLASSES=%WORK%\classes"
set "IN=%~1"
set "OUT=%~2"
set "STORE=%~3"
if "%STORE%"=="" set "STORE=%WORK%\store"
if "%IN%"=="" exit /b 2
if "%OUT%"=="" exit /b 2
for %%I in ("%IN%") do set "IN=%%~fI"
for %%I in ("%OUT%") do set "OUT=%%~fI"
for %%I in ("%STORE%") do set "STORE=%%~fI"
mkdir "%WORK%\tmp" 2>nul
mkdir "%CLASSES%" 2>nul
mkdir "%STORE%" 2>nul
mkdir "%OUT%" 2>nul
echo [javac] %JAVAC%
"%JAVAC%" -encoding UTF-8 -cp "%INSTALL%/app/*" -d "%CLASSES%" "%ROOT%\tools\PSD2LiveArmAuthor.java" || exit /b 1
echo [java]  %JAVAE%
"%JAVAE%" -Dpsd2live.agent.store="%STORE%" -Djava.io.tmpdir="%WORK%\tmp" -cp "%CLASSES%;%INSTALL%/app/*" PSD2LiveArmAuthor --install "%INSTALL%" --input "%IN%" --output "%OUT%" --store "%STORE%" --timeout-seconds 180 2>&1
echo [arm-author-exit] %ERRORLEVEL%
endlocal
