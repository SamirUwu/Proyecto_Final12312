@echo off
setlocal

set PROJECT_DIR=%~dp0
set NI_DEVICE=%1
set NI_CHANNEL=%2
set NI_MODE=%3
if "%NI_DEVICE%"=="" set NI_DEVICE=Dev2
if "%NI_CHANNEL%"=="" set NI_CHANNEL=ai0
if "%NI_MODE%"=="" set NI_MODE=rse

echo ============================================
echo   MultiFX Processor
echo   NI Device : %NI_DEVICE% / %NI_CHANNEL% (%NI_MODE%)
echo ============================================

:: Kill any leftover processes
taskkill /F /IM audio_engine.exe 2>NUL
taskkill /F /IM ni_feeder.exe 2>NUL
taskkill /F /IM pythonw.exe 2>NUL
taskkill /F /IM python.exe 2>NUL
timeout /t 1 >NUL

:: Start C NI feeder — sin ventana visible
echo [1/3] Starting C NI feeder...
cd /d "%PROJECT_DIR%audio_rpi"
start /B "C NI Feeder" ni_feeder.exe --device %NI_DEVICE% --mode %NI_MODE%

timeout /t 2 >NUL

:: Start audio engine — sin ventana visible
echo [2/3] Starting audio engine...
start /B "Audio Engine" audio_engine.exe

:: Wait for TCP socket to be ready
echo Waiting for audio engine TCP socket...
:wait_loop
timeout /t 1 >NUL
netstat -an | findstr "54321" | findstr "LISTENING" >NUL
if errorlevel 1 goto wait_loop
echo Socket ready.

:: Start GUI con pythonw — sin ventana de cmd, solo la GUI
echo [3/3] Starting GUI...
start /B "MultiFX GUI" "%PROJECT_DIR%Interfaz\venv\Scripts\pythonw.exe" "%PROJECT_DIR%Interfaz\main.py"
echo.
echo All running. Close this window to stop everything.
pause

taskkill /F /IM audio_engine.exe 2>NUL
taskkill /F /IM ni_feeder.exe 2>NUL
taskkill /F /IM pythonw.exe 2>NUL
taskkill /F /IM python.exe 2>NUL
timeout /t 1 >NUL