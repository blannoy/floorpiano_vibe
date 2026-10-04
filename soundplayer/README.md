# Floor Piano — SoundFont Player (laptop app)

A standalone web app that gives the floor piano **rich, switchable sounds** (acoustic piano,
drums, "funny" sounds, any General-MIDI instrument, or any custom `.sf2`).

It runs on your **laptop**, not the NodeMCU — SoundFont files are far too big for the device.
It connects to the NodeMCU over the same WebSocket API the built-in web app uses, listens for
key presses, and plays them through **FluidSynth compiled to WebAssembly** (`js-synthesizer`).

```
 floor key ──> NodeMCU (touch + WebSocket) ──ws──> laptop browser ──> FluidSynth (.sf2) ──> speakers
```

## One-time setup (needs internet once)

Run the fetch script — it downloads the FluidSynth library **and** a starter SoundFont:

- Windows: `powershell -ExecutionPolicy Bypass -File fetch-libs.ps1`
- macOS/Linux: `bash fetch-libs.sh`

This grabs:
- `lib/libfluidsynth-2.4.6.js` + `lib/js-synthesizer.js` — the FluidSynth WASM engine.
  The WASM is embedded in the JS, so after this the app is **fully offline**.
- `soundfonts/GeneralUser-GS.sf2` (~31 MB) — **GeneralUser GS**, a free General-MIDI set that
  already covers **piano + 127 instruments + a drum kit**. It auto-loads in the app.

### More / custom SoundFonts (optional)
- Drop extra `.sf2` files into `soundfonts/` and add them to `soundfonts/manifest.json` to auto-load.
- Or use the **“Load .sf2 file…”** button in the app to pick any `.sf2` at runtime (even via `file://`).
- For "funny"/cartoon sounds, grab a themed `.sf2` or make one with [Polyphone](https://www.polyphone.io/).
- Bigger GM bank: **FluidR3_GM** (~140 MB).

## Run

Easiest: **double-click `start.bat`** (Windows) or run **`./start.sh`** (macOS/Linux).
It serves this folder and opens your browser automatically.

Equivalent manual command:

```bash
# from the soundplayer/ folder
python serve.py            # or:  python -m http.server 8000
```

Then browse to <http://localhost:8000>.
(You can also just double-click `index.html`, but then use the “Load .sf2 file…” button —
`file://` blocks auto-loading the manifest/WASM in some browsers.)

## Use

1. **Connect** — enter the device address (`192.168.4.1` on its own WiFi, or its STA IP) and click
   **Connect**. This also turns on the device's key polling, so it starts emitting key events.
2. **Choose the sound** — pick a loaded SoundFont, then either an **instrument** (General-MIDI
   program) or switch **Mode → Drum kit** (each key triggers a different percussion sound).
3. Step on the keys — the dots light up and play. Adjust **Volume**; lower the **Audio buffer**
   for less latency if your laptop keeps up.

## Notes

- Keep the laptop on the **same network** as the NodeMCU (its AP, or your router if it joined STA).
- Multiple sound clients can listen at once; this app does not interfere with the device's own
  built-in piano page. (If both make sound, mute the built-in one on the Piano tab.)
- Per-key → MIDI note mapping is read from the device config; drum mode uses its own fixed spread
  of 24 General-MIDI percussion sounds.
- Everything runs locally after setup — no internet needed at play time.
