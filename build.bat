@echo off
REM Builds mstutor.exe. Tries CMake first, then falls back to a direct
REM compiler invocation so the build works with either toolchain.
setlocal enabledelayedexpansion
cd /d "%~dp0"

where cmake >nul 2>&1
if %errorlevel%==0 (
  echo Building with CMake...
  cmake -S . -B build-cmake -DCMAKE_BUILD_TYPE=Release
  if errorlevel 1 goto :try_make
  cmake --build build-cmake --config Release
  if errorlevel 1 goto :try_make
  goto :done
)

:try_make
where mingw32-make >nul 2>&1
if %errorlevel%==0 (
  echo Building with mingw32-make...
  mingw32-make
  if errorlevel 1 goto :fail
  goto :done
)

where make >nul 2>&1
if %errorlevel%==0 (
  echo Building with make...
  make
  if errorlevel 1 goto :fail
  goto :done
)

:fail
echo.
echo No usable build tool was found.
echo.
echo Install ONE of these, then run build.bat again:
echo.
echo   * MSYS2 / MinGW-w64   https://www.msys2.org/
echo     then:  pacman -S mingw-w64-x86_64-gcc mingw-w64-x86_64-make
echo.
echo   * w64devkit (portable, no installer)
echo     https://github.com/skeeto/w64devkit/releases
echo.
echo   * Visual Studio Build Tools with the C++ workload, plus CMake
echo     https://visualstudio.microsoft.com/downloads/
echo.
exit /b 1

:done
echo.
echo Build complete.
mstutor.exe selftest
if errorlevel 1 (
  echo.
  echo Self-tests FAILED. Do not ship this build.
  exit /b 1
)

if not exist "runtime\bin\cpu\llama-server.exe" (
  echo.
  echo NOTE: the inference engine is missing from runtime\bin.
  echo Fetch it with:
  echo   powershell -ExecutionPolicy Bypass -File ship\fetch_runtime.ps1
)
if not exist "runtime\models\*.gguf" (
  echo.
  echo NOTE: no model weights in runtime\models.
  echo Put a .gguf there, or copy them out of a local Ollama install with:
  echo   powershell -ExecutionPolicy Bypass -File ship\extract_models.ps1
)

echo.
echo Done. Double-click mstutor.exe to run it.
endlocal
