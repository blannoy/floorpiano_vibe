// Floor Piano — TurboWarp / Scratch extension
// Receives key (pad) events from the NodeMCU/ESP32 floor piano over its WebSocket
// API and exposes them as Scratch blocks.
//
// Load it in TurboWarp:  Extensions (bottom-left)  →  Custom Extension  →
//   "File" tab → choose this file  →  tick "Run extension without sandbox"  →  Load.
//
// Then use:  [connect to floor piano at (192.168.4.1)]  and the hat/boolean blocks.

(function (Scratch) {
  'use strict';

  const NUM_KEYS = 24;

  class FloorPiano {
    constructor() {
      this.ws = null;
      this.ip = '192.168.4.1';
      this.want = false;            // user asked to be connected
      this.connected = false;
      this.keys = new Array(NUM_KEYS).fill(false);
      this.notes = Array.from({ length: NUM_KEYS }, (_, i) => 60 + i);
      this.lastPressed = -1;
      this._reconnectTimer = null;
    }

    getInfo() {
      const keyMenu = Array.from({ length: NUM_KEYS }, (_, i) => ({ text: String(i), value: String(i) }));
      return {
        id: 'floorpiano',
        name: 'Floor Piano',
        color1: '#e94560',
        color2: '#c73050',
        blocks: [
          { opcode: 'connect', blockType: Scratch.BlockType.COMMAND,
            text: 'connect to floor piano at [IP]',
            arguments: { IP: { type: Scratch.ArgumentType.STRING, defaultValue: '192.168.4.1' } } },
          { opcode: 'disconnect', blockType: Scratch.BlockType.COMMAND,
            text: 'disconnect from floor piano' },
          { opcode: 'isConnected', blockType: Scratch.BlockType.BOOLEAN, text: 'connected?' },
          '---',
          { opcode: 'whenKey', blockType: Scratch.BlockType.HAT,
            text: 'when key [N] pressed', isEdgeActivated: true,
            arguments: { N: { type: Scratch.ArgumentType.NUMBER, menu: 'keys', defaultValue: '0' } } },
          { opcode: 'whenAnyKey', blockType: Scratch.BlockType.HAT,
            text: 'when any key pressed', isEdgeActivated: true },
          { opcode: 'isKeyDown', blockType: Scratch.BlockType.BOOLEAN,
            text: 'key [N] pressed?',
            arguments: { N: { type: Scratch.ArgumentType.NUMBER, menu: 'keys', defaultValue: '0' } } },
          '---',
          { opcode: 'lastKey', blockType: Scratch.BlockType.REPORTER, text: 'last pressed key' },
          { opcode: 'numPressed', blockType: Scratch.BlockType.REPORTER, text: 'number of keys pressed' },
          { opcode: 'noteOf', blockType: Scratch.BlockType.REPORTER,
            text: 'MIDI note of key [N]',
            arguments: { N: { type: Scratch.ArgumentType.NUMBER, menu: 'keys', defaultValue: '0' } } },
        ],
        menus: {
          keys: { acceptReporters: true, items: keyMenu },
        },
      };
    }

    // ── blocks ──
    connect(args) {
      this.ip = String(args.IP || '192.168.4.1').trim();
      this.want = true;
      this._open();
    }

    disconnect() {
      this.want = false;
      clearTimeout(this._reconnectTimer);
      if (this.ws) { try { this.ws.close(); } catch (e) {} this.ws = null; }
      this._allUp();
      this.connected = false;
    }

    isConnected() { return this.connected; }

    whenKey(args) { return this._on(args.N); }
    whenAnyKey() { return this.keys.some(Boolean); }
    isKeyDown(args) { return this._on(args.N); }
    lastKey() { return this.lastPressed; }
    numPressed() { return this.keys.reduce((n, k) => n + (k ? 1 : 0), 0); }
    noteOf(args) {
      const i = this._idx(args.N);
      return i < 0 ? 0 : this.notes[i];
    }

    // ── helpers ──
    _idx(n) { const i = parseInt(n, 10); return (i >= 0 && i < NUM_KEYS) ? i : -1; }
    _on(n) { const i = this._idx(n); return i >= 0 && this.keys[i]; }

    _allUp() { for (let i = 0; i < NUM_KEYS; i++) this.keys[i] = false; }

    _open() {
      if (this.ws) { try { this.ws.close(); } catch (e) {} }
      let ws;
      try {
        ws = new WebSocket('ws://' + this.ip + '/ws');
      } catch (e) { this._scheduleReconnect(); return; }
      ws.binaryType = 'arraybuffer';
      this.ws = ws;

      ws.onopen = () => {
        this.connected = true;
        try {
          ws.send(JSON.stringify({ cmd: 'getConfig' }));  // learn per-key MIDI notes
          ws.send(JSON.stringify({ cmd: 'startPoll' }));   // make the device emit events
        } catch (e) {}
      };
      ws.onclose = () => {
        this.connected = false;
        this._allUp();
        if (this.want) this._scheduleReconnect();
      };
      ws.onerror = () => { try { ws.close(); } catch (e) {} };
      ws.onmessage = (ev) => {
        if (ev.data instanceof ArrayBuffer) this._onBinary(new Uint8Array(ev.data));
        else if (typeof ev.data === 'string') this._onText(ev.data);
      };
    }

    _scheduleReconnect() {
      clearTimeout(this._reconnectTimer);
      if (this.want) this._reconnectTimer = setTimeout(() => this._open(), 2000);
    }

    _onBinary(d) {
      if (d.length < 4 || d[0] !== 0x01) return;
      for (let i = 0; i < NUM_KEYS; i++) {
        const on = !!((d[1 + (i >> 3)] >> (i & 7)) & 1);
        if (on && !this.keys[i]) this.lastPressed = i;   // newly pressed
        this.keys[i] = on;
      }
    }

    _onText(s) {
      try {
        const m = JSON.parse(s);
        if (m && m.type === 'config' && Array.isArray(m.keys)) {
          m.keys.forEach((k, i) => { if (k && k.note != null) this.notes[i] = k.note; });
        }
      } catch (e) {}
    }
  }

  Scratch.extensions.register(new FloorPiano());
})(Scratch);
