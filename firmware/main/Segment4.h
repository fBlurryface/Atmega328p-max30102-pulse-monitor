#pragma once
#include <Arduino.h>
#include <avr/interrupt.h>
#include <util/atomic.h>

// Common-cathode, directly driven: segments HIGH=on, digit LOW=selected.
// Owns Timer2. Do not use tone(), Timer2 libraries, or analogWrite on D3/D11.
// Define SEGMENT4_TIMER2_ISR(instance) exactly once in the main sketch.
class Segment4 {
 public:
  Segment4(const uint8_t (&segments)[7], const uint8_t (&digits)[4]) {
    for (uint8_t i = 0; i < 7; ++i) segments_[i] = segments[i];
    for (uint8_t i = 0; i < 4; ++i) digits_[i] = digits[i];
  }
  void begin() {
    for (uint8_t i = 0; i < 4; ++i) {
      digitalWrite(digits_[i], HIGH); // Inactive before changing direction.
      pinMode(digits_[i], OUTPUT);
    }
    for (uint8_t i = 0; i < 7; ++i) {
      digitalWrite(segments_[i], LOW);
      pinMode(segments_[i], OUTPUT);
    }
    clear();
    ATOMIC_BLOCK(ATOMIC_RESTORESTATE) {
      enabled_ = true;
      TCCR2A = _BV(WGM21);
      TCCR2B = 0;
      TCNT2 = 0;
      OCR2A = uint8_t(F_CPU / 64UL / 1000UL - 1UL);
      TIFR2 = _BV(OCF2A);
      TIMSK2 = _BV(OCIE2A);
      TCCR2B = _BV(CS22); // /64; 1 kHz slots, 250 Hz per digit at 16 MHz.
    }
  }
  void end() {
    ATOMIC_BLOCK(ATOMIC_RESTORESTATE) {
      TIMSK2 = 0;
      TCCR2B = 0;
      enabled_ = false;
      allDigitsOff();
      for (uint8_t i = 0; i < 7; ++i) digitalWrite(segments_[i], LOW);
    }
  }
  void enable(bool enabled) {
    ATOMIC_BLOCK(ATOMIC_RESTORESTATE) {
      enabled_ = enabled;
      if (!enabled) allDigitsOff();
    }
  }
  bool enabled() const { return enabled_; }
  void clear() { const uint8_t blank[4] = {0, 0, 0, 0}; showRaw(blank); }
  void showRaw(const uint8_t (&masks)[4]) {
    ATOMIC_BLOCK(ATOMIC_RESTORESTATE) {
      for (uint8_t i = 0; i < 4; ++i) masks_[i] = masks[i] & 0x7F;
    }
  }
  bool showNumber(int16_t value) {
    if (value < -999 || value > 9999) return false;
    uint8_t data[4] = {0, 0, 0, 0};
    uint16_t magnitude = value < 0 ? uint16_t(-value) : uint16_t(value);
    int8_t pos = 3;
    do {
      data[pos--] = digitMask(magnitude % 10);
      magnitude /= 10;
    } while (magnitude && pos >= 0);
    if (value < 0) data[pos] = 0x40;
    showRaw(data);
    return true;
  }
  // Digit=1..4; bit0=a ... bit6=g. No decimal-point wire exists in the schematic.
  bool showDigitMask(uint8_t digit, uint8_t mask) {
    if (digit < 1 || digit > 4 || mask > 0x7F) return false;
    uint8_t data[4] = {0, 0, 0, 0};
    data[digit - 1] = mask;
    showRaw(data);
    return true;
  }
  // Called from the Timer2 ISR only. No Serial, Wire, or millis() here.
  void onTimer() {
    allDigitsOff(); // Break before make to suppress ghosting.
    const uint8_t mask = enabled_ ? masks_[scan_] : 0;
    for (uint8_t i = 0; i < 7; ++i)
      digitalWrite(segments_[i], (mask & (1U << i)) ? HIGH : LOW);
    if (enabled_ && mask) digitalWrite(digits_[scan_], LOW);
    scan_ = (scan_ + 1) & 3;
  }
  static uint8_t digitMask(uint8_t value) {
    switch (value) {
      case 0: return 0x3F; case 1: return 0x06; case 2: return 0x5B;
      case 3: return 0x4F; case 4: return 0x66; case 5: return 0x6D;
      case 6: return 0x7D; case 7: return 0x07; case 8: return 0x7F;
      case 9: return 0x6F; default: return 0;
    }
  }

 private:
  uint8_t segments_[7], digits_[4];
  volatile uint8_t masks_[4] = {0, 0, 0, 0};
  volatile bool enabled_ = false;
  uint8_t scan_ = 0;
  void allDigitsOff() {
    for (uint8_t i = 0; i < 4; ++i) digitalWrite(digits_[i], HIGH);
  }
};

#define SEGMENT4_TIMER2_ISR(instance) \
  ISR(TIMER2_COMPA_vect) { (instance).onTimer(); }
