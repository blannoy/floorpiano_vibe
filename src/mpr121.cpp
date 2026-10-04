#include "mpr121.h"

bool MPR121::begin(const MPR121Settings& s) {
    softReset();
    delay(10);
    if (!isConnected()) return false;
    applySettings(s);
    return true;
}

bool MPR121::isConnected() {
    Wire.beginTransmission(_addr);
    return Wire.endTransmission() == 0;
}

uint16_t MPR121::getTouchStatus() {
    uint8_t lo = readReg(MPR_TOUCH_STATUS);
    uint8_t hi = readReg(MPR_TOUCH_STATUS + 1);
    return ((uint16_t)(hi & 0x1F) << 8) | lo;  // bits 0-11 = electrodes
}

uint16_t MPR121::getFilteredData(uint8_t electrode) {
    if (electrode > 11) return 0;
    uint8_t reg = MPR_FILT_DATA + (electrode * 2);
    uint8_t lo = readReg(reg);
    uint8_t hi = readReg(reg + 1);
    return ((uint16_t)(hi & 0x03) << 8) | lo;
}

uint8_t MPR121::getBaseline(uint8_t electrode) {
    if (electrode > 11) return 0;
    return readReg(MPR_BASELINE + electrode);
}

void MPR121::setThreshold(uint8_t electrode, uint8_t touch, uint8_t release) {
    uint8_t reg = MPR_ELE0_TTH + (electrode * 2);
    writeReg(reg,     touch);
    writeReg(reg + 1, release);
}

void MPR121::setAllThresholds(uint8_t touch, uint8_t release) {
    for (uint8_t i = 0; i < 12; i++) setThreshold(i, touch, release);
}

void MPR121::applySettings(const MPR121Settings& s) {
    writeReg(MPR_ELEC_CFG, 0x00);  // stop mode

    // Baseline filter – rising (baseline catching back up after a release).
    // Larger MHD/NHD let the baseline jump back ~32/16 counts per cycle instead of
    // ~1, so the post-release residual clears in a few ms instead of lingering at
    // 10-20 for >100ms (which can keep a key "on" against a low release threshold).
    writeReg(MPR_MHD_RISING, 0x3F);
    writeReg(MPR_NHD_RISING, 0x10);
    writeReg(MPR_NCL_RISING, 0x00);
    writeReg(MPR_FDL_RISING, 0x00);

    // Baseline filter – falling (when electrode is touched)
    writeReg(MPR_MHD_FALLING, 0x01);
    writeReg(MPR_NHD_FALLING, 0x01);
    writeReg(MPR_NCL_FALLING, 0xFF);
    writeReg(MPR_FDL_FALLING, 0x02);

    setAllThresholds(s.tth, s.rth);

    writeReg(MPR_DEBOUNCE, s.debounce);
    writeReg(MPR_AFE1,     s.afe1);
    writeReg(MPR_AFE2,     s.afe2);

    // Always write the auto-config registers — including AUTO_CFG0 = 0 to
    // *disable* it. Otherwise turning auto-config off via a live re-apply leaves
    // the old (enabled) value in 0x7B, so the chip keeps overriding the manual
    // CDC/CDT in AFE1/AFE2 until the next reboot clears the register.
    writeReg(MPR_USL,       s.usl);
    writeReg(MPR_LSL,       s.lsl);
    writeReg(MPR_TL,        s.tl);
    writeReg(MPR_AUTO_CFG1, 0x00);
    writeReg(MPR_AUTO_CFG0, s.autoCfg0);  // 0x00 = off; must be the last AC write

    // Run mode: CL=10 (load baseline from register then track), all 12 electrodes
    writeReg(MPR_ELEC_CFG, 0x8F);
}

// ── Per-electrode charge current / time ────────────────────────────────────────
uint8_t MPR121::getElectrodeCDC(uint8_t e) {
    if (e > 11) return 0;
    return readReg(MPR_ELE0_CDC + e) & 0x3F;
}

uint8_t MPR121::getElectrodeCDT(uint8_t e) {
    if (e > 11) return 0;
    uint8_t reg = readReg(MPR_ELE0_CDT + (e / 2));   // packed 2 electrodes per byte
    return (e & 1) ? ((reg >> 4) & 0x07) : (reg & 0x07);
}

void MPR121::setElectrodeCDC(uint8_t e, uint8_t cdc) {
    if (e > 11) return;
    writeReg(MPR_ELE0_CDC + e, cdc & 0x3F);
}

void MPR121::setElectrodeCDT(uint8_t e, uint8_t cdt) {
    if (e > 11) return;
    uint8_t reg = MPR_ELE0_CDT + (e / 2);
    uint8_t v   = readReg(reg);                       // read-modify-write the pair
    if (e & 1) v = (v & 0x0F) | ((cdt & 0x07) << 4);
    else       v = (v & 0xF8) | (cdt & 0x07);
    writeReg(reg, v);
}

// ── Hardware auto-configuration ─────────────────────────────────────────────────
void MPR121::runAutoConfig(uint8_t usl, uint8_t lsl, uint8_t tl) {
    enterStop();
    writeReg(MPR_USL, usl);
    writeReg(MPR_LSL, lsl);
    writeReg(MPR_TL,  tl);
    writeReg(MPR_AUTO_CFG1, 0x00);
    // 0x0B = enable auto-config + auto-reconfig + baseline adjust, retries off.
    // The FFI bits[7:6] must match AFE1 or the search uses the wrong sample count.
    uint8_t ffi = readReg(MPR_AFE1) & 0xC0;
    writeReg(MPR_AUTO_CFG0, ffi | 0x0B);
    enterRun();             // auto-config executes on the first sample after run
    delay(50);              // let it settle (well over one ESI sample cycle)
}

void MPR121::setAutoConfigEnabled(bool enabled) {
    enterStop();
    uint8_t ffi = readReg(MPR_AFE1) & 0xC0;
    writeReg(MPR_AUTO_CFG0, enabled ? (ffi | 0x0B) : 0x00);
    enterRun();             // per-electrode CDC/CDT registers are left untouched
}

uint16_t MPR121::getOORStatus() {
    uint8_t lo = readReg(MPR_OOR_STATUS_0);
    uint8_t hi = readReg(MPR_OOR_STATUS_1);
    return ((uint16_t)(hi & 0x0F) << 8) | lo;
}

void MPR121::softReset() {
    writeReg(MPR_SOFT_RESET, 0x63);
    delay(1);
}

uint8_t MPR121::readReg(uint8_t reg) {
    Wire.beginTransmission(_addr);
    Wire.write(reg);
    Wire.endTransmission(false);
    Wire.requestFrom(_addr, (uint8_t)1);
    return Wire.available() ? Wire.read() : 0;
}

void MPR121::writeReg(uint8_t reg, uint8_t val) {
    Wire.beginTransmission(_addr);
    Wire.write(reg);
    Wire.write(val);
    Wire.endTransmission();
}
