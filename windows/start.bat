@echo off
REM ════════════════════════════════════════════════════════════════════
REM   ChatBantu - Windows launcher (Spring Initializer-style: just run)
REM
REM   Double-click this file (or run `start.bat` in PowerShell/CMD)
REM   and the server will boot on http://localhost:8080, then your
REM   default browser will open automatically.
REM
REM   No installs, no internet, no WSL, no Docker.
REM ════════════════════════════════════════════════════════════════════

setlocal ENABLEDELAYEDEXPANSION

REM ── Resolve script dir (so it works no matter where you launch from) ──
cd /d "%~dp0"

echo.
echo  ╔══════════════════════════════════════════════════════════════╗
echo  ║  ChatBantu - Social Network (Pure Bantu + Sua)              ║
echo  ╚══════════════════════════════════════════════════════════════╝
echo.

REM ── Port: prefer $PORT if set (Render-style), else 8080 ──
if "%PORT%"=="" set PORT=8080
echo  Port:     %PORT%
echo  Database: %CD%\chatbantu.db
echo  URL:      http://localhost:%PORT%
echo.

REM ── Verify bantu.exe is here ──
if not exist "bantu.exe" (
    echo  [ERROR] bantu.exe not found in this folder.
    echo          This shouldn't happen if you got the zip from a release.
    echo          Re-download from: https://github.com/AsseySilivestir/bantusua-local/releases
    pause
    exit /b 1
)

REM ── Verify required DLLs are present ──
set MISSING=0
for %%D in (sqlite3.dll libcurl-x64.dll libc++.dll libunwind.dll libwinpthread-1.dll) do (
    if not exist "%%D" (
        echo  [ERROR] Missing: %%D
        set MISSING=1
    )
)
if "!MISSING!"=="1" (
    echo.
    echo  Some runtime DLLs are missing. Re-download the zip from:
    echo    https://github.com/AsseySilivestir/bantusua-local/releases
    pause
    exit /b 1
)

REM ── Start the server in background ──
echo  Starting server...
start "ChatBantu Server" /min cmd /c "bantu.exe run server.b > server.log 2>&1"

REM ── Wait for the server to come up ──
echo  Waiting for server to come up...
set TRIES=0
:waitloop
timeout /t 1 /nobreak >nul
set /a TRIES+=1
powershell -Command "try { (Invoke-WebRequest -Uri 'http://localhost:%PORT%/api/health' -UseBasicParsing -TimeoutSec 1).StatusCode } catch { exit 1 }" >nul 2>&1
if errorlevel 1 (
    if !TRIES! lss 10 (
        goto waitloop
    ) else (
        echo  [ERROR] Server did not come up within 10 seconds.
        echo          Check server.log for details.
        pause
        exit /b 1
    )
)

REM ── Open browser ──
echo  Opening browser...
start "" "http://localhost:%PORT%"

echo.
echo  ══════════════════════════════════════════════════════════════
echo   Server is running. Login with:
echo     username: silivestir
echo     password: bantu123
echo.
echo   To stop:  close the "ChatBantu Server" window,
echo             or run:  stop.bat
echo  ══════════════════════════════════════════════════════════════
echo.
echo  This window can be closed. The server keeps running.
pause
endlocal
