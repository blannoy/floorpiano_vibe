#pragma once
#include <Arduino.h>
#include <Wire.h>

// ── Register map ──────────────────────────────────────────────────────────────
#define MPR_TOUCH_STATUS    0x00  // 2 bytes: bit per electrode (0-11)
#define MPR_OOR_STATUS_0    0x02  // auto-config out-of-range flags, electrodes 0-7
#define MPR_OOR_STATUS_1    0x03  // auto-config out-of-range flags, electrodes 8-11
#define MPR_FILT_DATA       0x04  // 0x04-0x1D: 2 bytes per electrode (10-bit)
#define MPR_BASELINE        0x1E  // 0x1E-0x29: 1 byte per electrode (MSB of baseline)
#define MPR_MHD_RISING      0x2B
#define MPR_NHD_RISING      0x2C
#define MPR_NCL_RISING      0x2D
#define MPR_FDL_RISING      0x2E
#define MPR_MHD_FALLING     0x2F
#define MPR_NHD_FALLING     0x30
#define MPR_NCL_FALLING     0x31
#define MPR_FDL_FALLING     0x32
#define MPR_ELE0_TTH        0x41  // Touch threshold electrode 0  (stride: 2 bytes per electrode)
#define MPR_ELE0_RTH        0x42  // Release threshold electrode 0
#define MPR_DEBOUNCE        0x5B
#define MPR_AFE1            0x5C  // bits[7:6]=FFI  bits[5:0]=CDC (charge current 0-63 µA)
#define MPR_AFE2            0x5D  // bits[7:5]=CDT  bits[4:3]=SFI  bits[2:0]=ESI
#define MPR_ELEC_CFG        0x5E  // bits[7:6]=CL  bits[5:4]=ELEPROX_EN  bits[3:0]=ELE_EN
#define MPR_ELE0_CDC        0x5F  // 0x5F-0x6A: per-electrode charge current (6-bit, 0-63 µA)
#define MPR_ELE0_CDT        0x6C  // 0x6C-0x71: per-electrode charge time, packed 2/byte (3-bit)
#define MPR_AUTO_CFG0       0x7B
#define MPR_AUTO_CFG1       0x7C
#define MPR_USL             0x7D  // auto-config upper side limit
#define MPR_LSL             0x7E  // auto-config lower side limit
#define MPR_TL              0x7F  // auto-config target level
#define MPR_SOFT_RESET      0x80

// ── Settings struct passed to begin() ─────────────────────────────────────────
struct MPR121Settings {
    uint8_t tth;         // global touch threshold    (default 12)
    uint8_t rth;         // global release threshold  (default  6)
    uint8_t debounce;    // bits[6:4]=DR  bits[2:0]=DT  (default 0x00)
    uint8_t afe1;        // AFE config 1  (default 0xFF)
    uint8_t afe2;        // AFE config 2  (default 0x30 → CDT=1µs, SFI=10, ESI=1ms)
    uint8_t autoCfg0;    // 0x00 = disabled, 0x0B = auto-config on  (default 0x0B)
    uint8_t usl;         // (default 200)
    uint8_t lsl;         // (default 130)
    uint8_t tl;          // (default 180)
};

// ── Driver ────────────────────────────────────────────────────────────────────
class MPR121 {
public:
    explicit MPR121(uint8_t addr) : _addr(addr) {}

    bool    begin(const MPR121Settings& s);
    bool    isConnected();

    // Returns 12-bit mask, bit N = electrode N pressed
    uint16_t getTouchStatus();

    // 10-bit filtered ADC value for electrode 0-11
    uint16_t getFilteredData(uint8_t electrode);

    // Baseline MSB (actual baseline = value << 2)
    uint8_t  getBaseline(uint8_t electrode);

    // Set thresholds for a single electrode (stop mode not required)
    void setThreshold(uint8_t electrode, uint8_t touch, uint8_t release);
    void setAllThresholds(uint8_t touch, uint8_t release);

    // Re-apply full settings (stops then restarts the device)
    void applySettings(const MPR121Settings& s);

    // ── Per-electrode charge current / time (CDC/CDT) ──────────────────────────
    // CDC: 0-63 µA. CDT code: 0=off,1=0.5µs,2=1µs,3=2µs,4=4µs,5=8µs,6=16µs,7=32µs.
    uint8_t getElectrodeCDC(uint8_t electrode);
    uint8_t getElectrodeCDT(uint8_t electrode);
    void    setElectrodeCDC(uint8_t electrode, uint8_t cdc);   // call in stop mode
    void    setElectrodeCDT(uint8_t electrode, uint8_t cdt);   // call in stop mode

    // ── Hardware auto-configuration ────────────────────────────────────────────
    // Runs the chip's auto-config, which picks per-electrode CDC/CDT to bring each
    // electrode's charge reading to the target level. Blocks ~ms while it settles.
    void    runAutoConfig(uint8_t usl, uint8_t lsl, uint8_t tl);
    // Enable/disable auto-config without disturbing the per-electrode CDC/CDT
    // values already in the registers (used to "freeze" a calibration).
    void    setAutoConfigEnabled(bool enabled);
    // 12-bit mask of electrodes whose auto-config result was out of range (failed)
    uint16_t getOORStatus();

    void enterStop() { writeReg(MPR_ELEC_CFG, 0x00); }
    void enterRun()  { writeReg(MPR_ELEC_CFG, 0x8F); }

    uint8_t  readReg(uint8_t reg);
    void     writeReg(uint8_t reg, uint8_t val);

private:
    uint8_t _addr;
    void softReset();
};
