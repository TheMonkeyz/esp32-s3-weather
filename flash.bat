@echo off
rem Usage: flash.bat [auto] [seconds]   ("auto" = no pause at the end, for Claude Code)
cd /d "%~dp0"
set SECS=%2
if "%SECS%"=="" set SECS=40
echo Flashing ESP32-S3 (port auto-detected)...
tools\esptool.exe --chip esp32s3 -b 460800 --before default_reset --after hard_reset write_flash --flash_mode dio --flash_freq 80m --flash_size 16MB 0x0 firmware\bootloader.bin 0x8000 firmware\partition-table.bin 0x10000 firmware\weather_amoled.bin > flash_log.txt 2>&1
set RC=%ERRORLEVEL%
type flash_log.txt
if not "%RC%"=="0" (
  echo.
  echo FLASH FAILED. If no port was found, hold BOOT, tap RESET, release BOOT, then run again.
  if not "%1"=="auto" pause
  exit /b %RC%
)
echo.
echo Flash OK - reading serial output for %SECS% seconds into serial_log.txt ...
powershell -NoProfile -ExecutionPolicy Bypass -File monitor.ps1 -Seconds %SECS%
if not "%1"=="auto" pause
