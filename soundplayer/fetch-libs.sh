#!/usr/bin/env bash
# Downloads the FluidSynth WebAssembly library (js-synthesizer) into ./lib so the
# SoundFont player works fully offline afterwards. Run once, with internet access.
#
#   bash fetch-libs.sh
set -euo pipefail

VER=1.13.0                            # js-synthesizer version (bundles libfluidsynth 2.4.6)
BASE="https://unpkg.com/js-synthesizer@${VER}"
DIR="$(cd "$(dirname "$0")" && pwd)/lib"
mkdir -p "$DIR"

dl() { echo "Downloading $1"; curl -fsSL "$1" -o "$2"; }
dl "$BASE/externals/libfluidsynth-2.4.6.js" "$DIR/libfluidsynth-2.4.6.js"
dl "$BASE/dist/js-synthesizer.js"           "$DIR/js-synthesizer.js"

# Starter SoundFont: GeneralUser GS — piano + full General MIDI + drums (~31 MB).
SFDIR="$(cd "$(dirname "$0")" && pwd)/soundfonts"
mkdir -p "$SFDIR"
SF="$SFDIR/GeneralUser-GS.sf2"
if [ -f "$SF" ]; then
  echo "Starter SoundFont already present: $SF"
else
  echo "Downloading starter SoundFont (~31 MB)..."
  curl -fL "https://github.com/mrbumpy409/GeneralUser-GS/raw/main/GeneralUser-GS.sf2" -o "$SF"
fi

echo "Done."
echo "  Libraries: $DIR"
echo "  SoundFont: $SF"
echo "Run a local server in soundplayer/ (python -m http.server 8000) and open http://localhost:8000"
