@echo off
REM ════════════════════════════════════════════════════════════════════
REM   ChatBantu - Reset the database (wipe & reseed)
REM
REM   Deletes chatbantu.db (and SQLite sidecar files), so the next
REM   start.bat run will recreate it with the demo user.
REM ════════════════════════════════════════════════════════════════════

cd /d "%~dp0"

echo  Stopping server (if running)...
taskkill /F /IM bantu.exe >nul 2>&1
timeout /t 1 /nobreak >nul

echo  Deleting database files...
if exist "chatbantu.db"      del /F /Q "chatbantu.db"      >nul 2>&1
if exist "chatbantu.db-wal"  del /F /Q "chatbantu.db-wal"  >nul 2>&1
if exist "chatbantu.db-shm"  del /F /Q "chatbantu.db-shm"  >nul 2>&1

echo  Done. Run start.bat to recreate the database with the demo account.
pause
