#!/usr/bin/env python3
"""
Floor Piano – keyboard event translator
Connects to the NodeMCU via WebSocket and translates touch events into
keyboard key presses (e.g. for use with GarageBand, MIDI tools, games, etc.).

Install dependencies:
    pip install websockets pynput

Usage:
    python floor_piano.py                      # GUI (default)
    python floor_piano.py --cli                # headless / terminal mode
    python floor_piano.py --ip 192.168.4.1 --map piano   # prefill / CLI options
    python floor_piano.py --list               # print the active key map and exit
"""

import asyncio
import argparse
import json
import sys
import logging
import threading
import queue

try:
    import websockets
except ImportError:
    sys.exit("Missing: pip install websockets")

try:
    from pynput.keyboard import Controller, KeyCode, Key
except ImportError:
    sys.exit("Missing: pip install pynput")

logging.basicConfig(level=logging.INFO, format="%(asctime)s %(levelname)s %(message)s")
log = logging.getLogger("FloorPiano")

# ── Key maps ──────────────────────────────────────────────────────────────────
# Map floor key index (0-23) → keyboard key sent to the OS.
PIANO_MAP: dict[int, str] = {
     0: 'z',   1: 's',   2: 'x',   3: 'd',   4: 'c',   5: 'v',
     6: 'g',   7: 'b',   8: 'h',   9: 'n',  10: 'j',  11: 'm',
    12: 'q',  13: '2',  14: 'w',  15: '3',  16: 'e',  17: 'r',
    18: '5',  19: 't',  20: '6',  21: 'y',  22: '7',  23: 'u',
}
KEYS_MAP: dict[int, str] = {
    i: k for i, k in enumerate(list('1234567890') + list('abcdefghijklmn'))
}

kb = Controller()


def resolve_key(ch: str):
    """Turn a character string into a pynput pressable."""
    if len(ch) == 1:
        return KeyCode.from_char(ch)
    attr = getattr(Key, ch, None)          # named keys: 'space', 'enter', …
    return attr if attr else KeyCode.from_char(ch[0])


def resolve_map(name: str) -> dict[int, str]:
    if name == "piano":
        return dict(PIANO_MAP)
    if name == "keys":
        return dict(KEYS_MAP)
    with open(name) as f:                   # else treat as a JSON file path
        raw = json.load(f)
    return {int(k): v for k, v in raw.items()}


# ── WebSocket worker (runs its own asyncio loop in a background thread) ──────────
class WSWorker(threading.Thread):
    def __init__(self, ip, key_map, repeat_enabled, repeat_delay, repeat_interval,
                 status_cb, armed=False):
        super().__init__(daemon=True)
        self.ip = ip
        self.key_map = key_map
        self.repeat_enabled = repeat_enabled        # plain bool, toggled live from GUI
        self.repeat_delay = repeat_delay            # seconds before auto-repeat starts
        self.repeat_interval = repeat_interval      # seconds between repeats
        self.status_cb = status_cb
        self.armed = armed          # only send keystrokes to the OS when armed
        self.loop = None
        self._task = None
        self.key_state = [False] * 24
        self.repeat_tasks: dict[int, asyncio.Task] = {}

    # -- thread entry --
    def run(self):
        self.loop = asyncio.new_event_loop()
        asyncio.set_event_loop(self.loop)
        self._task = self.loop.create_task(self._main())
        try:
            self.loop.run_until_complete(self._task)
        except asyncio.CancelledError:
            pass
        finally:
            self._release_all()
            self.loop.close()
        self.status_cb("stopped", "Stopped.")

    def stop(self):
        if self.loop and self._task and not self._task.done():
            self.loop.call_soon_threadsafe(self._task.cancel)

    # -- arming (gate keystroke output without dropping the connection) --
    def set_armed(self, armed: bool):
        if self.loop:
            self.loop.call_soon_threadsafe(self._apply_armed, armed)

    def _apply_armed(self, armed: bool):
        if armed == self.armed:
            return
        self.armed = armed
        if armed:
            for i, held in enumerate(self.key_state):   # press any pads already held
                if held:
                    self._down(i)
        else:
            self._release_all(keep_state=True)          # release, but remember what's held

    # -- key handling --
    def _down(self, idx):
        ch = self.key_map.get(idx)
        if ch is None or not self.armed:
            return
        key = resolve_key(ch)
        try:
            kb.press(key)
        except Exception as e:
            log.warning(f"press error: {e}")
            return
        if self.repeat_enabled:
            self.repeat_tasks[idx] = self.loop.create_task(self._repeat(key))

    def _up(self, idx):
        t = self.repeat_tasks.pop(idx, None)
        if t:
            t.cancel()
        ch = self.key_map.get(idx)
        if ch is None or not self.armed:
            return
        try:
            kb.release(resolve_key(ch))
        except Exception as e:
            log.warning(f"release error: {e}")

    async def _repeat(self, key):
        # Mimic OS auto-repeat: after an initial delay, re-send key-down events.
        try:
            await asyncio.sleep(self.repeat_delay)
            while True:
                kb.press(key)
                await asyncio.sleep(self.repeat_interval)
        except asyncio.CancelledError:
            pass

    def _release_all(self, keep_state: bool = False):
        for t in self.repeat_tasks.values():
            t.cancel()
        self.repeat_tasks.clear()
        for i, held in enumerate(self.key_state):
            if held:
                ch = self.key_map.get(i)
                if ch:
                    try:
                        kb.release(resolve_key(ch))
                    except Exception:
                        pass
                if not keep_state:
                    self.key_state[i] = False

    def _process_binary(self, data: bytes):
        if len(data) < 4 or data[0] != 0x01:
            return
        for i in range(24):
            new = bool((data[1 + i // 8] >> (i % 8)) & 1)
            if new != self.key_state[i]:
                self.key_state[i] = new
                self.status_cb("key", f"{i},{1 if new else 0}")   # drives the indicator
                if new:
                    self._down(i)
                else:
                    self._up(i)

    # -- connection loop --
    async def _main(self):
        url = f"ws://{self.ip}/ws"
        delay = 2
        while True:
            try:
                self.status_cb("connecting", f"Connecting to {url} …")
                async with websockets.connect(url, ping_interval=10, ping_timeout=5) as socket:
                    self.status_cb("connected", "Connected — keys are live.")
                    delay = 2
                    # Ask the device to start emitting key events (polling is off by default)
                    await socket.send(json.dumps({"cmd": "startPoll"}))
                    async for message in socket:
                        if isinstance(message, bytes):
                            self._process_binary(message)
            except asyncio.CancelledError:
                raise
            except (OSError, websockets.ConnectionClosed,
                    websockets.InvalidURI, websockets.InvalidHandshake) as e:
                self._release_all()
                self.status_cb("disconnected", f"Connection lost: {e}. Retrying in {delay}s…")
                try:
                    await asyncio.sleep(delay)
                except asyncio.CancelledError:
                    raise
                delay = min(delay * 2, 30)


# ── GUI ─────────────────────────────────────────────────────────────────────────
def run_gui(default_ip: str, default_map: str):
    import tkinter as tk
    from tkinter import ttk

    worker: "WSWorker | None" = None
    msgs: "queue.Queue[tuple[str, str]]" = queue.Queue()

    root = tk.Tk()
    root.title("Floor Piano — Keyboard")
    root.resizable(False, False)
    pad = {"padx": 8, "pady": 6}

    frm = ttk.Frame(root, padding=14)
    frm.grid()

    # IP address
    ttk.Label(frm, text="Device IP").grid(row=0, column=0, sticky="w", **pad)
    ip_var = tk.StringVar(value=default_ip)
    ip_entry = ttk.Entry(frm, textvariable=ip_var, width=20)
    ip_entry.grid(row=0, column=1, sticky="w", **pad)

    # Key map
    ttk.Label(frm, text="Key map").grid(row=1, column=0, sticky="w", **pad)
    map_var = tk.StringVar(value=default_map if default_map in ("piano", "keys") else "piano")
    map_box = ttk.Combobox(frm, textvariable=map_var, width=18, state="readonly",
                           values=["piano", "keys"])
    map_box.grid(row=1, column=1, sticky="w", **pad)

    # Repeat on long press
    repeat_var = tk.BooleanVar(value=False)

    def on_repeat_toggle():
        nonlocal worker
        if worker:
            worker.repeat_enabled = repeat_var.get()
        state = "normal" if repeat_var.get() else "disabled"
        delay_spin.configure(state=state)
        rate_spin.configure(state=state)

    ttk.Checkbutton(frm, text="Repeat key while pad is held",
                    variable=repeat_var, command=on_repeat_toggle).grid(
        row=2, column=0, columnspan=2, sticky="w", **pad)

    ttk.Label(frm, text="Repeat delay (ms)").grid(row=3, column=0, sticky="w", **pad)
    delay_var = tk.IntVar(value=400)
    delay_spin = ttk.Spinbox(frm, from_=0, to=2000, increment=50, width=8,
                             textvariable=delay_var, state="disabled")
    delay_spin.grid(row=3, column=1, sticky="w", **pad)

    ttk.Label(frm, text="Repeat rate (ms)").grid(row=4, column=0, sticky="w", **pad)
    rate_var = tk.IntVar(value=60)
    rate_spin = ttk.Spinbox(frm, from_=10, to=1000, increment=10, width=8,
                            textvariable=rate_var, state="disabled")
    rate_spin.grid(row=4, column=1, sticky="w", **pad)

    # Connect  +  Start keyboard
    connect_btn = ttk.Button(frm, text="Connect")
    connect_btn.grid(row=5, column=0, sticky="ew", **pad)
    kbd_btn = ttk.Button(frm, text="Start keyboard", state="disabled")
    kbd_btn.grid(row=5, column=1, sticky="ew", **pad)

    # Status
    status_var = tk.StringVar(value="Idle")
    status_lbl = ttk.Label(frm, textvariable=status_var, foreground="#888")
    status_lbl.grid(row=6, column=0, columnspan=2, sticky="w", **pad)

    # Key indicator (24 dots, lights up as pads are pressed)
    ind = ttk.LabelFrame(frm, text="Keys", padding=6)
    ind.grid(row=7, column=0, columnspan=2, sticky="ew", **pad)
    dots = []
    for i in range(24):
        d = tk.Label(ind, text=f"{i}", width=3, relief="ridge",
                     bg="#dddddd", fg="#666", font=("TkDefaultFont", 8))
        d.grid(row=i // 12, column=i % 12, padx=1, pady=1)
        dots.append(d)

    COLORS = {"connected": "#1a7d3c", "connecting": "#b8860b",
              "disconnected": "#b00020", "stopped": "#888", "idle": "#888"}

    def set_connected(connected: bool):
        st = "disabled" if connected else "normal"
        ip_entry.configure(state=st)
        map_box.configure(state="disabled" if connected else "readonly")
        connect_btn.configure(text="Disconnect" if connected else "Connect")
        kbd_btn.configure(state="normal" if connected else "disabled",
                          text="Start keyboard")
        if not connected:
            for d in dots:
                d.configure(bg="#dddddd", fg="#666")

    def status_cb(kind: str, text: str):
        msgs.put((kind, text))          # called from worker thread → queue → GUI

    def do_connect():
        nonlocal worker
        try:
            key_map = resolve_map(map_var.get())
        except Exception as e:
            status_var.set(f"Map error: {e}")
            status_lbl.configure(foreground=COLORS["disconnected"])
            return
        worker = WSWorker(
            ip=ip_var.get().strip() or "192.168.4.1",
            key_map=key_map,
            repeat_enabled=repeat_var.get(),
            repeat_delay=max(0, delay_var.get()) / 1000.0,
            repeat_interval=max(0.01, rate_var.get() / 1000.0),
            status_cb=status_cb,
        )
        worker.start()
        set_connected(True)

    def do_disconnect():
        nonlocal worker
        if worker:
            worker.stop()
            worker = None
        set_connected(False)
        status_var.set("Disconnected.")
        status_lbl.configure(foreground=COLORS["stopped"])

    def toggle_connect():
        if connect_btn.cget("text") == "Connect":
            do_connect()
        else:
            do_disconnect()

    def toggle_keyboard():
        if not worker:
            return
        starting = kbd_btn.cget("text") == "Start keyboard"
        worker.set_armed(starting)
        kbd_btn.configure(text="Stop keyboard" if starting else "Start keyboard")

    connect_btn.configure(command=toggle_connect)
    kbd_btn.configure(command=toggle_keyboard)

    def poll():
        try:
            while True:
                kind, text = msgs.get_nowait()
                if kind == "key":
                    idx, on = (int(x) for x in text.split(","))
                    armed = kbd_btn.cget("text") == "Stop keyboard"
                    dots[idx].configure(
                        bg=("#e94560" if armed else "#ffd166") if on else "#dddddd",
                        fg="#fff" if on else "#666")
                    continue
                status_var.set(text)
                status_lbl.configure(foreground=COLORS.get(kind, "#888"))
                if kind == "stopped":
                    set_connected(False)
        except queue.Empty:
            pass
        root.after(60, poll)

    def on_close():
        if worker:
            worker.stop()
        root.after(150, root.destroy)

    root.protocol("WM_DELETE_WINDOW", on_close)
    root.after(60, poll)
    root.mainloop()


# ── CLI ───────────────────────────────────────────────────────────────────────
def run_cli(ip: str, key_map: dict[int, str], repeat: bool, delay_ms: int, rate_ms: int):
    print(f"Floor Piano translator  |  device: {ip}  |  repeat: {repeat}")
    print("Press Ctrl-C to stop.\n")
    worker = WSWorker(ip, key_map, repeat, delay_ms / 1000.0,
                      max(0.01, rate_ms / 1000.0),
                      lambda kind, text: None if kind == "key" else log.info(text),
                      armed=True)
    worker.start()
    try:
        while worker.is_alive():
            worker.join(0.2)
    except KeyboardInterrupt:
        print("\nStopping…")
        worker.stop()
        worker.join(2)


def main() -> None:
    parser = argparse.ArgumentParser(description="Floor Piano keyboard translator")
    parser.add_argument("--ip", default="192.168.4.1",
                        help="NodeMCU IP address (default 192.168.4.1 = AP mode)")
    parser.add_argument("--map", default="piano",
                        help="Key map: 'piano' (default), 'keys', or path to JSON file")
    parser.add_argument("--repeat", action="store_true", help="Auto-repeat keys while held (CLI)")
    parser.add_argument("--repeat-delay", type=int, default=400, help="ms before repeat starts")
    parser.add_argument("--repeat-rate", type=int, default=60, help="ms between repeats")
    parser.add_argument("--cli", action="store_true", help="Run headless (no GUI)")
    parser.add_argument("--list", action="store_true", help="Print the active key map and exit")
    args = parser.parse_args()

    # websockets is most robust on the selector loop on Windows
    if sys.platform == "win32":
        asyncio.set_event_loop_policy(asyncio.WindowsSelectorEventLoopPolicy())

    if args.list:
        try:
            km = resolve_map(args.map)
        except Exception as e:
            sys.exit(f"Cannot load map '{args.map}': {e}")
        print("Floor key → keyboard key mapping:")
        for i in range(24):
            print(f"  [{i:2d}] → '{km.get(i, '(none)')}'")
        return

    if args.cli:
        try:
            km = resolve_map(args.map)
        except Exception as e:
            sys.exit(f"Cannot load map '{args.map}': {e}")
        run_cli(args.ip, km, args.repeat, args.repeat_delay, args.repeat_rate)
        return

    try:
        run_gui(args.ip, args.map)
    except ImportError:
        sys.exit("tkinter is not available — run with --cli for headless mode.")


if __name__ == "__main__":
    main()
