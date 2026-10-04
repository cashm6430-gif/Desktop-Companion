@echo off
rem Local incremental build + test helper (build/ is gitignored).
rem The MSVC environment is assembled by hand instead of calling vcvarsall.bat,
rem because restricted shells can block the registry probe it relies on.
setlocal
set "MSVC_ROOT=C:\Program Files\Microsoft Visual Studio\18\Community\VC\Tools\MSVC\14.51.36231"
set "SDK_ROOT=C:\Program Files (x86)\Windows Kits\10"
set "SDK_VER=10.0.26100.0"
set "QT_PREFIX=E:\Qt\release\bin"
set "WindowsSdkVersion=%SDK_VER%\"
set "INCLUDE=%MSVC_ROOT%\include;%SDK_ROOT%\Include\%SDK_VER%\ucrt;%SDK_ROOT%\Include\%SDK_VER%\shared;%SDK_ROOT%\Include\%SDK_VER%\um;%SDK_ROOT%\Include\%SDK_VER%\winrt;%SDK_ROOT%\Include\%SDK_VER%\cppwinrt"
set "LIB=%MSVC_ROOT%\lib\x64;%SDK_ROOT%\Lib\%SDK_VER%\ucrt\x64;%SDK_ROOT%\Lib\%SDK_VER%\um\x64"
set "PATH=%MSVC_ROOT%\bin\Hostx64\x64;%QT_PREFIX%;%PATH%"
cmake --build "%~dp0..\build" -j 6 || exit /b 1
copy /y "%QT_PREFIX%\Qt6Test.dll" "%~dp0..\build\Qt6Test.dll" >nul 2>&1
if /i "%~1"=="build" goto :eof
ctest --test-dir "%~dp0..\build" --output-on-failure || exit /b 1
