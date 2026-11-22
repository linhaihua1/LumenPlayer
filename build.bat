@echo off
rem ==========================================================================
rem  LumenPlayer dev build (Windows / MinGW-w64)
rem  Outputs: build\lumen-tests.exe / lumen-probe.exe / lumen-player.exe
rem
rem  The dev player is ~3MB and needs mpv.exe beside it (or on PATH).
rem  For the single-file release build, run:
rem      python tools\build_single.py
rem  NOTE: keep this file ASCII-only. cmd.exe parses .bat byte-by-byte using
rem  the active code page; Chinese bytes can be misread as \ or & and split a
rem  command line in half. Pure ASCII avoids that class of bug entirely.
rem ==========================================================================
setlocal enabledelayedexpansion

set ROOT=%~dp0
set SRC=%ROOT%src
set OUT=%ROOT%build
if not exist "%OUT%" mkdir "%OUT%"

rem ---- Locate g++: PATH first, then the portable toolchain next to the project ----
set GXX=
where g++ >nul 2>nul && set GXX=g++
if "%GXX%"=="" (
  if exist "%ROOT%..\..\tools\mingw64\bin\g++.exe" set GXX=%ROOT%..\..\tools\mingw64\bin\g++.exe
)
if "%GXX%"=="" (
  if exist "%USERPROFILE%\tools\mingw64\bin\g++.exe" set GXX=%USERPROFILE%\tools\mingw64\bin\g++.exe
)
if "%GXX%"=="" (
  echo [ERROR] g++ not found. Install MinGW-w64 or add it to PATH.
  exit /b 1
)

echo Compiler: %GXX%

set CXXFLAGS=-std=c++17 -O2 -Wall -Wextra -Wno-unused-parameter -I"%SRC%"
set LDFLAGS=-static -static-libgcc -static-libstdc++

set CORE=%SRC%\core\types.cpp %SRC%\core\json.cpp %SRC%\core\frame_desc.cpp %SRC%\core\frame_buffer.cpp %SRC%\core\field_analyzer.cpp %SRC%\core\color_policy.cpp %SRC%\core\grain_policy.cpp %SRC%\core\source_info.cpp %SRC%\core\processing_chain.cpp %SRC%\core\config.cpp
set PLATFORM=%SRC%\platform\process.cpp
set PLATFORM_ALL=%PLATFORM% %SRC%\platform\mpv_ipc.cpp %SRC%\platform\stream_pipe.cpp
set ENGINE=%SRC%\engine\sr_backend.cpp %SRC%\engine\dummy_backend.cpp
set SERVICE=%SRC%\service\inspector.cpp

echo.
echo [1/4] Building unit tests ...
"%GXX%" %CXXFLAGS% -I"%ROOT%third_party\onnxruntime" -o "%OUT%\lumen_tests.exe" tests\test_main.cpp %CORE% %SRC%\engine\onnx_backend.cpp %ENGINE% %LDFLAGS%
if errorlevel 1 goto fail

echo [2/4] Building lumen-probe ...
"%GXX%" %CXXFLAGS% -o "%OUT%\lumen-probe.exe" "%SRC%\apps\lumen-probe\main.cpp" %CORE% %PLATFORM% %SERVICE% %LDFLAGS%
if errorlevel 1 goto fail

echo [3/4] Building lumen-player ...
"%GXX%" %CXXFLAGS% -o "%OUT%\lumen-player.exe" "%SRC%\apps\lumen-player\main.cpp" "%SRC%\apps\lumen-player\player_ui.cpp" %CORE% %PLATFORM_ALL% %SERVICE% -lgdi32 -lcomdlg32 -lshell32 -luser32 %LDFLAGS%
if errorlevel 1 goto fail

echo [4/4] Running unit tests ...
"%OUT%\lumen_tests.exe"
if errorlevel 1 (
  echo.
  echo [WARNING] Some tests failed, see output above.
  exit /b 1
)

echo.
echo Build complete:
echo   %OUT%\lumen-player.exe   [player, needs mpv.exe beside it]
echo   %OUT%\lumen-probe.exe    [source inspection]
echo   %OUT%\lumen_tests.exe    [unit tests]
echo.
echo For the single-file release build, run:
echo   python tools\build_single.py
exit /b 0

:fail
echo.
echo [FAILED] Compilation failed, see errors above.
exit /b 1
