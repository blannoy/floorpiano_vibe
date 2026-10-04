@echo off
rem Launch the Floor Piano keyboard translator (GUI)
cd /d "%~dp0"

rem Pick the Python launcher
set PY=python
where python >nul 2>nul || set PY=py

rem Install dependencies the first time
%PY% -c "import websockets, pynput" >nul 2>nul
if errorlevel 1 (
  echo Installing dependencies ^(websockets, pynput^)...
  %PY% -m pip install --user websockets pynput
)

%PY% floor_piano.py
if errorlevel 1 pause
