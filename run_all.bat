@echo off
setlocal

set PROJECT_DIR=%~dp0
set NI_MODE=%1
if "%NI_MODE%"=="" set NI_MODE=rse

echo ============================================
echo   MultiFX Processor
echo   NI Mode: %NI_MODE%
echo   Device: auto-detected
echo ============================================

:: Kill any leftover processes
taskkill /F /IM audio_engine.exe 2>NUL
taskkill /F /IM ni_feeder.exe 2>NUL
taskkill /F /IM pythonw.exe 2>NUL
taskkill /F /IM python.exe 2>NUL
timeout /t 1 >NUL

:: Start C NI feeder — device auto-detected
echo [1/3] Starting C NI feeder...
cd /d "%PROJECT_DIR%audio_rpi"
start /B "C NI Feeder" ni_feeder.exe --mode %NI_MODE%

timeout /t 2 >NUL

:: Start audio engine
echo [2/3] Starting audio engine...
start /B "Audio Engine" audio_engine.exe

:: Wait for TCP socket to be ready
echo Waiting for audio engine TCP socket...
:wait_loop
timeout /t 1 >NUL
netstat -an | findstr "54321" | findstr "LISTENING" >NUL
if errorlevel 1 goto wait_loop
echo Socket ready.

:: Start GUI
echo [3/3] Starting GUI...
start "MultiFX GUI" "%PROJECT_DIR%Interfaz\venv\Scripts\pythonw.exe" "%PROJECT_DIR%Interfaz\main.py"

echo.
echo ============================================
echo Press Q to stop everything
echo ============================================

:wait_key
choice /c Q /n /m ""
if errorlevel 1 goto shutdown

:shutdown
echo.
echo Stopping processes...
taskkill /F /IM audio_engine.exe 2>NUL
taskkill /F /IM ni_feeder.exe 2>NUL
taskkill /F /IM pythonw.exe 2>NUL
taskkill /F /IM python.exe 2>NUL
timeout /t 1 >NUL
exit