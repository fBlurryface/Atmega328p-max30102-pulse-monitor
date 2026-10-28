#pragma once
#include <Arduino.h>
#include "I2cBus.h"

// MAX30102 register/FIFO driver, red+IR only. No HR/SpO2 estimation here.
// Datasheet: https://www.analog.com/media/en/technical-documentation/data-sheets/MAX30102.pdf
class Max30102 {
 public:
  struct Sample { uint32_t red; uint32_t ir; };
  enum class Error : uint8_t { None, Bus, WrongPart, ResetTimeout, NotReady };
  explicit Max30102(I2cBus &bus, uint8_t address = 0x57)
      : bus_(bus), address_(address) {}

  bool identify(uint8_t &part, uint8_t &revision) {
    uint8_t ids[2];
    if (!read(0xFE, ids, 2)) return false;
    revision = ids[0]; part = ids[1];
    return true;
  }
  bool begin(uint8_t redCode = 0x1F, uint8_t irCode = 0x1F) {
    ready_ = false;
    uint8_t part, revision;
    if (!identify(part, revision)) return false;
    if (part != 0x15) { error_ = Error::WrongPart; return false; }
    // 0x15 is also used by related MAX3010x parts; verify module marking as well.
    if (!write(0x09, 0x40)) return false;
    const uint32_t started = millis();
    uint8_t mode;
    do {
      if (!read(0x09, &mode, 1)) return false;
      if (!(mode & 0x40)) break;
      if (uint32_t(millis() - started) >= 100) {
        error_ = Error::ResetTimeout; return false;
      }
      delay(1); // Bounded reset wait; not used in the acquisition path.
    } while (true);
    // Keep shutdown asserted until FIFO and all conversion settings are ready.
    if (!write(0x09, 0x83) || !write(0x02, 0) || !write(0x03, 0) ||
        !write(0x08, 0x00) || // No averaging, no rollover; A_FULL at 32 samples.
        !write(0x0A, 0x27) || // 4096 nA range, 100 sps, 411 us / 18 bits.
        !setLedCodes(redCode, irCode) || !clearFifo()) return false;
    uint8_t status[2];
    if (!read(0x00, status, 2) || !write(0x09, 0x03)) return false;
    ready_ = true;
    error_ = Error::None;
    lostMinimum_ = 0;
    return true;
  }
  bool stop() {
    ready_ = false;
    return write(0x09, 0x83); // Shutdown, preserve red+IR mode bits.
  }
  bool ready() const { return ready_; }
  Error error() const { return error_; }
  uint8_t lastBusStatus() const { return lastBusStatus_; }
  uint32_t lostMinimum() const { return lostMinimum_; }
  bool clearFifo() {
    return write(0x04, 0) && write(0x05, 0) && write(0x06, 0);
  }
  bool setLedCodes(uint8_t red, uint8_t ir) {
    return write(0x0C, red) && write(0x0D, ir);
  }
  // Returns 0..min(capacity,5), or -1 on error. Never waits for a new sample.
  int8_t readSamples(Sample *samples, uint8_t capacity) {
    if (!ready_) { error_ = Error::NotReady; return -1; }
    if (!samples || !capacity) return 0;
    uint8_t status, pointers[3];
    if (!read(0x00, &status, 1) || !read(0x04, pointers, 3)) return -1;
    const uint8_t overflow = pointers[1] & 0x1F;
    uint8_t available = (pointers[0] - pointers[2]) & 0x1F;
    // Equal pointers may mean a completely full FIFO, not just an empty one.
    if (!available && ((status & 0x80) || overflow)) available = 32;
    if (!available) { error_ = Error::None; return 0; }
    uint8_t count = available < capacity ? available : capacity;
    if (count > 5) count = 5; // 5 * 6 = 30 bytes, fits AVR Wire RX buffer.
    uint8_t data[30];
    if (!read(0x07, data, uint8_t(count * 6))) {
      // A partial FIFO transfer may advance RD_PTR. Restore before reporting.
      bus_.writeRegister(address_, 0x06, pointers[2] & 0x1F);
      error_ = Error::Bus;
      return -1;
    }
    for (uint8_t i = 0; i < count; ++i) {
      const uint8_t *p = data + i * 6;
      samples[i].red = ((uint32_t(p[0]) << 16) | (uint32_t(p[1]) << 8) | p[2]) & 0x3FFFFUL;
      samples[i].ir = ((uint32_t(p[3]) << 16) | (uint32_t(p[4]) << 8) | p[5]) & 0x3FFFFUL;
    }
    lostMinimum_ += overflow; // Hardware loss counter saturates at 31.
    error_ = Error::None;
    return int8_t(count);
  }
  bool readRegister(uint8_t reg, uint8_t &value) { return read(reg, &value, 1); }

 private:
  I2cBus &bus_;
  uint8_t address_;
  bool ready_ = false;
  Error error_ = Error::None;
  uint8_t lastBusStatus_ = 0;
  uint32_t lostMinimum_ = 0;
  bool write(uint8_t reg, uint8_t value) {
    if (!bus_.writeRegister(address_, reg, value)) {
      lastBusStatus_ = bus_.status(); error_ = Error::Bus; return false;
    }
    error_ = Error::None;
    return true;
  }
  bool read(uint8_t reg, uint8_t *data, uint8_t count) {
    if (!bus_.readRegisters(address_, reg, data, count)) {
      lastBusStatus_ = bus_.status(); error_ = Error::Bus; return false;
    }
    error_ = Error::None;
    return true;
  }
};
