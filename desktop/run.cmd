@echo off
setlocal
set "APP=%~dp0dist\win-unpacked\StoryBox NFC Studio.exe"
if not exist "%APP%" (
  echo [ERROR] Packaged app not found. Run from the desktop folder:
  echo     npm install
  echo     npm run build
  echo     npm run dist:dir
  pause
  exit /b 1
)
start "" "%APP%" --no-sandbox
