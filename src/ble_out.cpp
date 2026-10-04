#include "ble_out.h"

// Whole file is a no-op unless BLE is enabled (i.e. on ESP32 builds).
#ifdef FP_BLE_ENABLED

#include <BleKeyboard.h>
#include <NimBLEDevice.h>             // for stopAdvertising / startAdvertising
#include <BLEMIDI_Transport.h>
// NimBLE backend — matches -DUSE_NIMBLE in platformio.ini so both libraries
// share the single underlying NimBLE stack (Bluedroid and NimBLE are mutually
// exclusive; only one can be linked into the binary).
#include <hardware/BLEMIDI_ESP32_NimBLE.h>

namespace BleOut {

static Mode      g_mode       = ModeKeyboard;
static bool      g_running    = false;
static uint8_t   g_numKeys    = 0;
static uint8_t   g_kbdKeys[32];
static uint8_t   g_midiNotes[32];

// Keyboard mode owns this lazily — constructed on first start(Keyboard).
// We NEVER call g_kb->end() (NimBLE deinit crashes) — once begin() has run, the
// stack stays initialized for the device's lifetime; stop() just halts advertising.
static BleKeyboard* g_kb = nullptr;
static bool         g_kbBegun = false;   // true once g_kb->begin() has been called

// MIDI mode: the BLEMIDI_CREATE_INSTANCE macro creates a static transport +
// MidiInterface pair. Construction is harmless until begin() is called.
// The macro creates a transport variable named BLE<Name> — so with Name=gMidi,
// the transport instance is BLEgMidi (referenced explicitly below for callbacks).
BLEMIDI_CREATE_INSTANCE("FloorPiano", gMidi)
static bool g_midiStarted = false;
static bool g_midiConnected = false;

static void onMidiConnected()    { g_midiConnected = true;  }
static void onMidiDisconnected() { g_midiConnected = false; }

void start(const char* name, Mode mode, uint8_t numKeys,
           const uint8_t* kbdKeys, const uint8_t* midiNotes) {
    if (g_running) stop();

    g_mode    = mode;
    g_numKeys = numKeys > 32 ? 32 : numKeys;
    if (kbdKeys)   memcpy(g_kbdKeys,   kbdKeys,   g_numKeys);
    if (midiNotes) memcpy(g_midiNotes, midiNotes, g_numKeys);

    Serial.printf("[BLE] starting (mode=%s, name='%s'), free heap=%u, largest block=%u\n",
                  mode == ModeKeyboard ? "Keyboard" : "MIDI",
                  name ? name : "FloorPiano",
                  ESP.getFreeHeap(),
                  ESP.getMaxAllocHeap());

    if (mode == ModeKeyboard) {
        if (!g_kb) g_kb = new BleKeyboard(name ? name : "FloorPiano",
                                          "FloorPiano", 100);
        else       g_kb->setName(name ? name : "FloorPiano");
        if (!g_kbBegun) {
            // First time: full NimBLE init + advertising. We can only do this once
            // per boot (end() crashes; see notes above).
            g_kb->begin();
            g_kbBegun = true;
        } else {
            // Subsequent restarts: NimBLE is already up, just resume advertising.
            NimBLEDevice::startAdvertising();
        }
        // Defensive: any leftover modifier/key in the HID report from a previous
        // session is delivered to the host on connect and Windows then auto-repeats
        // it (e.g. a stuck Alt key making it look like the host is "cycling
        // through windows"). Flush the report on every fresh start.
        g_kb->releaseAll();
    } else {
        // BLE-MIDI: the device name is baked in at macro-instantiation time, so
        // we can't honour a custom name here without rebuilding. Document this
        // in the UI rather than working around it.
        gMidi.begin();
        BLEgMidi.setHandleConnected(onMidiConnected);
        BLEgMidi.setHandleDisconnected(onMidiDisconnected);
        g_midiStarted = true;
    }
    Serial.printf("[BLE] started, free heap=%u\n", ESP.getFreeHeap());
    g_running = true;
}

void stop() {
    if (!g_running) return;
    if (g_mode == ModeKeyboard) {
        if (g_kb) {
            // Do NOT call g_kb->end() — NimBLEDevice::deinit() crashes hot. And once
            // BT controller is up we can never restore WIFI_PS_NONE either ("Should
            // enable WiFi modem sleep when both WiFi and Bluetooth are enabled").
            // Instead: release any held keys + stop advertising. Existing pairs
            // disconnect on their own; new pairings can't happen.
            g_kb->releaseAll();
            NimBLEDevice::stopAdvertising();
        }
    } else if (g_midiStarted) {
        // BLE-MIDI shares the same NimBLE stack — same deinit problem applies.
        // We intentionally don't call BLEgMidi.end() here.
        gMidi.sendControlChange(123, 0, 1);   // All Notes Off
        NimBLEDevice::stopAdvertising();
        g_midiConnected = false;
    }
    g_running = false;
}

bool isRunning()   { return g_running; }

bool isConnected() {
    if (!g_running) return false;
    if (g_mode == ModeKeyboard) return g_kb && g_kb->isConnected();
    return g_midiConnected;
}

Mode currentMode() { return g_mode; }

void updateMapping(const uint8_t* kbdKeys, const uint8_t* midiNotes) {
    if (kbdKeys)   memcpy(g_kbdKeys,   kbdKeys,   g_numKeys);
    if (midiNotes) memcpy(g_midiNotes, midiNotes, g_numKeys);
}

void keyDown(uint8_t k) {
    if (!g_running || k >= g_numKeys) return;
    if (g_mode == ModeKeyboard) {
        if (g_kb && g_kb->isConnected()) {
            uint8_t c = g_kbdKeys[k];
            Serial.printf("[BLE] keyDown k=%u char=0x%02X '%c'\n",
                          k, c, (c >= 32 && c < 127) ? (char)c : '?');
            g_kb->press(c);
        }
    } else {
        if (g_midiConnected) gMidi.sendNoteOn(g_midiNotes[k], 100, 1);
    }
}

void keyUp(uint8_t k) {
    if (!g_running || k >= g_numKeys) return;
    if (g_mode == ModeKeyboard) {
        if (g_kb && g_kb->isConnected()) {
            uint8_t c = g_kbdKeys[k];
            Serial.printf("[BLE] keyUp   k=%u char=0x%02X '%c'\n",
                          k, c, (c >= 32 && c < 127) ? (char)c : '?');
            g_kb->release(c);
        }
    } else {
        if (g_midiConnected) gMidi.sendNoteOff(g_midiNotes[k], 0, 1);
    }
}

void releaseAll() {
    if (!g_running) return;
    if (g_mode == ModeKeyboard && g_kb) {
        Serial.println("[BLE] releaseAll()");
        g_kb->releaseAll();
    }
    // BLE-MIDI: send All Notes Off (CC 123) on channel 1.
    if (g_mode == ModeMidi && g_midiConnected) {
        gMidi.sendControlChange(123, 0, 1);
    }
}

} // namespace BleOut

#endif // FP_BLE_ENABLED
