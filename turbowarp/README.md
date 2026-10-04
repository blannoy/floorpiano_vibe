# Floor Piano — TurboWarp extension

Brings the floor-piano pad presses into **TurboWarp** (the Scratch desktop/web clone) as
blocks, so kids can build their own Scratch projects driven by the floor keys.

It connects directly to the NodeMCU/ESP32 over the same WebSocket API the web app uses —
no Python or extra software in between.

## Load it

1. Open **TurboWarp** (desktop app or <https://turbowarp.org>).
2. Click the **Extensions** button (bottom-left).
3. Choose **Custom Extension**.
4. On the **File** tab, select `floorpiano.extension.js` (this file).
   *(Or use the URL tab if you serve it — e.g. drop it in `soundplayer/` and run `serve.py`,
   then load `http://localhost:8000/floorpiano.extension.js`.)*
5. **Tick “Run extension without sandbox”**, then **Load**.

A new **Floor Piano** category appears in the blocks palette.

> The laptop running TurboWarp must be on the **same network** as the device
> (its “FloorPiano” WiFi, or your router if it joined in STA mode).

## Blocks

| Block | What it does |
|---|---|
| `connect to floor piano at (192.168.4.1)` | Opens the connection (and turns on the device's key polling). |
| `disconnect from floor piano` | Closes it. |
| `connected?` | Boolean — is the link up. |
| `when key (0) pressed` | Hat — runs when that pad is stepped on. Each key tracks its own press. |
| `when any key pressed` | Hat — runs when a key goes down (no key → some key). |
| `key (0) pressed?` | Boolean — is that pad currently held. |
| `last pressed key` | The most recently pressed key number (0–23). |
| `number of keys pressed` | How many pads are held right now. |
| `MIDI note of key (0)` | The device's configured MIDI note for that key (default 60 + index). Handy with Scratch's Music extension. |

## Example projects

**Play a sound per key**
```
when green flag clicked
  connect to floor piano at (192.168.4.1)

when key (0) pressed
  play sound (meow)
```

**Drive the Music extension with the real notes**
```
when key (5) pressed
  play note (MIDI note of key (5)) for (0.5) beats
```

**Move a sprite**
```
when key (0) pressed
  change x by (-10)
when key (1) pressed
  change x by (10)
```

## Notes

- Put `connect to floor piano at (…)` under `when green flag clicked` so it links on start.
- It auto-reconnects if the WiFi drops.
- This is one more WebSocket client; the device allows several at once
  (8 on ESP32, 4 on ESP8266), so the web app / SoundFont player / this extension can coexist.
- Works fully offline — nothing is fetched from the internet.
