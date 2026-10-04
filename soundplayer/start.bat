@echo off
rem Launch the Floor Piano SoundFont player (serves this folder + opens the browser)
cd /d "%~dp0"
where python >nul 2>nul && (python serve.py) || (py serve.py)
if errorlevel 1 pause
