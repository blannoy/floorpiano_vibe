#!/usr/bin/env bash
# Launch the Floor Piano keyboard translator (GUI)
cd "$(dirname "$0")"

PY=python3
command -v python3 >/dev/null 2>&1 || PY=python

# Install dependencies the first time
"$PY" -c "import websockets, pynput" >/dev/null 2>&1 || \
  "$PY" -m pip install --user websockets pynput

exec "$PY" floor_piano.py
