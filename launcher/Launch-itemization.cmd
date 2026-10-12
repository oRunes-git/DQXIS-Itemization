@echo off
powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0Launch-itemization.ps1"
if errorlevel 1 (
    echo.
    echo Itemization is not ready. Leave XI S at the title screen.
    pause
    exit /b 1
)
