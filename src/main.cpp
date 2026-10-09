#include <Arduino.h>
#include <Wire.h>
#if defined(ESP8266)
  #include <ESP8266WiFi.h>
  #include <ESPAsyncTCP.h>
  #include <Updater.h>
  // Linker symbols marking the LittleFS partition (for filesystem OTA size)
  extern "C" uint32_t _FS_start;
  extern "C" uint32_t _FS_end;
  #define FS_OTA_CMD U_FS
#elif defined(ESP32)
  #include <WiFi.h>
  #include <AsyncTCP.h>
  #include <Update.h>
  #define FS_OTA_CMD U_SPIFFS   // ESP32 writes any FS partition (incl. LittleFS) via U_SPIFFS
#else
  #error "Unsupported platform — build for ESP8266 (nodemcuv2) or ESP32-S3."
#endif
#include <ESPAsyncWebServer.h>
#include <LittleFS.h>
#include <ArduinoJson.h>
#include "mpr121.h"
#include "ble_out.h"

// ── Hardware ──────────────────────────────────────────────────────────────────
#if defined(ESP8266)
  #define I2C_SDA    4    // D2 on NodeMCU
  #define I2C_SCL    5    // D1 on NodeMCU
#else
  #define I2C_SDA    5    // ESP32-S3: change to match your wiring
  #define I2C_SCL    6
#endif
#define MPR_ADDR_0   0x5A
#define MPR_ADDR_1   0x5B
#define NUM_KEYS     24

// ── Network ───────────────────────────────────────────────────────────────────
static const char* AP_SSID = "FloorPiano";
static const char* AP_PASS = "piano1234";

// ── Config ────────────────────────────────────────────────────────────────────
struct KeyCfg {
    uint8_t  tth;       // 0 = use global
    uint8_t  rth;       // 0 = use global
    uint16_t relDelay;  // software release-hold ms
    uint8_t  note;      // MIDI note (default 60+i)
    uint8_t  cdc;       // calibrated charge current (0 = not calibrated / use global)
    uint8_t  cdt;       // calibrated charge time code (0 = not calibrated / use global)
    uint8_t  kbdKey;    // ASCII char to send in BLE keyboard mode (default 'a'+i)
};

struct AppConfig {
    char    wifiSSID[33];
    char    wifiPass[65];
    uint8_t apChannel;   // SoftAP WiFi channel 1-13 (applied at boot)
    uint32_t holdTimeoutMs; // 0 = disabled; forced key-off after continuous hold
    uint8_t globalTTH;
    uint8_t globalRTH;
    uint8_t debounce;
    uint8_t afe1;
    uint8_t afe2;
    uint8_t autoCfg0;
    uint8_t usl, lsl, tl;
    // BLE output (ESP32 only; ignored on ESP8266 build but kept in config so the
    // same config.json works on both platforms)
    uint8_t bleEnabled;  // 1 = auto-start BLE at boot
    uint8_t bleMode;     // 0 = keyboard, 1 = MIDI
    char    bleName[33];
    KeyCfg  keys[NUM_KEYS];
};

static AppConfig cfg;

static void configDefaults() {
    memset(&cfg, 0, sizeof(cfg));
    cfg.apChannel = 1;
    cfg.holdTimeoutMs = 0;
    cfg.globalTTH = 12;
    cfg.globalRTH = 6;
    cfg.debounce  = 0x00;
    cfg.afe1      = 0xFF;
    cfg.afe2      = 0x30;
    cfg.autoCfg0  = 0x0B;
    cfg.usl       = 200;
    cfg.lsl       = 130;
    cfg.tl        = 180;
    cfg.bleEnabled = 0;
    cfg.bleMode    = 0;     // keyboard
    strlcpy(cfg.bleName, "FloorPiano", sizeof(cfg.bleName));
    // Default keyboard map: a..x for keys 0..23 (24 letters of the alphabet).
    for (int i = 0; i < NUM_KEYS; i++) {
        cfg.keys[i] = {0, 0, 0, (uint8_t)(60 + i), 0, 0, (uint8_t)('a' + i)};
    }
}

static bool configLoad() {
    File f = LittleFS.open("/config.json", "r");
    if (!f) return false;

    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, f);
    f.close();
    if (err) return false;

    strlcpy(cfg.wifiSSID, doc["wifi"]["ssid"] | "", sizeof(cfg.wifiSSID));
    strlcpy(cfg.wifiPass, doc["wifi"]["pass"] | "", sizeof(cfg.wifiPass));
    cfg.apChannel = doc["wifi"]["channel"] | cfg.apChannel;
    if (cfg.apChannel < 1 || cfg.apChannel > 13) cfg.apChannel = 1;
    cfg.holdTimeoutMs = doc["holdTimeoutMs"] | cfg.holdTimeoutMs;

    JsonObject b = doc["ble"];
    if (!b.isNull()) {
        // Note: bleEnabled is intentionally NOT loaded — we always boot with
        // BLE off, regardless of what was saved. The user opts in each session.
        cfg.bleMode    = b["mode"] | cfg.bleMode;
        const char* nm = b["name"] | "";
        if (nm[0]) strlcpy(cfg.bleName, nm, sizeof(cfg.bleName));
    }

    JsonObject m = doc["mpr"];
    if (!m.isNull()) {
        cfg.globalTTH = m["tth"]      | cfg.globalTTH;
        cfg.globalRTH = m["rth"]      | cfg.globalRTH;
        cfg.debounce  = m["debounce"] | cfg.debounce;
        cfg.afe1      = m["afe1"]     | cfg.afe1;
        cfg.afe2      = m["afe2"]     | cfg.afe2;
        cfg.autoCfg0  = m["acfg0"]   | cfg.autoCfg0;
        cfg.usl       = m["usl"]      | cfg.usl;
        cfg.lsl       = m["lsl"]      | cfg.lsl;
        cfg.tl        = m["tl"]       | cfg.tl;
    }

    JsonArray keys = doc["keys"];
    for (int i = 0; i < NUM_KEYS && i < (int)keys.size(); i++) {
        cfg.keys[i].tth      = keys[i]["tth"]   | cfg.keys[i].tth;
        cfg.keys[i].rth      = keys[i]["rth"]   | cfg.keys[i].rth;
        cfg.keys[i].relDelay = keys[i]["delay"] | cfg.keys[i].relDelay;
        cfg.keys[i].note     = keys[i]["note"]  | cfg.keys[i].note;
        cfg.keys[i].cdc      = keys[i]["cdc"]   | cfg.keys[i].cdc;
        cfg.keys[i].cdt      = keys[i]["cdt"]   | cfg.keys[i].cdt;
        cfg.keys[i].kbdKey   = keys[i]["kbd"]   | cfg.keys[i].kbdKey;
    }
    return true;
}

static bool configSave() {
    JsonDocument doc;
    doc["wifi"]["ssid"]    = cfg.wifiSSID;
    doc["wifi"]["pass"]    = cfg.wifiPass;
    doc["wifi"]["channel"] = cfg.apChannel;
    doc["holdTimeoutMs"]   = cfg.holdTimeoutMs;

    JsonObject b = doc["ble"].to<JsonObject>();
    b["enabled"] = cfg.bleEnabled;
    b["mode"]    = cfg.bleMode;
    b["name"]    = cfg.bleName;

    JsonObject m = doc["mpr"].to<JsonObject>();
    m["tth"]     = cfg.globalTTH;
    m["rth"]     = cfg.globalRTH;
    m["debounce"]= cfg.debounce;
    m["afe1"]    = cfg.afe1;
    m["afe2"]    = cfg.afe2;
    m["acfg0"]   = cfg.autoCfg0;
    m["usl"]     = cfg.usl;
    m["lsl"]     = cfg.lsl;
    m["tl"]      = cfg.tl;

    JsonArray keys = doc["keys"].to<JsonArray>();
    for (int i = 0; i < NUM_KEYS; i++) {
        JsonObject k = keys.add<JsonObject>();
        k["tth"]   = cfg.keys[i].tth;
        k["rth"]   = cfg.keys[i].rth;
        k["delay"] = cfg.keys[i].relDelay;
        k["note"]  = cfg.keys[i].note;
        k["cdc"]   = cfg.keys[i].cdc;
        k["cdt"]   = cfg.keys[i].cdt;
        k["kbd"]   = cfg.keys[i].kbdKey;
    }

    File f = LittleFS.open("/config.json", "w");
    if (!f) return false;
    serializeJson(doc, f);
    f.close();
    return true;
}

// ── MPR121 ────────────────────────────────────────────────────────────────────
static MPR121 mpr0(MPR_ADDR_0);
static MPR121 mpr1(MPR_ADDR_1);

static MPR121Settings buildMPRSettings() {
    MPR121Settings s;
    s.tth      = cfg.globalTTH;
    s.rth      = cfg.globalRTH;
    s.debounce = cfg.debounce;
    s.afe1     = cfg.afe1;
    s.afe2     = cfg.afe2;
    s.autoCfg0 = cfg.autoCfg0;
    s.usl      = cfg.usl;
    s.lsl      = cfg.lsl;
    s.tl       = cfg.tl;
    return s;
}

static void applyPerKeyThresholds() {
    // The MPR121 only latches threshold-register writes while in stop mode
    // (run bits = 0); writing them while running is ignored. Stop, write, run.
    mpr0.enterStop();
    for (int i = 0; i < 12; i++) {
        uint8_t t = cfg.keys[i].tth   ? cfg.keys[i].tth   : cfg.globalTTH;
        uint8_t r = cfg.keys[i].rth   ? cfg.keys[i].rth   : cfg.globalRTH;
        mpr0.setThreshold(i, t, r);
    }
    mpr0.enterRun();

    mpr1.enterStop();
    for (int i = 0; i < 12; i++) {
        uint8_t t = cfg.keys[12+i].tth ? cfg.keys[12+i].tth : cfg.globalTTH;
        uint8_t r = cfg.keys[12+i].rth ? cfg.keys[12+i].rth : cfg.globalRTH;
        mpr1.setThreshold(i, t, r);
    }
    mpr1.enterRun();
}

// Returns true if any key has a stored (frozen) calibration to re-apply
static bool hasCalibration() {
    for (int i = 0; i < NUM_KEYS; i++)
        if (cfg.keys[i].cdc || cfg.keys[i].cdt) return true;
    return false;
}

// Write the per-key CDC/CDT stored in flash back into the chips. The MPR121's
// per-electrode charge registers are volatile, so frozen calibrations must be
// restored after every power-up.
static void applyPerKeyCharge() {
    mpr0.enterStop();
    mpr1.enterStop();
    for (int i = 0; i < 12; i++) {
        if (cfg.keys[i].cdc)   mpr0.setElectrodeCDC(i, cfg.keys[i].cdc);
        if (cfg.keys[i].cdt)   mpr0.setElectrodeCDT(i, cfg.keys[i].cdt);
        if (cfg.keys[12+i].cdc) mpr1.setElectrodeCDC(i, cfg.keys[12+i].cdc);
        if (cfg.keys[12+i].cdt) mpr1.setElectrodeCDT(i, cfg.keys[12+i].cdt);
    }
    mpr0.enterRun();
    mpr1.enterRun();
}

// Auto-calibrate one chip. The MPR121's hardware auto-config always reconfigures
// *all* enabled electrodes, so to calibrate only a subset we snapshot the current
// per-electrode CDC/CDT, run auto-config, then restore the electrodes that were
// not selected. `selMask` is a 12-bit mask of electrodes to (re)calibrate.
// Results are written into cfg.keys[baseKey + e]; returns the OOR status bits.
static uint16_t calibrateChip(MPR121& mpr, int baseKey, uint16_t selMask, bool freeze) {
    uint8_t prevCdc[12], prevCdt[12];
    for (int i = 0; i < 12; i++) {
        prevCdc[i] = mpr.getElectrodeCDC(i);
        prevCdt[i] = mpr.getElectrodeCDT(i);
    }

    mpr.runAutoConfig(cfg.usl, cfg.lsl, cfg.tl);
    uint16_t oor = mpr.getOORStatus();

    // Restore electrodes that were not part of this calibration request
    mpr.enterStop();
    for (int i = 0; i < 12; i++) {
        if (!((selMask >> i) & 1)) {
            mpr.setElectrodeCDC(i, prevCdc[i]);
            mpr.setElectrodeCDT(i, prevCdt[i]);
        }
    }
    mpr.enterRun();

    if (freeze) mpr.setAutoConfigEnabled(false);

    for (int i = 0; i < 12; i++) {
        cfg.keys[baseKey + i].cdc = mpr.getElectrodeCDC(i);
        cfg.keys[baseKey + i].cdt = mpr.getElectrodeCDT(i);
    }
    return oor;
}

// ── BLE output helpers ────────────────────────────────────────────────────────

// WiFi power-save / BT coexistence. Without BT, WIFI_PS_NONE gives the lowest
// latency (modem never sleeps). But WiFi+BT coexistence on ESP32 *requires* the
// modem to be able to yield — calling BLEDevice::init() with WIFI_PS_NONE aborts
// in coex_core_enable(). When BLE is on we switch to WIFI_PS_MIN_MODEM (sleeps
// only between DTIM beacons, near-zero latency hit while traffic is flowing).
// On ESP8266 the radio sleep mode is configured once in setup and BLE doesn't
// exist, so this is a no-op there.
static void applyWifiPowerSave(bool bleActive) {
#if defined(ESP32)
    WiFi.setSleep(bleActive ? WIFI_PS_MIN_MODEM : WIFI_PS_NONE);
#else
    (void)bleActive;
#endif
}

// Collects the per-key char + MIDI note arrays from cfg and (re)starts the BLE
// stack in the configured mode. Safe to call when already running — it will
// stop first to pick up mode / name / mapping changes.
static void bleStartFromConfig() {
    // CRITICAL: switch WiFi to a coex-compatible power-save mode BEFORE BLE init,
    // or BLEDevice::init() will abort in coex_core_enable().
    applyWifiPowerSave(true);
    delay(20);   // let the radio settle into the new PS mode

    uint8_t kbd[NUM_KEYS], notes[NUM_KEYS];
    for (int i = 0; i < NUM_KEYS; i++) {
        kbd[i]   = cfg.keys[i].kbdKey ? cfg.keys[i].kbdKey : (uint8_t)('a' + i);
        notes[i] = cfg.keys[i].note;
    }
    Serial.printf("[BLE] starting (mode=%s) free heap=%u\n",
                  cfg.bleMode ? "MIDI" : "Keyboard",
                  (unsigned)ESP.getFreeHeap());
    BleOut::start(cfg.bleName,
                  cfg.bleMode ? BleOut::ModeMidi : BleOut::ModeKeyboard,
                  NUM_KEYS, kbd, notes);
    Serial.printf("[BLE] started.            free heap=%u\n",
                  (unsigned)ESP.getFreeHeap());
}

static void blePushMappingFromConfig() {
    uint8_t kbd[NUM_KEYS], notes[NUM_KEYS];
    for (int i = 0; i < NUM_KEYS; i++) {
        kbd[i]   = cfg.keys[i].kbdKey ? cfg.keys[i].kbdKey : (uint8_t)('a' + i);
        notes[i] = cfg.keys[i].note;
    }
    BleOut::updateMapping(kbd, notes);
}

// ── WebSocket / HTTP server ───────────────────────────────────────────────────
static AsyncWebServer server(80);
static AsyncWebSocket  ws("/ws");

// ── Key state ─────────────────────────────────────────────────────────────────
static uint32_t logicalKeyState   = 0;   // 24-bit, with release-hold applied
static uint32_t lastSentKeyState  = 0xFFFFFFFF;
static bool          keyDirty      = false;  // unsent key-state change pending
static unsigned long keyDirtySince = 0;      // when it first became dirty (force timeout)
static unsigned long keyOffAt[NUM_KEYS];
static bool          keyOffPending[NUM_KEYS];
static unsigned long keyPressedAt[NUM_KEYS];
static bool          keyTimedOut[NUM_KEYS];

// Calibration streaming
static bool          calibMode    = false;
static unsigned long lastCalibSend = 0;

// Debug streaming
static bool          debugMode    = false;
static unsigned long lastDebugSend = 0;

// Poll control
static bool          pollEnabled  = false;  // OFF at boot; enabled from the Piano page
static unsigned long lastPoll = 0;
#if defined(ESP32)
  #define POLL_MS 1          // S3 is fast enough to poll at the MPR121's sample rate
#else
  #define POLL_MS 2
#endif
#define DEBUG_SEND_MS 50

// ── Helpers ───────────────────────────────────────────────────────────────────
static void sendKeyStateBinary() {
    uint8_t buf[4];
    buf[0] = 0x01;
    buf[1] = (uint8_t)(logicalKeyState);
    buf[2] = (uint8_t)(logicalKeyState >> 8);
    buf[3] = (uint8_t)(logicalKeyState >> 16);
    ws.binaryAll(buf, 4);
    lastSentKeyState = logicalKeyState;
}

static void sendCalibData() {
    // Skip only if no listeners or memory is tight. We do NOT gate on the send
    // queue here: on a marginal link the library drops the odd frame cleanly, and
    // gating could mute the diagnostic entirely if a client is slow to drain.
    if (ws.count() == 0 || ESP.getFreeHeap() < 9000) return;
    JsonDocument doc;
    doc["type"] = "calib";
    JsonArray arr = doc["data"].to<JsonArray>();

    for (int i = 0; i < 12; i++) {
        JsonObject o = arr.add<JsonObject>();
        o["f"] = mpr0.getFilteredData(i);
        o["b"] = (uint16_t)mpr0.getBaseline(i) << 2;
    }
    for (int i = 0; i < 12; i++) {
        JsonObject o = arr.add<JsonObject>();
        o["f"] = mpr1.getFilteredData(i);
        o["b"] = (uint16_t)mpr1.getBaseline(i) << 2;
    }

    String out;
    serializeJson(doc, out);
    ws.textAll(out);
}

static void sendDebugData() {
    if (ws.count() == 0 || ESP.getFreeHeap() < 9000) return;   // drop, don't wedge
    JsonDocument doc;
    doc["type"] = "debug";
    doc["heap"] = ESP.getFreeHeap();   // for the live diagnostic
    JsonArray arr = doc["data"].to<JsonArray>();

    // Use the chip's own touch status so "hit" reflects the applied thresholds
    // even when the polling loop is off.
    uint16_t ts0 = mpr0.getTouchStatus();
    uint16_t ts1 = mpr1.getTouchStatus();

    for (int i = 0; i < 12; i++) {
        JsonObject o = arr.add<JsonObject>();
        o["k"] = i;
        o["f"] = mpr0.getFilteredData(i);
        o["b"] = (uint16_t)mpr0.getBaseline(i) << 2;
        uint8_t t = cfg.keys[i].tth   ? cfg.keys[i].tth   : cfg.globalTTH;
        uint8_t r = cfg.keys[i].rth   ? cfg.keys[i].rth   : cfg.globalRTH;
        o["th"] = t;
        o["rh"] = r;
        o["p"] = (ts0 >> i) & 1;
    }
    for (int i = 0; i < 12; i++) {
        JsonObject o = arr.add<JsonObject>();
        o["k"] = 12 + i;
        o["f"] = mpr1.getFilteredData(i);
        o["b"] = (uint16_t)mpr1.getBaseline(i) << 2;
        uint8_t t = cfg.keys[12+i].tth ? cfg.keys[12+i].tth : cfg.globalTTH;
        uint8_t r = cfg.keys[12+i].rth ? cfg.keys[12+i].rth : cfg.globalRTH;
        o["th"] = t;
        o["rh"] = r;
        o["p"] = (ts1 >> i) & 1;
    }

    String out;
    serializeJson(doc, out);
    ws.textAll(out);
}

static void sendConfig(AsyncWebSocketClient* client) {
    JsonDocument doc;
    doc["type"] = "config";
    doc["holdTimeoutMs"] = cfg.holdTimeoutMs;
    doc["wifi"]["ssid"]    = cfg.wifiSSID;
    doc["wifi"]["channel"] = cfg.apChannel;
    // never send password back

    JsonObject b = doc["ble"].to<JsonObject>();
    b["enabled"] = cfg.bleEnabled;
    b["mode"]    = cfg.bleMode;
    b["name"]    = cfg.bleName;

    JsonObject m = doc["mpr"].to<JsonObject>();
    m["tth"]     = cfg.globalTTH;
    m["rth"]     = cfg.globalRTH;
    m["debounce"]= cfg.debounce;
    m["afe1"]    = cfg.afe1;
    m["afe2"]    = cfg.afe2;
    m["acfg0"]   = cfg.autoCfg0;
    m["usl"]     = cfg.usl;
    m["lsl"]     = cfg.lsl;
    m["tl"]      = cfg.tl;

    JsonArray keys = doc["keys"].to<JsonArray>();
    for (int i = 0; i < NUM_KEYS; i++) {
        JsonObject k = keys.add<JsonObject>();
        k["tth"]   = cfg.keys[i].tth;
        k["rth"]   = cfg.keys[i].rth;
        k["delay"] = cfg.keys[i].relDelay;
        k["note"]  = cfg.keys[i].note;
        k["cdc"]   = cfg.keys[i].cdc;
        k["cdt"]   = cfg.keys[i].cdt;
        k["kbd"]   = cfg.keys[i].kbdKey;
    }
    doc["autoCfgOn"] = (cfg.autoCfg0 & 0x01) != 0;

    String out;
    serializeJson(doc, out);
    if (client) client->text(out);
    else        ws.textAll(out);
}

static void sendInfo(AsyncWebSocketClient* client) {
    JsonDocument doc;
    doc["type"]    = "info";
    doc["apIP"]    = WiFi.softAPIP().toString();
    doc["staIP"]   = (WiFi.status() == WL_CONNECTED) ? WiFi.localIP().toString() : "";
    doc["mpr0ok"]  = mpr0.isConnected();
    doc["mpr1ok"]  = mpr1.isConnected();
    doc["poll"]    = pollEnabled;
    doc["apChannel"] = cfg.apChannel;
#ifdef FP_BLE_ENABLED
    doc["hasBle"]       = true;
    doc["bleRunning"]   = BleOut::isRunning();
    doc["bleConnected"] = BleOut::isConnected();
    doc["bleMode"]      = cfg.bleMode;   // 0=kbd, 1=midi — drives the Piano output picker
#else
    doc["hasBle"]    = false;
#endif
    String out;
    serializeJson(doc, out);
    client->text(out);
}

// Broadcast just the live BLE status (used after start/stop and on peer connect
// state changes, so the UI's BLE chip can update without re-fetching the full
// info block).
static void broadcastBleStatus() {
    if (ws.count() == 0) return;
    JsonDocument doc;
    doc["type"] = "bleStatus";
#ifdef FP_BLE_ENABLED
    doc["running"]   = BleOut::isRunning();
    doc["connected"] = BleOut::isConnected();
    doc["mode"]      = (int)BleOut::currentMode();
#else
    doc["running"]   = false;
    doc["connected"] = false;
    doc["mode"]      = 0;
#endif
    String out;
    serializeJson(doc, out);
    ws.textAll(out);
}

// ── WiFi channel scan (async, non-blocking) ─────────────────────────────────────
// Surveys nearby APs and recommends the least-congested 2.4 GHz channel. Scanning
// briefly retunes the radio, so the AP may stall for ~2s — the result is broadcast
// to all clients and cached (getScan) so it survives a momentary reconnect.
static bool   scanRunning = false;
static String lastScanJson;

static void startWifiScan() {
    if (scanRunning) return;
    WiFi.scanDelete();
    WiFi.scanNetworks(true /*async*/, false /*hidden*/);
    scanRunning = true;
}

static void buildScanResult() {
    int n = WiFi.scanComplete();
    JsonDocument doc;
    doc["type"] = "wifiScan";
    if (n < 0) {
        doc["error"] = true;
    } else {
        float load[14] = {0};                      // congestion per channel 1..13
        JsonArray nets = doc["networks"].to<JsonArray>();
        for (int i = 0; i < n && i < 30; i++) {
            int ch   = WiFi.channel(i);
            int rssi = WiFi.RSSI(i);
            JsonObject o = nets.add<JsonObject>();
            o["ssid"] = WiFi.SSID(i);
            o["ch"]   = ch;
            o["rssi"] = rssi;
            float w = (rssi + 100) / 70.0f;        // stronger AP = more interference
            if (w < 0.05f) w = 0.05f;
            if (w > 1.0f)  w = 1.0f;
            for (int c = 1; c <= 13; c++) {        // 2.4 GHz channels overlap ±4
                int dch = abs(c - ch);
                if (dch <= 4) load[c] += w * (1.0f - dch / 5.0f);
            }
        }
        float maxL = 0.001f;
        for (int c = 1; c <= 13; c++) if (load[c] > maxL) maxL = load[c];
        JsonArray la = doc["load"].to<JsonArray>();      // 13 values (ch 1..13), 0..100
        for (int c = 1; c <= 13; c++) la.add((int)(load[c] / maxL * 100));
        int best = 1; float bestL = 1e9f;                // prefer non-overlapping 1/6/11
        const int candidates[3] = {1, 6, 11};
        for (int j = 0; j < 3; j++) if (load[candidates[j]] < bestL) { bestL = load[candidates[j]]; best = candidates[j]; }
        doc["recommended"] = best;
        doc["count"] = n;
    }
    String out;
    serializeJson(doc, out);
    lastScanJson = out;
    ws.textAll(out);
    WiFi.scanDelete();
}

static void pollWifiScan() {
    if (!scanRunning) return;
    int n = WiFi.scanComplete();
    if (n == -1) return;                 // still running
    scanRunning = false;
    buildScanResult();                   // n>=0 (ok) or n==-2 (failed → error)
}

static void handleWsMessage(AsyncWebSocketClient* client, uint8_t* data, size_t len) {
    JsonDocument doc;
    if (deserializeJson(doc, data, len) != DeserializationError::Ok) return;

    const char* cmd = doc["cmd"];
    if (!cmd) return;

    if (strcmp(cmd, "getConfig") == 0) {
        sendConfig(client);
        sendInfo(client);
        // Also send initial key state
        uint8_t buf[4] = {0x01,
            (uint8_t)logicalKeyState,
            (uint8_t)(logicalKeyState >> 8),
            (uint8_t)(logicalKeyState >> 16)};
        client->binary(buf, 4);
        return;
    }

    if (strcmp(cmd, "setConfig") == 0) {
        JsonObject d = doc["data"];
        bool mprChanged = false, keyChanged = false;
        bool holdTimeoutChanged = false;

        if (!d["holdTimeoutMs"].isNull()) {
            if (!d["holdTimeoutMs"].is<uint32_t>()) {
                client->text("{\"type\":\"error\",\"message\":\"Hold timeout must be an integer from 0 to 3600000 ms\"}");
                return;
            }
            uint32_t timeoutMs = d["holdTimeoutMs"].as<uint32_t>();
            if (timeoutMs > 3600000UL) {
                client->text("{\"type\":\"error\",\"message\":\"Hold timeout must be an integer from 0 to 3600000 ms\"}");
                return;
            }
            cfg.holdTimeoutMs = timeoutMs;
            holdTimeoutChanged = true;
        }

        // WiFi
        if (d["wifi"]["ssid"].is<const char*>()) {
            strlcpy(cfg.wifiSSID, d["wifi"]["ssid"] | "", sizeof(cfg.wifiSSID));
            strlcpy(cfg.wifiPass, d["wifi"]["pass"] | "", sizeof(cfg.wifiPass));
        }
        if (d["wifi"]["channel"].is<int>()) {
            int ch = d["wifi"]["channel"] | 1;
            cfg.apChannel = (ch >= 1 && ch <= 13) ? ch : 1;   // applied on next boot
        }

        // MPR global
        if (!d["mpr"].isNull()) {
            cfg.globalTTH = d["mpr"]["tth"]      | cfg.globalTTH;
            cfg.globalRTH = d["mpr"]["rth"]      | cfg.globalRTH;
            cfg.debounce  = d["mpr"]["debounce"] | cfg.debounce;
            cfg.afe1      = d["mpr"]["afe1"]     | cfg.afe1;
            cfg.afe2      = d["mpr"]["afe2"]     | cfg.afe2;
            cfg.autoCfg0  = d["mpr"]["acfg0"]   | cfg.autoCfg0;
            cfg.usl       = d["mpr"]["usl"]      | cfg.usl;
            cfg.lsl       = d["mpr"]["lsl"]      | cfg.lsl;
            cfg.tl        = d["mpr"]["tl"]       | cfg.tl;
            mprChanged = true;
        }

        // BLE (live-applies name + mode + enabled; mapping arrives via per-key kbd)
        bool bleChanged = false;
        if (!d["ble"].isNull()) {
            cfg.bleMode    = d["ble"]["mode"]    | cfg.bleMode;
            cfg.bleEnabled = d["ble"]["enabled"] | cfg.bleEnabled;
            const char* nm = d["ble"]["name"] | "";
            if (nm[0]) strlcpy(cfg.bleName, nm, sizeof(cfg.bleName));
            bleChanged = true;
        }

        // Per-key
        JsonArray keys = d["keys"];
        if (!keys.isNull()) {
            for (int i = 0; i < NUM_KEYS && i < (int)keys.size(); i++) {
                cfg.keys[i].tth      = keys[i]["tth"]   | cfg.keys[i].tth;
                cfg.keys[i].rth      = keys[i]["rth"]   | cfg.keys[i].rth;
                cfg.keys[i].relDelay = keys[i]["delay"] | cfg.keys[i].relDelay;
                cfg.keys[i].note     = keys[i]["note"]  | cfg.keys[i].note;
                cfg.keys[i].kbdKey   = keys[i]["kbd"]   | cfg.keys[i].kbdKey;
            }
            keyChanged = true;
        }

        configSave();

        // BLE: push new mapping live (no reconnect needed). Mode / name changes
        // require a restart — only restart if BLE is already running, so toggling
        // these while disabled doesn't surprise-start the radio.
        if (bleChanged && BleOut::isRunning()) {
            bleStartFromConfig();        // tears down + restarts in new mode
            broadcastBleStatus();
        } else if (keyChanged) {
            blePushMappingFromConfig();  // live mapping update
        }

        if (mprChanged) {
            MPR121Settings s = buildMPRSettings();
            mpr0.applySettings(s);
            mpr1.applySettings(s);
        }
        if (keyChanged || mprChanged) {
            applyPerKeyThresholds();
        }

        // Ack — echo the effective per-key thresholds actually stored on the
        // device, so the UI can confirm the write really took (not just locally).
        JsonDocument ack;
        ack["type"] = "ack";
        ack["cmd"]  = "setConfig";
        if (holdTimeoutChanged) ack["holdTimeoutMs"] = cfg.holdTimeoutMs;
        JsonArray ks = ack["keys"].to<JsonArray>();
        for (int i = 0; i < NUM_KEYS; i++) {
            JsonObject o = ks.add<JsonObject>();
            o["t"] = cfg.keys[i].tth ? cfg.keys[i].tth : cfg.globalTTH;
            o["r"] = cfg.keys[i].rth ? cfg.keys[i].rth : cfg.globalRTH;
        }
        String out;
        serializeJson(ack, out);
        client->text(out);
        return;
    }

    if (strcmp(cmd, "setKey") == 0) {
        int idx = doc["key"] | -1;
        if (idx >= 0 && idx < NUM_KEYS) {
            cfg.keys[idx].tth      = doc["tth"]   | cfg.keys[idx].tth;
            cfg.keys[idx].rth      = doc["rth"]   | cfg.keys[idx].rth;
            cfg.keys[idx].relDelay = doc["delay"] | cfg.keys[idx].relDelay;
            cfg.keys[idx].note     = doc["note"]  | cfg.keys[idx].note;
            configSave();
            applyPerKeyThresholds();
        }
        client->text("{\"type\":\"ack\",\"cmd\":\"setKey\"}");
        return;
    }

    if (strcmp(cmd, "startCalib") == 0) { calibMode = true;  return; }
    if (strcmp(cmd, "stopCalib")  == 0) { calibMode = false; return; }

    if (strcmp(cmd, "startDebug") == 0) { debugMode = true;  return; }
    if (strcmp(cmd, "stopDebug")  == 0) { debugMode = false; return; }

    if (strcmp(cmd, "startPoll") == 0) { pollEnabled = true;  return; }
    if (strcmp(cmd, "stopPoll")  == 0) { pollEnabled = false; return; }

    if (strcmp(cmd, "resetBaseline") == 0) {
        // Re-apply settings causes MPR121 to re-calibrate baseline
        MPR121Settings s = buildMPRSettings();
        mpr0.applySettings(s);
        mpr1.applySettings(s);
        applyPerKeyThresholds();
        client->text("{\"type\":\"ack\",\"cmd\":\"resetBaseline\"}");
        return;
    }

    if (strcmp(cmd, "autoCalibrate") == 0) {
        cfg.tl  = doc["tl"]  | 180;
        cfg.usl = doc["usl"] | 200;
        cfg.lsl = doc["lsl"] | 130;
        bool freeze = doc["freeze"] | true;

        // Selection: optional "keys" array of indices. Empty / missing = all keys.
        uint32_t sel = 0;
        JsonArray ksel = doc["keys"];
        if (ksel.isNull() || ksel.size() == 0) {
            sel = 0xFFFFFFUL;   // all 24
        } else {
            for (JsonVariant v : ksel) {
                int k = v.as<int>();
                if (k >= 0 && k < NUM_KEYS) sel |= (1UL << k);
            }
        }
        bool allKeys = (sel == 0xFFFFFFUL);
        // Calibrating a subset only makes sense with fixed values — adaptive
        // auto-config would re-tune every electrode and undo the per-key restore.
        if (!allKeys) freeze = true;

        uint16_t sel0 = sel & 0x0FFF;
        uint16_t sel1 = (sel >> 12) & 0x0FFF;
        uint16_t oor0 = 0, oor1 = 0;
        if (sel0) oor0 = calibrateChip(mpr0, 0,  sel0, freeze);
        if (sel1) oor1 = calibrateChip(mpr1, 12, sel1, freeze);

        // Global auto-config state: only "all keys + adaptive" keeps it running.
        bool frozenGlobal = !(allKeys && !freeze);
        if (frozenGlobal) {
            // Freeze BOTH chips so a subset calibration doesn't leave the other
            // chip silently adapting, and the saved values fully define the state.
            mpr0.setAutoConfigEnabled(false);
            mpr1.setAutoConfigEnabled(false);
            cfg.autoCfg0 = 0x00;
        } else {
            cfg.autoCfg0 = 0x0B;
        }

        // Persist the current per-electrode values for all keys so a frozen
        // calibration survives a power cycle (these registers are volatile).
        for (int i = 0; i < 12; i++) {
            cfg.keys[i].cdc    = mpr0.getElectrodeCDC(i);
            cfg.keys[i].cdt    = mpr0.getElectrodeCDT(i);
            cfg.keys[12+i].cdc = mpr1.getElectrodeCDC(i);
            cfg.keys[12+i].cdt = mpr1.getElectrodeCDT(i);
        }
        configSave();
        applyPerKeyThresholds();   // thresholds are unaffected by stop/run cycling

        JsonDocument res;
        res["type"]   = "calibResult";
        res["frozen"] = freeze;
        res["subset"] = !allKeys;
        JsonArray arr = res["keys"].to<JsonArray>();
        for (int i = 0; i < NUM_KEYS; i++) {
            bool selected = (sel >> i) & 1;
            JsonObject o = arr.add<JsonObject>();
            o["cdc"]  = cfg.keys[i].cdc;
            o["cdt"]  = cfg.keys[i].cdt;
            o["sel"]  = selected;
            o["oor"]  = selected && ((i < 12) ? ((oor0 >> i) & 1)
                                              : ((oor1 >> (i - 12)) & 1));
            o["base"] = (i < 12) ? ((uint16_t)mpr0.getBaseline(i) << 2)
                                 : ((uint16_t)mpr1.getBaseline(i - 12) << 2);
            o["filt"] = (i < 12) ? mpr0.getFilteredData(i)
                                 : mpr1.getFilteredData(i - 12);
        }
        String out;
        serializeJson(res, out);
        client->text(out);
        return;
    }

    if (strcmp(cmd, "setAdaptive") == 0) {
        // Hand control back to continuous hardware auto-config (re-runs each boot)
        mpr0.setAutoConfigEnabled(true);
        mpr1.setAutoConfigEnabled(true);
        cfg.autoCfg0 = 0x0B;
        for (int i = 0; i < NUM_KEYS; i++) { cfg.keys[i].cdc = 0; cfg.keys[i].cdt = 0; }
        configSave();
        client->text("{\"type\":\"ack\",\"cmd\":\"setAdaptive\"}");
        return;
    }

    if (strcmp(cmd, "resetRecover") == 0) {
        // One-click clean slate for a new environment: drop frozen per-key charge
        // AND thresholds, return to adaptive auto-config, then reboot so the chips
        // re-calibrate fresh on the current floor. Re-run calibration + wizard after.
        cfg.autoCfg0 = 0x0B;
        for (int i = 0; i < NUM_KEYS; i++) {
            cfg.keys[i].cdc = 0; cfg.keys[i].cdt = 0;   // adaptive charge
            cfg.keys[i].tth = 0; cfg.keys[i].rth = 0;   // back to global thresholds
        }
        configSave();
        client->text("{\"type\":\"ack\",\"cmd\":\"resetRecover\"}");
        delay(200);
        ESP.restart();
        return;
    }

    if (strcmp(cmd, "scanWifi") == 0) {
        startWifiScan();
        return;
    }

    if (strcmp(cmd, "getScan") == 0) {
        if (lastScanJson.length()) client->text(lastScanJson);
        return;
    }

    if (strcmp(cmd, "startBle") == 0) {
#ifdef FP_BLE_ENABLED
        bleStartFromConfig();
        cfg.bleEnabled = 1;
        configSave();
        // BLE is only useful if the polling loop is running (that's where
        // key events feed into BleOut::keyDown/keyUp). It also keeps the
        // MPR121 IRQ line clearing, which otherwise stays asserted forever
        // and looks like a stuck-on LED to the user.
        pollEnabled = true;
        broadcastBleStatus();
        sendInfo(client);   // tell the requester the new poll state
        client->text("{\"type\":\"ack\",\"cmd\":\"startBle\"}");
#else
        client->text("{\"type\":\"ack\",\"cmd\":\"startBle\",\"unsupported\":true}");
#endif
        return;
    }

    if (strcmp(cmd, "bleReleaseAll") == 0) {
#ifdef FP_BLE_ENABLED
        BleOut::releaseAll();
#endif
        client->text("{\"type\":\"ack\",\"cmd\":\"bleReleaseAll\"}");
        return;
    }

    if (strcmp(cmd, "stopBle") == 0) {
#ifdef FP_BLE_ENABLED
        BleOut::stop();
        // NOTE: do NOT restore WIFI_PS_NONE here. The BLE/NimBLE stack stays
        // initialized (deinit crashes), and once BT controller is up the WiFi
        // driver requires modem sleep ("Should enable WiFi modem sleep when
        // both WiFi and Bluetooth are enabled" → abort). WIFI_PS_MIN_MODEM
        // stays for the rest of this boot, until a reboot.
        cfg.bleEnabled = 0;
        configSave();
        broadcastBleStatus();
        client->text("{\"type\":\"ack\",\"cmd\":\"stopBle\"}");
#else
        client->text("{\"type\":\"ack\",\"cmd\":\"stopBle\",\"unsupported\":true}");
#endif
        return;
    }

    if (strcmp(cmd, "reboot") == 0) {
        client->text("{\"type\":\"ack\",\"cmd\":\"reboot\"}");
        delay(200);
        ESP.restart();
        return;
    }
}

// Inbound WS commands are queued here and handled in loop() — NEVER in the async
// TCP callback. Heavy work (JSON parse, LittleFS writes, I2C, delay()) on the
// callback's small system stack crashes the ESP8266.
struct PendingCmd { uint32_t clientId; String data; };
// Was 8 — too small during slow operations like BLE init that block loop() for
// ~1s; the browser would pile up status/retry commands and overflow. 32 leaves
// plenty of slack while staying well under the WS lib's own queue limits.
static const uint8_t PENDING_MAX = 32;
static PendingCmd  pendingCmds[PENDING_MAX];
static uint8_t     pendHead = 0, pendCount = 0;

static void enqueueWsMessage(uint32_t clientId, const char* data, size_t len) {
    if (pendCount >= PENDING_MAX) return;          // flood guard (shouldn't happen)
    uint8_t slot = (pendHead + pendCount) % PENDING_MAX;
    pendingCmds[slot].clientId = clientId;
    pendingCmds[slot].data = String();
    pendingCmds[slot].data.reserve(len + 1);
    pendingCmds[slot].data.concat(data, (unsigned int)len);
    pendCount++;
}

static void processPendingWs() {
    while (pendCount > 0) {
        PendingCmd& p = pendingCmds[pendHead];
        AsyncWebSocketClient* c = ws.client(p.clientId);   // may be gone
        if (c) handleWsMessage(c, (uint8_t*)p.data.c_str(), p.data.length());
        p.data = String();                                 // free the buffer
        pendHead = (pendHead + 1) % PENDING_MAX;
        pendCount--;
    }
}

static void onWsEvent(AsyncWebSocket* server, AsyncWebSocketClient* client,
                      AwsEventType type, void* arg, uint8_t* data, size_t len) {
    switch (type) {
        case WS_EVT_CONNECT:
            Serial.printf("WS client #%u connected\n", client->id());
            // Disable Nagle's algorithm so key events ship immediately instead of
            // being buffered up to ~40ms waiting to coalesce with other data.
            if (client->client()) client->client()->setNoDelay(true);
            break;
        case WS_EVT_DISCONNECT:
            Serial.printf("WS client #%u disconnected\n", client->id());
            break;
        case WS_EVT_DATA: {
            AwsFrameInfo* info = (AwsFrameInfo*)arg;
            if (info->opcode != WS_TEXT) break;

            if (info->final && info->index == 0 && info->len == len) {
                // Whole message in one callback (common, small case) — just queue it.
                enqueueWsMessage(client->id(), (const char*)data, len);
            } else {
                // Larger messages (e.g. the 24-key setConfig) arrive split across
                // several callbacks / frames — reassemble, then queue.
                static String wsBuf;
                if (info->num == 0 && info->index == 0) wsBuf = "";
                wsBuf.concat((const char*)data, (unsigned int)len);
                if (info->final && (info->index + len) == info->len) {
                    enqueueWsMessage(client->id(), wsBuf.c_str(), wsBuf.length());
                    wsBuf = "";
                }
            }
            break;
        }
        default: break;
    }
}

// ── OTA firmware / filesystem upload ──────────────────────────────────────────
// Two endpoints, each takes a multipart file POST:
//   POST /ota/firmware    → flash a new firmware .bin   (U_FLASH)
//   POST /ota/filesystem  → flash a new littlefs.bin    (U_FS)
// On success the response is "OK" and the device reboots.
static bool otaError = false;

static void handleOTAUpload(AsyncWebServerRequest* request, const String& filename,
                            size_t index, uint8_t* data, size_t len, bool final,
                            int command) {
    if (index == 0) {
        otaError = false;
        bool isFirmware = (command == U_FLASH);
        Serial.printf("OTA start (%s): %s\n",
                      isFirmware ? "firmware" : "filesystem", filename.c_str());
        if (!isFirmware) LittleFS.end();   // unmount before overwriting the FS partition

#if defined(ESP8266)
        uint32_t maxSize = isFirmware
            ? ((ESP.getFreeSketchSpace() - 0x1000) & 0xFFFFF000)
            : ((uint32_t)&_FS_end - (uint32_t)&_FS_start);
        Update.runAsync(true);
        if (!Update.begin(maxSize, command)) {
#else  // ESP32: the Update library figures out the partition size itself
        if (!Update.begin(UPDATE_SIZE_UNKNOWN, command)) {
#endif
            Update.printError(Serial);
            otaError = true;
        }
    }

    if (!otaError && len) {
        if (Update.write(data, len) != len) {
            Update.printError(Serial);
            otaError = true;
        }
    }

    if (final) {
        if (!otaError && Update.end(true)) {
            Serial.printf("OTA success: %u bytes\n", index + len);
        } else {
            Update.printError(Serial);
            otaError = true;
        }
    }
}

static void registerOTA() {
    auto finish = [](AsyncWebServerRequest* request) {
        bool ok = !Update.hasError() && !otaError;
        AsyncWebServerResponse* res =
            request->beginResponse(ok ? 200 : 500, "text/plain", ok ? "OK" : "FAIL");
        res->addHeader("Connection", "close");
        request->send(res);
        if (ok) { delay(200); ESP.restart(); }
    };

    server.on("/ota/firmware", HTTP_POST, finish,
        [](AsyncWebServerRequest* r, String fn, size_t i, uint8_t* d, size_t l, bool f) {
            handleOTAUpload(r, fn, i, d, l, f, U_FLASH);
        });

    server.on("/ota/filesystem", HTTP_POST, finish,
        [](AsyncWebServerRequest* r, String fn, size_t i, uint8_t* d, size_t l, bool f) {
            handleOTAUpload(r, fn, i, d, l, f, FS_OTA_CMD);
        });
}

// ── setup ─────────────────────────────────────────────────────────────────────
void setup() {
    Serial.begin(115200);
    delay(100);
    Serial.println("\n\n=== Floor Piano ===");

    Wire.begin(I2C_SDA, I2C_SCL);
    Wire.setClock(400000);

    if (!LittleFS.begin()) {
        Serial.println("LittleFS mount failed – check filesystem upload");
    }

    configDefaults();
    if (!configLoad()) {
        Serial.println("No config found, using defaults");
        configSave();
    }

    // WiFi: always start AP, optionally join STA
    WiFi.mode(WIFI_AP_STA);
    // Disable modem power-save — this is the single biggest latency win. With it on,
    // the radio sleeps between beacons and key events can be delayed by 100ms+.
#if defined(ESP8266)
    WiFi.setSleepMode(WIFI_NONE_SLEEP);
#else
    WiFi.setSleep(false);
#endif
    WiFi.softAP(AP_SSID, AP_PASS, cfg.apChannel);
    Serial.printf("AP: %s  IP: %s\n", AP_SSID, WiFi.softAPIP().toString().c_str());

    if (strlen(cfg.wifiSSID) > 0) {
        WiFi.begin(cfg.wifiSSID, cfg.wifiPass);
        Serial.printf("Connecting to %s", cfg.wifiSSID);
        for (int i = 0; i < 20 && WiFi.status() != WL_CONNECTED; i++) {
            delay(500); Serial.print(".");
        }
        if (WiFi.status() == WL_CONNECTED)
            Serial.printf("\nSTA IP: %s\n", WiFi.localIP().toString().c_str());
        else
            Serial.println("\nSTA connect failed, AP-only mode");
    }

    // MPR121 init
    MPR121Settings s = buildMPRSettings();
    if (!mpr0.begin(s)) Serial.println("MPR121 #0 not found! Check I2C / address ADDR=GND");
    if (!mpr1.begin(s)) Serial.println("MPR121 #1 not found! Check I2C / address ADDR=VCC");
    applyPerKeyThresholds();

    // Restore a frozen per-key calibration (volatile in the chip across power-ups)
    if (!(cfg.autoCfg0 & 0x01) && hasCalibration()) {
        applyPerKeyCharge();
        Serial.println("Restored frozen per-key CDC/CDT calibration");
    }

    // HTTP: serve web app from LittleFS
    server.serveStatic("/", LittleFS, "/").setDefaultFile("index.html");

    // WebSocket
    ws.onEvent(onWsEvent);
    server.addHandler(&ws);

    // OTA firmware / filesystem upload endpoints
    registerOTA();

    server.begin();
    Serial.println("HTTP + WebSocket server started on port 80");

    memset(keyOffAt,      0, sizeof(keyOffAt));
    memset(keyOffPending, 0, sizeof(keyOffPending));
    memset(keyPressedAt,  0, sizeof(keyPressedAt));
    memset(keyTimedOut,   0, sizeof(keyTimedOut));

    // BLE is intentionally OFF at every boot — the user must opt in each session
    // via System → Bluetooth → Start BLE. Reasons: paired hosts can flap until
    // the user is actually nearby, BLE consumes battery, and a quiet boot is
    // easier to debug. cfg.bleMode / bleName / per-key kbd mapping are still
    // persisted so the saved settings come back, just not the on/off state.
}

// ── loop ──────────────────────────────────────────────────────────────────────
void loop() {
    ws.cleanupClients();
    processPendingWs();          // handle queued commands here, not in the TCP callback
    pollWifiScan();              // finish an in-progress WiFi channel scan

    unsigned long now = millis();

    if (pollEnabled && (now - lastPoll >= POLL_MS)) {
        lastPoll = now;

        uint16_t s0 = mpr0.getTouchStatus() & 0x0FFF;
        uint16_t s1 = mpr1.getTouchStatus() & 0x0FFF;
        uint32_t raw = ((uint32_t)s1 << 12) | s0;

        bool changed = false;
        for (int i = 0; i < NUM_KEYS; i++) {
            bool rawOn  = (raw >> i) & 1;
            bool logOn  = (logicalKeyState >> i) & 1;
            uint16_t holdMs = cfg.keys[i].relDelay;

            if (rawOn) {
                if (!keyTimedOut[i] && !logOn) {
                    logicalKeyState |= (1UL << i);
                    BleOut::keyDown(i);
                    keyPressedAt[i] = now;
                    changed = true;
                }
                if (keyTimedOut[i]) continue;
                keyOffPending[i] = false;   // still pressed, cancel pending off
                if (cfg.holdTimeoutMs > 0 && logOn &&
                    (uint32_t)(now - keyPressedAt[i]) >= cfg.holdTimeoutMs) {
                    logicalKeyState &= ~(1UL << i);
                    BleOut::keyUp(i);
                    keyTimedOut[i] = true;
                    keyOffPending[i] = false;
                    changed = true;
                }
            } else {
                if (keyTimedOut[i]) {
                    keyTimedOut[i] = false;
                    keyOffPending[i] = false;
                    continue;
                }
                if (logOn) {
                    if (holdMs == 0) {
                        logicalKeyState &= ~(1UL << i);
                        BleOut::keyUp(i);
                        keyOffPending[i] = false;
                        changed = true;
                    } else {
                        if (!keyOffPending[i]) {
                            keyOffPending[i] = true;
                            keyOffAt[i] = now + holdMs;
                        } else if (now >= keyOffAt[i]) {
                            logicalKeyState &= ~(1UL << i);
                            BleOut::keyUp(i);
                            keyOffPending[i] = false;
                            changed = true;
                        }
                    }
                }
            }
        }

        if (changed) { if (!keyDirty) keyDirtySince = now; keyDirty = true; }
    }

    // Output exclusivity: BLE and the WS binary key-state stream are mutually
    // exclusive — Start BLE / Stop BLE is the toggle. Only one path delivers
    // key events to a host at any time, so the browser piano / Python bridge /
    // SoundFont player / TurboWarp aren't doubling up with the BLE keyboard.
    // (Debug, calib, info, bleStatus streams keep flowing — they're for the
    // config UI, not for input delivery.)
    //
    // We keep keyDirty *true* during BLE mode (don't clear it) so the moment BLE
    // is stopped, the next loop pass re-syncs the browser visual with the current
    // logicalKeyState. While BLE is on, the visual freezes at the last frame.
    if (!BleOut::isRunning() && keyDirty && ws.count()) {
        // Send the latest key state when the socket can accept it — coalescing rapid
        // changes so we never outrun a slow client and overflow the WS queue. But if
        // it has been blocked too long (a wedged/half-open client), force the send
        // rather than going dark; the library drops cleanly if the queue is truly full.
        bool forced = (now - keyDirtySince) >= 250;
        if (ws.availableForWriteAll() || forced) {
            sendKeyStateBinary();                 // updates lastSentKeyState
            keyDirty = (logicalKeyState != lastSentKeyState);
            if (keyDirty) keyDirtySince = now;    // restart the timer for the next try
        }
    }

    // Calibration data (slower, 100ms)
    if (calibMode && (now - lastCalibSend >= 100)) {
        lastCalibSend = now;
        sendCalibData();
    }

    // Debug data (when enabled, 50ms)
    if (debugMode && (now - lastDebugSend >= DEBUG_SEND_MS)) {
        lastDebugSend = now;
        sendDebugData();
    }

#ifdef FP_BLE_ENABLED
    // Push a BLE status update to clients when the peer connection state flips
    // (e.g. the phone/PC pairs or drops). 200 ms polling is plenty for a chip
    // indicator and adds no measurable load.
    static bool          lastBleConnected = false;
    static unsigned long lastBleCheck = 0;
    if (now - lastBleCheck >= 200) {
        lastBleCheck = now;
        bool c = BleOut::isConnected();
        if (c != lastBleConnected) {
            lastBleConnected = c;
            broadcastBleStatus();
        }
    }
#endif
}
