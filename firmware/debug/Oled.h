#pragma once
#include <Arduino.h>
#include <avr/pgmspace.h>
#include "I2cBus.h"

/* Font data license (also distributed in THIRD_PARTY_LICENSES.txt):
Software License Agreement (BSD License)

Copyright (c) 2012 Adafruit Industries.  All rights reserved.

Redistribution and use in source and binary forms, with or without
modification, are permitted provided that the following conditions are met:

- Redistributions of source code must retain the above copyright notice,
  this list of conditions and the following disclaimer.
- Redistributions in binary form must reproduce the above copyright notice,
  this list of conditions and the following disclaimer in the documentation
  and/or other materials provided with the distribution.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE
LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
POSSIBILITY OF SUCH DAMAGE.
*/

// SSD1306 128x32 / four-pin I2C. A set pixel emits the panel's physical color.
// No frame buffer, heap, reset GPIO, or timer. Call update() from loop().
// A job writes at most 16 data bytes per update, leaving time for other devices.
class Oled {
 public:
  enum class State : uint8_t { NotStarted, Initializing, Ready, AddressMissing, Fault };
  enum class Error : uint8_t { None, Bus, NotReady, Busy, Arguments };
  enum class Pattern : uint8_t { Layout, Border, Checker };
  explicit Oled(I2cBus &bus, uint8_t address = 0x3C) : bus_(bus), address_(address) {}

  bool probe(uint8_t address) {
    if (busy()) return reject(Error::Busy);
    if (!validAddress(address)) return reject(Error::Arguments);
    lastProbeAddress_ = address;
    lastProbeAck_ = bus_.probe(address);
    if (!lastProbeAck_) return reject(Error::Bus);
    error_ = Error::None;
    return true; // Does not change the configured drawing address or identify the IC.
  }
  bool begin(uint8_t address) {
    if (busy()) return reject(Error::Busy);
    if (!validAddress(address)) return reject(Error::Arguments);
    const uint8_t previousAddress = address_;
    const State previousState = state_;
    address_ = address;
    const bool accepted = begin();
    if (!accepted) {
      address_ = previousAddress;
      // A missing candidate did not change the active display's configuration.
      // Failures at the active address, or after writes started, remain faults.
      if (address != previousAddress && state_ == State::AddressMissing)
        state_ = previousState;
    }
    return accepted;
  }
  bool begin() {
    if (busy()) return reject(Error::Busy);
    if (!validAddress(address_)) return reject(Error::Arguments);
    if (!probe(address_)) { state_ = State::AddressMissing; return false; }
    // Explicitly re-establish the profile even if a USB reset did not reset OLED.
    // COM pins=0x02: sequential 32-row panel, with reserved bit1 set per SSD1306.
    const uint8_t init[] = {
      0xAE, 0xD5, 0x80, 0xA8, 0x1F, 0xD3, 0x00, 0x40,
      0x8D, 0x14, 0x20, 0x02, 0xA1, 0xC8, 0xDA, 0x02,
      0x81, 0x7F, 0xD9, 0xF1, 0xDB, 0x40, 0xA4, 0xA6, 0x2E
    };
    if (!commands(init, sizeof(init))) return false;
    inverted_ = flipped_ = displayOn_ = false;
    state_ = State::Initializing;
    job_ = Job::Initialize;
    offset_ = 0; length_ = 512;
    started_ = millis();
    error_ = Error::None;
    return true; // Accepted; update() finishes clear and display-on asynchronously.
  }
  bool clear() { return fill(false); }
  bool fill(bool white) {
    if (!start(Job::Fill)) return false;
    fillByte_ = white ? 0xFF : 0;
    return true;
  }
  bool test(Pattern pattern = Pattern::Layout) {
    if (pattern != Pattern::Layout && pattern != Pattern::Border && pattern != Pattern::Checker)
      return reject(Error::Arguments);
    if (!start(Job::Pattern)) return false;
    pattern_ = pattern;
    return true;
  }
  // 6x8 cells: column 0..20, row 0..3. ASCII 32..126, max 21-column chars.
  // Copies the text immediately, then rewrites through the right edge of that row.
  bool writeText(uint8_t column, uint8_t row, const char *text) {
    if (busy()) return reject(Error::Busy);
    if (!ready()) return reject(Error::NotReady);
    if (!text || column > 20 || row > 3) return reject(Error::Arguments);
    uint8_t n = 0;
    while (text[n]) {
      const uint8_t c = uint8_t(text[n]);
      if (n >= 21 - column || c < 32 || c > 126) return reject(Error::Arguments);
      ++n;
    }
    for (uint8_t i = 0; i < n; ++i) text_[i] = text[i];
    text_[n] = '\0'; textLength_ = n;
    textX_ = column * 6; textRow_ = row;
    if (!start(Job::Text)) return false;
    length_ = 128 - textX_;
    return true;
  }
  bool setInverted(bool value) {
    if (!idleReady()) return false;
    const uint8_t cmd = value ? 0xA7 : 0xA6;
    if (!commands(&cmd, 1)) return false;
    inverted_ = value; return true;
  }
  bool setFlipped(bool value) {
    if (!idleReady()) return false;
    const uint8_t cmd[2] = {uint8_t(value ? 0xA0 : 0xA1), uint8_t(value ? 0xC0 : 0xC8)};
    if (!commands(cmd, 2)) return false;
    flipped_ = value; return true;
  }
  bool setDisplayOn(bool value) {
    if (!idleReady()) return false;
    const uint8_t cmd = value ? 0xAF : 0xAE;
    if (!commands(&cmd, 1)) return false;
    displayOn_ = value; return true;
  }
  bool setContrast(uint8_t value) {
    if (!idleReady()) return false;
    const uint8_t cmd[2] = {0x81, value};
    return commands(cmd, 2);
  }
  void update() {
    if (!busy()) return;
    if (job_ == Job::Initialize && uint32_t(millis() - started_) < 100) return;
    const uint8_t count = uint8_t(length_ - offset_ > 16 ? 16 : length_ - offset_);
    const uint8_t page = job_ == Job::Text ? textRow_ : offset_ / 128;
    const uint8_t x = job_ == Job::Text ? textX_ + offset_ : offset_ % 128;
    const uint8_t position[3] = {uint8_t(0xB0 | page), uint8_t(x & 0x0F), uint8_t(0x10 | (x >> 4))};
    uint8_t data[16];
    for (uint8_t i = 0; i < count; ++i) data[i] = render(offset_ + i);
    if (!commands(position, 3)) return;
    if (!bus_.writePacket(address_, 0x40, data, count)) { fail(); return; }
    offset_ += count;
    if (offset_ == length_) {
      if (job_ == Job::Initialize) {
        const uint8_t on = 0xAF;
        if (!commands(&on, 1)) return;
        state_ = State::Ready; displayOn_ = true;
      }
      job_ = Job::None;
      error_ = Error::None;
    }
  }
  bool ready() const { return state_ == State::Ready; }
  bool busy() const { return job_ != Job::None; }
  State state() const { return state_; }
  Error error() const { return error_; }
  uint8_t address() const { return address_; }
  uint8_t lastProbeAddress() const { return lastProbeAddress_; }
  bool lastProbeAck() const { return lastProbeAck_; }
  bool inverted() const { return inverted_; }
  bool flipped() const { return flipped_; }
  bool displayOn() const { return displayOn_; }

 private:
  enum class Job : uint8_t { None, Initialize, Fill, Pattern, Text };
  I2cBus &bus_;
  uint8_t address_, lastProbeAddress_ = 0;
  State state_ = State::NotStarted;
  Error error_ = Error::None;
  Job job_ = Job::None;
  Pattern pattern_ = Pattern::Layout;
  bool lastProbeAck_ = false, inverted_ = false, flipped_ = false, displayOn_ = false;
  uint16_t offset_ = 0, length_ = 0;
  uint32_t started_ = 0;
  uint8_t fillByte_ = 0, textX_ = 0, textRow_ = 0, textLength_ = 0;
  char text_[22] = {};
  static bool validAddress(uint8_t address) { return address == 0x3C || address == 0x3D; }
  bool reject(Error error) { error_ = error; return false; }
  void fail() { error_ = Error::Bus; state_ = State::Fault; job_ = Job::None; }
  bool commands(const uint8_t *data, uint8_t count) {
    if (!bus_.writePacket(address_, 0x00, data, count)) { fail(); return false; }
    error_ = Error::None; return true;
  }
  bool idleReady() {
    if (busy()) return reject(Error::Busy);
    if (!ready()) return reject(Error::NotReady);
    return true;
  }
  bool start(Job job) {
    if (!idleReady()) return false;
    job_ = job; offset_ = 0; length_ = 512; error_ = Error::None;
    return true;
  }
  static uint8_t glyph(char c, uint8_t column) {
    // ASCII subset of Adafruit GFX glcdfont.c; BSD notice in THIRD_PARTY_LICENSES.txt.
    // Copyright (c) 2012 Adafruit Industries. All rights reserved.
    static const uint8_t font[475] PROGMEM = {
      0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x5F, 0x00, 0x00, 0x00, 0x07, 0x00, 0x07, 0x00,
      0x14, 0x7F, 0x14, 0x7F, 0x14, 0x24, 0x2A, 0x7F, 0x2A, 0x12, 0x23, 0x13, 0x08, 0x64, 0x62,
      0x36, 0x49, 0x56, 0x20, 0x50, 0x00, 0x08, 0x07, 0x03, 0x00, 0x00, 0x1C, 0x22, 0x41, 0x00,
      0x00, 0x41, 0x22, 0x1C, 0x00, 0x2A, 0x1C, 0x7F, 0x1C, 0x2A, 0x08, 0x08, 0x3E, 0x08, 0x08,
      0x00, 0x80, 0x70, 0x30, 0x00, 0x08, 0x08, 0x08, 0x08, 0x08, 0x00, 0x00, 0x60, 0x60, 0x00,
      0x20, 0x10, 0x08, 0x04, 0x02, 0x3E, 0x51, 0x49, 0x45, 0x3E, 0x00, 0x42, 0x7F, 0x40, 0x00,
      0x72, 0x49, 0x49, 0x49, 0x46, 0x21, 0x41, 0x49, 0x4D, 0x33, 0x18, 0x14, 0x12, 0x7F, 0x10,
      0x27, 0x45, 0x45, 0x45, 0x39, 0x3C, 0x4A, 0x49, 0x49, 0x31, 0x41, 0x21, 0x11, 0x09, 0x07,
      0x36, 0x49, 0x49, 0x49, 0x36, 0x46, 0x49, 0x49, 0x29, 0x1E, 0x00, 0x00, 0x14, 0x00, 0x00,
      0x00, 0x40, 0x34, 0x00, 0x00, 0x00, 0x08, 0x14, 0x22, 0x41, 0x14, 0x14, 0x14, 0x14, 0x14,
      0x00, 0x41, 0x22, 0x14, 0x08, 0x02, 0x01, 0x59, 0x09, 0x06, 0x3E, 0x41, 0x5D, 0x59, 0x4E,
      0x7C, 0x12, 0x11, 0x12, 0x7C, 0x7F, 0x49, 0x49, 0x49, 0x36, 0x3E, 0x41, 0x41, 0x41, 0x22,
      0x7F, 0x41, 0x41, 0x41, 0x3E, 0x7F, 0x49, 0x49, 0x49, 0x41, 0x7F, 0x09, 0x09, 0x09, 0x01,
      0x3E, 0x41, 0x41, 0x51, 0x73, 0x7F, 0x08, 0x08, 0x08, 0x7F, 0x00, 0x41, 0x7F, 0x41, 0x00,
      0x20, 0x40, 0x41, 0x3F, 0x01, 0x7F, 0x08, 0x14, 0x22, 0x41, 0x7F, 0x40, 0x40, 0x40, 0x40,
      0x7F, 0x02, 0x1C, 0x02, 0x7F, 0x7F, 0x04, 0x08, 0x10, 0x7F, 0x3E, 0x41, 0x41, 0x41, 0x3E,
      0x7F, 0x09, 0x09, 0x09, 0x06, 0x3E, 0x41, 0x51, 0x21, 0x5E, 0x7F, 0x09, 0x19, 0x29, 0x46,
      0x26, 0x49, 0x49, 0x49, 0x32, 0x03, 0x01, 0x7F, 0x01, 0x03, 0x3F, 0x40, 0x40, 0x40, 0x3F,
      0x1F, 0x20, 0x40, 0x20, 0x1F, 0x3F, 0x40, 0x38, 0x40, 0x3F, 0x63, 0x14, 0x08, 0x14, 0x63,
      0x03, 0x04, 0x78, 0x04, 0x03, 0x61, 0x59, 0x49, 0x4D, 0x43, 0x00, 0x7F, 0x41, 0x41, 0x41,
      0x02, 0x04, 0x08, 0x10, 0x20, 0x00, 0x41, 0x41, 0x41, 0x7F, 0x04, 0x02, 0x01, 0x02, 0x04,
      0x40, 0x40, 0x40, 0x40, 0x40, 0x00, 0x03, 0x07, 0x08, 0x00, 0x20, 0x54, 0x54, 0x78, 0x40,
      0x7F, 0x28, 0x44, 0x44, 0x38, 0x38, 0x44, 0x44, 0x44, 0x28, 0x38, 0x44, 0x44, 0x28, 0x7F,
      0x38, 0x54, 0x54, 0x54, 0x18, 0x00, 0x08, 0x7E, 0x09, 0x02, 0x18, 0xA4, 0xA4, 0x9C, 0x78,
      0x7F, 0x08, 0x04, 0x04, 0x78, 0x00, 0x44, 0x7D, 0x40, 0x00, 0x20, 0x40, 0x40, 0x3D, 0x00,
      0x7F, 0x10, 0x28, 0x44, 0x00, 0x00, 0x41, 0x7F, 0x40, 0x00, 0x7C, 0x04, 0x78, 0x04, 0x78,
      0x7C, 0x08, 0x04, 0x04, 0x78, 0x38, 0x44, 0x44, 0x44, 0x38, 0xFC, 0x18, 0x24, 0x24, 0x18,
      0x18, 0x24, 0x24, 0x18, 0xFC, 0x7C, 0x08, 0x04, 0x04, 0x08, 0x48, 0x54, 0x54, 0x54, 0x24,
      0x04, 0x04, 0x3F, 0x44, 0x24, 0x3C, 0x40, 0x40, 0x20, 0x7C, 0x1C, 0x20, 0x40, 0x20, 0x1C,
      0x3C, 0x40, 0x30, 0x40, 0x3C, 0x44, 0x28, 0x10, 0x28, 0x44, 0x4C, 0x90, 0x90, 0x90, 0x7C,
      0x44, 0x64, 0x54, 0x4C, 0x44, 0x00, 0x08, 0x36, 0x41, 0x00, 0x00, 0x00, 0x77, 0x00, 0x00,
      0x00, 0x41, 0x36, 0x08, 0x00, 0x02, 0x01, 0x02, 0x04, 0x02,
    };
    if (column >= 5 || uint8_t(c) < 32 || uint8_t(c) > 126) return 0;
    return pgm_read_byte(font + (uint8_t(c) - 32) * 5 + column);
  }
  uint8_t label(uint8_t x, uint8_t origin, const char *flashText, uint8_t len,
                bool addressText = false) const {
    if (x < origin || x - origin >= len * 6) return 0;
    const uint8_t offset = x - origin;
    char c = char(pgm_read_byte(flashText + offset / 6));
    if (addressText && offset / 6 == 3 && address_ == 0x3D) c = 'D';
    return glyph(c, offset % 6);
  }
  uint8_t render(uint16_t offset) const {
    if (job_ == Job::Initialize) return 0;
    if (job_ == Job::Fill) return fillByte_;
    if (job_ == Job::Text) {
      const uint8_t index = offset / 6;
      return index < textLength_ ? glyph(text_[index], offset % 6) : 0;
    }
    const uint8_t page = offset / 128, x = offset % 128;
    if (pattern_ == Pattern::Checker) return ((x / 8 + page) & 1) ? 0xFF : 0;
    uint8_t data = x == 0 || x == 127 ? 0xFF : (page == 0 ? 0x01 : (page == 3 ? 0x80 : 0));
    if (pattern_ == Pattern::Border) return data;
    if (page == 0) {
      data |= uint8_t(label(x, 3, PSTR("TL"), 2) << 1);
      data |= uint8_t(label(x, 113, PSTR("TR"), 2) << 1);
    } else if (page == 3) {
      data |= label(x, 3, PSTR("BL"), 2);
      data |= label(x, 113, PSTR("BR"), 2);
    } else if (page == 1) data |= label(x, 22, PSTR("SSD1306 128x32"), 14);
    else data |= label(x, 19, PSTR("0x3C WHITE OLED"), 15, true);
    return data;
  }
};
