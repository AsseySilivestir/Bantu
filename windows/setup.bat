@echo off
REM ============================================================
REM   Bantu v1.2.1 — Windows one-time setup
REM   (run from this folder after unzipping)
REM ============================================================
setlocal EnableDelayedExpansion

echo.
echo   Bantu v1.2.1 — Windows setup
echo   ----------------------------
echo.

REM 1) Where are we?
set "SCRIPT_DIR=%~dp0"
if "%SCRIPT_DIR:~-1%"=="\" set "SCRIPT_DIR=%SCRIPT_DIR:~0,-1%"
echo   Script dir: %SCRIPT_DIR%

REM 2) Find a bantu.exe (next to this script, or already on PATH)
set "BANTU_EXE=%SCRIPT_DIR%\bantu.exe"
if not exist "%BANTU_EXE%" (
    REM Try bantu on PATH
    where bantu.exe >nul 2>nul
    if !errorlevel! == 0 (
        for /f "delims=" %%i in ('where bantu.exe') do (
            set "BANTU_EXE=%%i"
            goto :found
        )
    )
    echo   [ERROR] bantu.exe not found.
    echo   Place bantu.exe in %SCRIPT_DIR% then re-run setup.bat.
    echo   Build it from source:
    echo     cd bantu-src\compiler
    echo     cmake -B build -DCMAKE_BUILD_TYPE=Release
    echo     cmake --build build --config Release
    echo     copy build\Release\bantu.exe .
    echo.
    pause
    exit /b 1
)
:found
echo   Using: %BANTU_EXE%

REM 3) Add to user PATH (persistent, current user only)
echo.
echo   Adding %SCRIPT_DIR% to user PATH ...
REM    Use setx for persistence (max 1024 chars, hence we read existing PATH first)
for /f "usebackq tokens=2,*" %%a in (`reg query "HKCU\Environment" /v PATH 2^>nul`) do set "USER_PATH=%%b"
if defined USER_PATH (
    echo %USER_PATH% | findstr /i /c:"%SCRIPT_DIR%" >nul
    if !errorlevel! == 0 (
        echo   [OK] Already on user PATH.
    ) else (
        setx PATH "%USER_PATH%;%SCRIPT_DIR%" >nul
        echo   [OK] Added. Open a NEW terminal for the change to take effect.
    )
) else (
    setx PATH "%SCRIPT_DIR%" >nul
    echo   [OK] User PATH set. Open a NEW terminal.
)

REM 4) Seed registry if --seed was passed
if /i "%~1"=="--seed" (
    echo.
    echo   Seeding local package registry ...
    "%BANTU_EXE%" setup --seed
) else (
    echo.
    echo   To seed the local package registry with starter packages:
    echo     "%BANTU_EXE%" setup --seed
)

REM 5) Verify
echo.
echo   Verifying install ...
"%BANTU_EXE%" --version

echo.
echo   Done. Next steps (in a NEW terminal):
echo     bantu init myproject
echo     cd myproject
echo     bantu run
echo.
pause
endlocal
