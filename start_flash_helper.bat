@echo off
cd /d "%~dp0"
rem Opens the helper in its own (normal-size) window so you can watch progress. Close it to stop.
start "ESP flash helper" powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0flash_helper.ps1"
