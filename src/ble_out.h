// BLE output for the floor piano (ESP32 only — ESP8266 has no Bluetooth radio).
// Two mutually-exclusive modes:
//   Keyboard — HID keypress per key (drop-in replacement for the Python pynput
//              bridge; pairs with PC/tablet, no host software needed).
//   MIDI     — BLE-MIDI Note On/Off using each key's MIDI note (works with
//              GarageBand, DAWs, etc.).
//
// The library you pick at start() time owns the BLE stack until stop() — changing
// mode requires stop() + start() (or a reboot). isConnected() reflects whether a
// peer is paired and listening; while only advertising, key events are dropped.
#pragma once
#include <Arduino.h>

namespace BleOut {

enum Mode : uint8_t { ModeKeyboard = 0, ModeMidi = 1 };

#ifdef FP_BLE_ENABLED

// Start advertising as a BLE device. `kbdKeys` and `midiNotes` are arrays of
// length `numKeys` — only the one matching `mode` is read, but pass both so
// updateMapping() can hot-swap without remembering which is active.
void start(const char* name, Mode mode, uint8_t numKeys,
           const uint8_t* kbdKeys, const uint8_t* midiNotes);

// Stop advertising and release the BLE stack. Safe to call when not running.
void stop();

// True between start() and stop() (whether or not a peer is connected).
bool isRunning();

// True once a peer has paired (HID host or MIDI host).
bool isConnected();

Mode currentMode();

// Update per-key mapping without restarting BLE. Either array may be null to
// leave that map unchanged. Lengths must match the numKeys passed to start().
void updateMapping(const uint8_t* kbdKeys, const uint8_t* midiNotes);

// Key event hooks — call once per logical key state edge (matches the existing
// WS binary key-state send path). Indices are 0..numKeys-1. No-op if not running
// or if no peer is connected.
void keyDown(uint8_t k);
void keyUp(uint8_t k);

// Force-release every held key / note. Use as a "panic" if the host (e.g. Windows)
// gets a stuck modifier and starts auto-repeating.
void releaseAll();

#else  // !FP_BLE_ENABLED → empty inline stubs so the call sites compile cleanly.

inline void start(const char*, Mode, uint8_t, const uint8_t*, const uint8_t*) {}
inline void stop() {}
inline bool isRunning()   { return false; }
inline bool isConnected() { return false; }
inline Mode currentMode() { return ModeKeyboard; }
inline void updateMapping(const uint8_t*, const uint8_t*) {}
inline void keyDown(uint8_t) {}
inline void keyUp(uint8_t)   {}
inline void releaseAll()     {}

#endif

} // namespace BleOut
