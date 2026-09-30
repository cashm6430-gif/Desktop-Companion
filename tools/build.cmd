@echo off
setlocal
set "VS_VCVARS=C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvarsall.bat"
set "QT_PREFIX=E:\Qt\release\bin"
set "CONAN_HOME=%~dp0..\build\conan-home"
if not exist "%VS_VCVARS%" (echo MSVC environment not found: %VS_VCVARS% & exit /b 1)
if not exist "%QT_PREFIX%\lib\cmake\Qt6\Qt6Config.cmake" (echo Qt not found: %QT_PREFIX% & exit /b 1)
call "%VS_VCVARS%" x64 || exit /b 1
set "PATH=%QT_PREFIX%\bin;%PATH%"
if not exist "%CONAN_HOME%\profiles\default" conan profile detect --force || exit /b 1
conan install "%~dp0.." -of "%~dp0..\build\conan" -pr default --build=missing || exit /b 1
cmake -S "%~dp0.." -B "%~dp0..\build" -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH="%QT_PREFIX%;%~dp0..\build\conan" || exit /b 1
cmake --build "%~dp0..\build" -j 6 || exit /b 1
"%QT_PREFIX%\bin\windeployqt.exe" --release --no-translations "%~dp0..\build\DesktopCompanion.exe" || exit /b 1
copy /y "%QT_PREFIX%\bin\Qt6Test.dll" "%~dp0..\build\Qt6Test.dll" >nul || exit /b 1
ctest --test-dir "%~dp0..\build" --output-on-failure || exit /b 1
echo Build ready: %~dp0..\build\DesktopCompanion.exe
