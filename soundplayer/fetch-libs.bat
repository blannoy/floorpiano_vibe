@echo off
rem One-time: download FluidSynth (WASM) + a starter SoundFont into this folder.
cd /d "%~dp0"
powershell -ExecutionPolicy Bypass -File "%~dp0fetch-libs.ps1"
if errorlevel 1 pause
