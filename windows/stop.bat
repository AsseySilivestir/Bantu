@echo off
REM   ChatBantu - Stop the running server
cd /d "%~dp0"

echo  Stopping ChatBantu server...

REM  Kill any process whose image name is bantu.exe
taskkill /F /IM bantu.exe >nul 2>&1

if errorlevel 1 (
    echo  Server was not running.
) else (
    echo  Server stopped.
)

timeout /t 2 /nobreak >nul
