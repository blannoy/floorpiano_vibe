# Floor Piano

A giant **floor piano** for kids: 24 foot-sized capacitive pads laid out as piano keys.
Step on a key and it lights up, plays a sound, and can drive a real keyboard, a SoundFont
synth, or Scratch/TurboWarp projects.

The brain is a single **ESP8266 (NodeMCU)** or **ESP32-S3** reading two **MPR121** capacitive
touch chips (12 electrodes each = 24 keys) over I²C. It serves its own web app and broadcasts
key events over WebSocket, so everything else (browser, laptop apps) just connects to it —
**no internet required**.

```
  foot ─▶ pad ─▶ MPR121 ×2 (I²C) ─▶ ESP8266/ESP32 ─▶ WiFi AP + WebSocket
                                          │              │
                                          │              ├─ Web app (built-in) ─ tune / calibrate / play / diagnose
                                          │              ├─ Python keyboard translator (pynput)
                                          │              ├─ SoundFont player (FluidSynth WASM on laptop)
                                          │              └─ TurboWarp extension (Scratch blocks)
                                          │
                                          └─▶ Bluetooth LE (ESP32-S3 only)
                                                 ├─ BT keyboard (HID) — pairs as a normal keyboard
                                                 └─ BT MIDI — pairs as a MIDI instrument
```

---

## Contents

- [Hardware](#hardware)
- [Repository layout](#repository-layout)
- [Build & flash the device](#build--flash-the-device)
- [Quick start](#quick-start)
- [The web interface](#the-web-interface)
- [Calibration guide](#calibration-guide)
- [Bluetooth (BLE) output — ESP32-S3](#bluetooth-ble-output--esp32-s3)
- [Companion apps](#companion-apps)
- [Troubleshooting](#troubleshooting)
- [WebSocket API](#websocket-api-for-custom-clients)
- [Performance & latency notes](#performance--latency-notes)

---

## Hardware

| Part | Notes |
|---|---|
| **NodeMCU (ESP8266)** *or* **ESP32-S3 DevKitC-1** | The controller. ESP32-S3 is faster/lower-latency; the ESP8266 is cheaper. |
| **2 × MPR121** breakout boards | 12 capacitive electrodes each. Addresses **0x5A** (ADDR→GND) and **0x5B** (ADDR→VCC). |
| **24 conductive pads** | Foil/copper tape under floor tiles, wired one per electrode. |
| 3.3 V power | A decent USB supply or power bank. |

### Wiring (I²C, shared bus)

| Signal | ESP8266 (NodeMCU) | ESP32-S3 | MPR121 (both) |
|---|---|---|---|
| SDA | D2 / GPIO4 | GPIO5* | SDA |
| SCL | D1 / GPIO5 | GPIO6* | SCL |
| 3V3 | 3V3 | 3V3 | VIN/VCC |
| GND | GND | GND | GND |
| addr | — | — | #0 ADDR→GND, #1 ADDR→VCC |

\*ESP32-S3 pins are set near the top of `src/main.cpp` — change to match your board.

> **Grounding matters.** Capacitive sensing needs an earth reference. If touch is weak or
> erratic, see [Troubleshooting](#troubleshooting) — a grounded supply or an earth wire to the
> device GND often fixes it.

---

## Repository layout

| Path | What it is |
|---|---|
| `src/` | Device firmware (Arduino/C++): `main.cpp` (WiFi, HTTP, WebSocket, key polling, config) + `mpr121.{h,cpp}` (sensor driver) + `ble_out.{h,cpp}` (BLE keyboard/MIDI output, ESP32-S3 only). |
| `data/` | The web app (`index.html`) — a single self-contained file served from the device's flash. |
| `platformio.ini` | Build config — two environments: `nodemcuv2` (ESP8266) and `esp32-s3`. |
| `python/` | Keyboard translator GUI (`floor_piano.py`) + `start.bat`/`start.sh`. |
| `soundplayer/` | Laptop SoundFont player (FluidSynth WASM) + `serve.py`, `start.bat`, `fetch-libs.*`. See its own README. |
| `turbowarp/` | Scratch/TurboWarp extension (`floorpiano.extension.js`) + README. |

---

## Build & flash the device

Uses [PlatformIO](https://platformio.org/) (CLI or the VS Code extension).

Two **builds** must be uploaded: the **firmware** and the **filesystem image** (the web app in `data/`).

```bash
# ESP8266 (NodeMCU)
pio run -e nodemcuv2 -t upload       # firmware
pio run -e nodemcuv2 -t uploadfs     # web app (LittleFS)

# ESP32-S3
pio run -e esp32-s3 -t upload
pio run -e esp32-s3 -t uploadfs
```

After the first USB flash, you can update **over WiFi** from the web app
(**Settings → System → Firmware / Filesystem Update**) — no cable needed.
Firmware = `.pio/build/<env>/firmware.bin`, filesystem = `.pio/build/<env>/littlefs.bin`.

---

## Quick start

1. Power the device. It creates a WiFi access point:
   - **SSID:** `FloorPiano`  **Password:** `piano1234`
2. Connect your laptop/tablet to that network and open **`http://192.168.4.1`**.
3. On the **Piano** tab, click **▶ Start playing** (polling is off until you do — this also
   unlocks browser audio). Step on the pads: they light up and play.
4. New venue or weak response? Run the **Calibration guide** below.

The device can also **join your home WiFi** (Settings → System → WiFi) and then be reached at
its STA IP — but the `FloorPiano` AP is always available for a direct, standalone connection.

---

## The web interface

Three top tabs: **Piano**, **Settings**, **System**. *Settings* holds five sub-tabs.

### Piano
The playable view. A 2-octave keyboard lights up with the pads and plays built-in synth sounds
(Web Audio — no files needed). **▶ Start playing / ● Live** toggles the device's key polling
(needed for any sound/events). On ESP32-S3 builds, the row also carries a one-click
**Bluetooth output** toggle + status chip (full BLE controls live in *System → Bluetooth*).
Volume and octave-shift controls.

### Settings ▸ Debug
Real-time per-key charts of filtered value, baseline and thresholds. Toggles for the MPR121
poll and the debug data stream.

### Settings ▸ Monitor
One **bar per key** showing the touch amount **Δ = baseline − filtered**, with each key's
threshold line. Press one key and watch whether neighbours rise — this is your **cross-talk**
check. Bars turn orange on a registered hit.

### Settings ▸ Calibrate
Two tools (see [Calibration guide](#calibration-guide)):
- **Auto-Calibration** — uses the MPR121's hardware auto-config to set the right charge
  current/time (CDC/CDT) per key. Pick all keys or a subset; freeze the values or stay adaptive.
- **Threshold wizard** — step on each key when prompted (full-screen, floor-readable prompts;
  optional auto-detect). It measures each key's strength and its cross-talk, then sets per-key
  **touch** and **release** thresholds. Sliders for the trigger point and the release threshold
  (% of each key's peak). The device echoes back the stored values so you know it saved.

### Settings ▸ Advanced configuration
All MPR121 registers and per-key settings:
- Global touch/release thresholds, debounce, and the raw **AFE1/AFE2** registers — with friendly
  **charge current (CDC)** and **charge time (CDT)** helpers on top. AFE1/AFE2 also hold the filter
  sample counts (FFI/SFI) and sample interval (ESI): fewer samples = snappier but noisier.
- Auto-config target levels (USL/LSL/TL).
- Per-key table: touch/release threshold, **release delay** (key stays on N ms after lift-off),
  and MIDI note.

### Settings ▸ Help
- **Reset sensors & reboot** — one-click clean slate (clears per-key charge + thresholds,
  returns to adaptive auto-config, reboots). Use it when arriving somewhere new.
- **Live diagnostics** — per-key filtered/baseline/Δ, **free heap**, and **frames/s**. This tells
  sensor problems from link problems instantly (see Troubleshooting).
- **"Why doesn't it work?"** — a built-in troubleshooting guide.

### System
- **Device info** — AP/STA IP, **MPR121 #0/#1 connected status**, free heap.
- **WiFi** — optionally join a network; set the **AP channel**; **Scan channels** surveys nearby
  WiFi and recommends the least-congested channel (applies after reboot).
- **Bluetooth (BLE) output** *(ESP32-S3 only — section is hidden on ESP8266)*:
  **Start/Stop** the BLE radio, pick **Mode** (Keyboard / MIDI), and set the advertised
  **Device name**. Status chip shows **Off / ◌ Advertising / ● Connected** live.
  WiFi keeps running alongside, so this page stays reachable. See
  [Bluetooth (BLE) output](#bluetooth-ble-output--esp32-s3) for the full story.
- **Firmware / Filesystem update** (OTA) and **Reboot**.

---

## Calibration guide

Recommended order, especially in a new location:

1. **Settings → Help → Reset sensors & reboot** (clean slate).
2. **Calibrate → Auto-Calibration** — keep off the pads, Run. This sets per-key charge so every
   pad responds similarly. Out-of-range keys are flagged (lower the target or check wiring/pads).
3. **Calibrate → Threshold wizard** — select **All** keys, optionally tick **Auto-detect press**,
   Start, and step on each key when prompted. Review the suggested thresholds:
   - **Trigger point** — higher needs a firmer press; lower is more sensitive.
   - **Release threshold (% of peak)** — lower = more hysteresis (releases only when nearly off,
     e.g. drops below 10–20 instead of 0). Watch for the "release near cross-talk" warning.
   - **Apply** — the device confirms the stored values.
4. **Monitor** — press keys and confirm only the pressed key crosses its line.

If touch is weak: raise **charge current (CDC)** then **charge time (CDT)** in Configuration, or
improve grounding. If keys chatter: raise **debounce**.

---

## Bluetooth (BLE) output — ESP32-S3

> **ESP32-S3 only.** The ESP8266 has no Bluetooth radio; on that build the firmware compiles
> the BLE module out (no-op stubs) and the System tab hides the section.

The piano can also act as a **Bluetooth keyboard** or **Bluetooth MIDI instrument** — useful
when you don't want to set up WiFi every time, or when a wired connection to the host PC
would re-introduce a **ground loop** through mains earth and break the capacitive readings.
WiFi keeps running in parallel, so the configuration / calibration UI stays reachable.

| Mode | What pairs see | Per-key data sent | When to use |
|---|---|---|---|
| **Keyboard (HID)** | A regular BT keyboard | `cfg.keys[i].kbdKey` (default `a`–`x`) | Drop-in replacement for the Python `floor_piano.py` bridge — games, GarageBand keystrokes, Scratch typing, any app that reads the keyboard. |
| **MIDI** | A BT MIDI instrument | MIDI Note On/Off using `cfg.keys[i].note` | GarageBand, iPad music apps, DAWs, the SoundFont player (over BLE-MIDI). |

**Where it lives:** **Settings → System → Bluetooth (BLE) output**.

- **Start BLE** / **Stop BLE** — runtime toggle. **BLE is off at every boot** and you opt
  in for that session; the mode / name / per-key mapping are persisted but the on/off
  state is not. (Quiet boot, less battery drain, and no host flapping if you reset
  while a paired phone is still in range.) Starting BLE also turns on touch polling
  — otherwise no key events would be sent.
- **Output is exclusive.** While BLE is on, the WebSocket binary key-state stream
  is suppressed — the paired Bluetooth host is the only consumer of foot events.
  The browser piano visual freezes at the last frame, and companion apps (Python
  bridge, SoundFont player, TurboWarp extension) stop receiving events. Stop BLE
  to put the WebSocket stream back in charge. The configuration / Debug / Monitor /
  Calibrate streams keep flowing in either mode so the web UI stays usable for tuning.
- **Mode** — Keyboard or MIDI. If BLE is already running, changing the mode restarts the
  radio; you'll need to **unpair + re-pair on the host**.
- **Device name** — what shows up in the host's Bluetooth scan list.
  *Caveat:* MIDI mode currently advertises a fixed name (**FloorPiano**) because the
  underlying BLE-MIDI library bakes the name in at compile time. Keyboard mode honours
  the configured name.
- Status chip: **Off** (radio idle), **◌ Advertising** (waiting for a host to pair),
  **● Connected** (a peer is bonded and receiving events).

**Quick test:**
1. Flash the ESP32-S3 build, open the web UI, go to **Settings → System**.
2. In **Bluetooth (BLE) output**, set the mode and press **Start BLE**.
3. On the host (PC / iPad / phone), open the Bluetooth scanner and pair with the device.
4. **Keyboard mode**: open any text editor. **MIDI mode**: open a MIDI-aware app and select
   the floor piano as the input.
5. **Settings → Piano → Start playing** (or step on a pad) and watch the host receive the keys.

**Per-key mapping (Keyboard mode):** the defaults map keys 0..23 to characters `a..x`.
Custom mappings are stored in flash and can be set today via the WebSocket API
(`setConfig` with `keys[i].kbd`); a dedicated UI editor is on the to-do list. See the
[WebSocket API](#websocket-api-for-custom-clients).

**Grounding note:** plugging the device directly into a PC via USB ties the ESP32 ground
to the PC's chassis (mains earth). That changes the reference for the MPR121 and can
inject hum / break weak touches. WiFi and BLE both avoid this — there's no electrical path
between sensor and host. On battery power, BLE gives full galvanic isolation.

---

## Companion apps

All connect to the same device WebSocket; multiple can run at once
(up to 8 clients on ESP32, 4 on ESP8266).

### Python keyboard translator — `python/`
Turns pad presses into OS keystrokes (for games, GarageBand, etc.). GUI with device IP,
key-map, a **"repeat key while held"** option, **Connect** + **Start keyboard** (arm) buttons,
and a 24-pad indicator.
```bash
python/start.bat            # Windows (auto-installs deps)
./python/start.sh           # macOS/Linux
# or:  python floor_piano.py        (GUI)  /  --cli   (headless)
```
Needs `pip install websockets pynput` (tkinter ships with Python).

### SoundFont player — `soundplayer/`
A laptop web app that plays the pads through real **`.sf2` SoundFonts** via FluidSynth (WASM):
piano, drums, "funny" sounds, any General-MIDI instrument. SoundFont files are too big for the
device, so this runs on the laptop.
```bash
soundplayer/fetch-libs.bat        # one-time: downloads FluidSynth + a starter SoundFont
soundplayer/start.bat             # serves the app and opens the browser
```
See `soundplayer/README.md`.

### TurboWarp / Scratch extension — `turbowarp/`
Adds **Floor Piano** blocks to TurboWarp (`when key (N) pressed`, `key (N) pressed?`,
`MIDI note of key (N)`, …) so kids can build their own projects. Load
`floorpiano.extension.js` via TurboWarp's Custom Extension dialog ("Run without sandbox").
See `turbowarp/README.md`.

---

## Troubleshooting

Most of this is built into **Settings → Help**, including a one-click reset and live diagnostics.
The key idea: **use Live diagnostics to tell a *link* problem from a *sensor* problem.**

| Symptom | Likely cause | Fix |
|---|---|---|
| Connects, but keys dead / Debug graphs frozen, **frames/s = 0** | WiFi link (congested channel, distance) | System → WiFi → **Scan channels**, pick recommended, reboot; move laptop closer |
| Frames arriving but **Δ ≈ 0** on touch | Sensor: bad calibration, weak charge, grounding | **Help → Reset**, then recalibrate; raise CDC/CDT; improve earth ground; bare/thin-sock feet |
| MPR121 shows **✗ Not found** | I²C wiring / power | Reseat SDA/SCL/3V3/GND and the address pins |
| Several keys fire from one press | Cross-talk | Run the Threshold wizard; raise trigger point; more pad spacing / grounded strip |
| Laggy | WiFi / sensor filtering / audio buffer | Clear channel; lower the filter samples (AFE1/AFE2); SoundFont buffer 512 |
| No sound | Polling off / no SoundFont | Piano → **Start playing**; load a `.sf2` in the player |

> Capacitive tip: the same rig can behave differently in different buildings (floor type,
> grounding). The **Reset** + recalibrate flow plus a clear WiFi channel handles most venue changes.

---

## WebSocket API (for custom clients)

Connect to `ws://<device-ip>/ws`. Send `{"cmd":"startPoll"}` to make the device emit events.

**Key-state (device → client, binary, on change):** 4 bytes — `0x01` then a 24-bit little-endian
mask (`buf[1]` = keys 0–7, `buf[2]` = 8–15, `buf[3]` = 16–23). Bit set = pressed.

**Useful commands (client → device, JSON text):**

| Command | Purpose |
|---|---|
| `{"cmd":"getConfig"}` | Returns `config` (per-key notes/thresholds) + `info`. |
| `{"cmd":"startPoll"}` / `stopPoll` | Enable/disable key-event emission. |
| `{"cmd":"startDebug"}` / `stopDebug` | Stream per-key filtered/baseline/threshold (`debug` messages, ~20/s). |
| `{"cmd":"setConfig","data":{…}}` | Update WiFi / MPR121 / per-key settings. |
| `{"cmd":"autoCalibrate",…}` / `setAdaptive` | Charge calibration. |
| `{"cmd":"scanWifi"}` / `getScan` | WiFi channel survey. |
| `{"cmd":"startBle"}` / `stopBle` | Start / stop the BLE radio (ESP32-S3). Setting is persisted. |
| `{"cmd":"setConfig","data":{"ble":{"mode":0\|1,"name":"…"}}}` | BLE mode (0 = keyboard, 1 = MIDI) and advertised name. Live-restarts the radio if running. |
| `{"cmd":"setConfig","data":{"keys":[{…,"kbd":97},…]}}` | Per-key BLE-keyboard character (ASCII code; 97 = `a`). |
| `{"cmd":"resetRecover"}` | Clean reset + reboot. |
| `{"cmd":"reboot"}` | Restart. |

**Status updates from device:** when BLE is running, the device pushes
`{"type":"bleStatus","running":bool,"connected":bool,"mode":0|1}` on every peer
connect/disconnect. The initial `info` message also carries `hasBle`, `bleRunning`,
and `bleConnected` so a fresh client can render the state without waiting.

The TurboWarp extension and SoundFont player are small worked examples of a client.

---

## Performance & latency notes

Latency is treated as a first-class concern (kids notice lag between step and sound):

- **WiFi modem-sleep disabled** (biggest single win — avoids 100 ms+ stalls).
- **TCP `NODELAY`** (Nagle off) so 4-byte key packets ship immediately.
- **Fast polling** (1 ms on ESP32, 2 ms on ESP8266) and **coalesced, back-pressured** sends that
  never overflow the WebSocket queue — and never *wedge* (force-send after 250 ms).
- **ESP32-S3 build**: 240 MHz, `-O2`, TCP task tuned. Pick a **clear WiFi channel** for the AP.
- Sensor-side latency is dominated by the MPR121's filtering (FFI/SFI/ESI) — tunable in
  Configuration. Client audio uses a low-latency setting; the SoundFont player buffer is
  adjustable.

Everything runs **offline** — once flashed (and the SoundFont libs fetched once), no internet
is needed at play time.
