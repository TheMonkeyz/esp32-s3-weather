@echo off
cd /d "%~dp0"
start "ESP flash helper" /min powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0flash_helper.ps1"
