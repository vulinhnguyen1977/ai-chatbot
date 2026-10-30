@echo off
REM Downloads the optional "Smart" answer-style model (Qwen3 4B) into
REM runtime\models. It is not inside the release zip because GitHub caps each
REM release download at 2 GB, so it is published as two parts that this script
REM downloads, joins and checks against a known SHA-256.
REM
REM Run it again after an interrupted download: finished parts are kept and a
REM partial part resumes where it stopped.
REM
REM MSTUTOR_MODEL_BASE overrides the download location (a mirror, or a local
REM folder as file:///C:/path for testing).
setlocal
cd /d "%~dp0"

set "BASE=https://github.com/vulinhnguyen1977/ai-chatbot/releases/download/v0.1.0"
if defined MSTUTOR_MODEL_BASE set "BASE=%MSTUTOR_MODEL_BASE%"
set "NAME=qwen3-4b.gguf"
set "PART_BYTES=1248640240"
set "SHA256=3e4cb14174460404e7a233e531675303b2fbf7749c02f91864fe311ab6344e4f"
set "DIR=runtime\models"
set "OUT=%DIR%\%NAME%"

if not exist "%DIR%\" (
  echo Cannot find %DIR%. Run this from the folder that contains mstutor.exe.
  goto :fail
)
if exist "%OUT%" (
  echo The Smart model is already installed.
  goto :done
)
where curl >nul 2>&1
if errorlevel 1 (
  echo curl was not found. It is built into Windows 10 version 1803 and later.
  goto :fail
)

echo Downloading the Smart model: about 2.5 GB in two parts.
echo Needs about 5 GB of free disk space while it runs.
echo.

for %%P in (1 2) do (
  call :fetch %%P
  if errorlevel 1 goto :fail
)

echo.
echo Joining the parts...
copy /b "%OUT%.part1" + "%OUT%.part2" "%OUT%.tmp" >nul
if errorlevel 1 goto :fail

echo Checking the download...
set "GOT="
for /f "skip=1 delims=" %%H in ('certutil -hashfile "%OUT%.tmp" SHA256') do (
  if not defined GOT set "GOT=%%H"
)
set "GOT=%GOT: =%"
if /i not "%GOT%"=="%SHA256%" (
  echo The download is damaged: its checksum does not match.
  echo It has been deleted. Run this script again to start over.
  del "%OUT%.tmp" "%OUT%.part1" "%OUT%.part2" >nul 2>&1
  goto :fail
)

move /y "%OUT%.tmp" "%OUT%" >nul
del "%OUT%.part1" "%OUT%.part2" >nul 2>&1
echo.
echo Done. Restart mstutor.exe and pick Smart under Answer style.
goto :done

REM --- download one part, skipping it if already complete -------------------
:fetch
set "PART=%OUT%.part%1"
set "SIZE=0"
if exist "%PART%" for %%F in ("%PART%") do set "SIZE=%%~zF"
if "%SIZE%"=="%PART_BYTES%" (
  echo Part %1 of 2: already downloaded.
  exit /b 0
)
REM A part larger than expected is not resumable; start it again.
if %SIZE% GTR %PART_BYTES% del "%PART%"
echo Part %1 of 2:
curl -L --fail --retry 3 -C - -# -o "%PART%" "%BASE%/%NAME%.part%1"
if errorlevel 1 (
  echo Download failed. Check the internet connection and run this script again.
  exit /b 1
)
exit /b 0

:fail
echo.
echo The Smart model was not installed. The Fast model still works.
endlocal
pause
exit /b 1

:done
endlocal
pause
exit /b 0
