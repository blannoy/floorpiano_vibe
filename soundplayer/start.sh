#!/usr/bin/env bash
# Launch the Floor Piano SoundFont player (serves this folder + opens the browser)
cd "$(dirname "$0")"
command -v python3 >/dev/null 2>&1 && exec python3 serve.py || exec python serve.py
