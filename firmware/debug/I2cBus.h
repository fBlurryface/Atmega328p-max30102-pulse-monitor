#pragma once
#include <Arduino.h>
#include <Wire.h>

// A small shared transport. Call begin() once before either I2C driver.
class I2cBus {
 public:
  explicit I2cBus(TwoWire &wire) : wire_(wire) {}
  void begin(uint32_t hz = 100000UL, uint32_t timeoutUs = 25000UL) {
    wire_.begin();
    wire_.setClock(hz);
    // Arduino AVR Boards 1.8.6 has this API but no WIRE_HAS_TIMEOUT macro.
    // Require that core version or newer; never silently disable bus timeouts.
    wire_.setWireTimeout(timeoutUs, true);
    wire_.clearWireTimeoutFlag();
    status_ = 0;
  }
  bool probe(uint8_t address) {
    wire_.beginTransmission(address);
    return finish();
  }
  bool writeRegister(uint8_t address, uint8_t reg, uint8_t value) {
    wire_.beginTransmission(address);
    wire_.write(reg);
    wire_.write(value);
    return finish();
  }
  // Maximum 30-byte payload plus a control byte: fits the AVR Wire TX buffer.
  bool writePacket(uint8_t address, uint8_t control, const uint8_t *data, uint8_t count) {
    if (!data || count > 30) { status_ = 1; return false; }
    wire_.beginTransmission(address);
    wire_.write(control);
    for (uint8_t i = 0; i < count; ++i) wire_.write(data[i]);
    return finish();
  }
  bool readRegisters(uint8_t address, uint8_t reg, uint8_t *data, uint8_t count) {
    if (!data || count == 0 || count > 32) { status_ = 1; return false; }
    wire_.beginTransmission(address);
    wire_.write(reg);
    status_ = wire_.endTransmission(false);
    if (status_) return false;
    const uint8_t got = wire_.requestFrom(address, count, uint8_t(true));
    if (got != count) {
      while (wire_.available()) wire_.read();
      status_ = 6; // Short read, distinct from Wire endTransmission status codes.
      return false;
    }
    for (uint8_t i = 0; i < count; ++i) data[i] = uint8_t(wire_.read());
    status_ = 0;
    return true;
  }
  uint8_t status() const { return status_; }
  bool timedOut() { return wire_.getWireTimeoutFlag(); }
  void clearTimeout() { wire_.clearWireTimeoutFlag(); }

 private:
  TwoWire &wire_;
  uint8_t status_ = 0;
  bool finish() { status_ = wire_.endTransmission(); return status_ == 0; }
};
