@echo off
cd /d "%~dp0"
where py >nul 2>nul
if errorlevel 1 (
  echo Python 3.10 or later is required. Install Python 3, then reopen this installer.
  pause
  exit /b 1
)
py -3 flash.py %*
set FLASH_EXIT=%ERRORLEVEL%
if not "%FLASH_EXIT%"=="0" pause
exit /b %FLASH_EXIT%
