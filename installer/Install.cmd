@echo off
setlocal
powershell.exe -NoProfile -STA -ExecutionPolicy Bypass -File "%~dp0Install.ps1"
if errorlevel 1 (
    echo.
    echo Installation did not finish. See the message above.
    pause
    exit /b 1
)
echo.
pause
