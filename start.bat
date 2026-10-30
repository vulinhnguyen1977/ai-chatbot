@echo off
REM Convenience launcher. Double-clicking mstutor.exe does exactly the same
REM thing -- this exists only so there is an obvious "start here" file.
REM
REM Nothing else needs to be running: the app starts its own bundled model
REM server and shuts it down again when the window closes.
cd /d "%~dp0"

if not exist "mstutor.exe" (
  echo mstutor.exe is missing.
  echo Build it first -- see README.md, "Building from source".
  pause
  exit /b 1
)

start "" "mstutor.exe"
