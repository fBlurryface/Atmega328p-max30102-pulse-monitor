#pragma once
#include <Arduino.h>

// Verified against Sheet1.SchDoc (TQFP-32 ATmega328P).
// This package uses the standard Arduino Uno/Nano ATmega328P pin mapping.
#if !defined(__AVR_ATmega328P__)
#error "Select an ATmega328P Arduino AVR board (Uno/Nano), not Uno R4 or Nano Every."
#endif
#if F_CPU != 16000000UL
#error "This board has a 16 MHz crystal; select a 16 MHz target."
#endif

namespace Board {
constexpr uint32_t SERIAL_BAUD = 115200UL;
constexpr uint32_t I2C_HZ = 100000UL;
constexpr uint32_t I2C_TIMEOUT_US = 25000UL;
constexpr uint8_t KEY_PINS[3] = {A1, A2, A3}; // S2=PC1, S3=PC2, S4=PC3.
constexpr uint8_t SEGMENT_PINS[7] = {2, 3, 4, 5, 6, 7, 8}; // a..g.
constexpr uint8_t DIGIT_PINS[4] = {10, 11, 12, 13}; // DIG1..DIG4.
constexpr uint8_t MAX30102_ADDRESS = 0x57; // 7-bit address, not 0xAE.
constexpr uint16_t KEY_DEBOUNCE_MS = 25;
constexpr uint16_t KEY_LONG_MS = 800;
// User verified the 0.91-inch SSD1306 128x32 white OLED at 0x3C on hardware.
constexpr uint8_t OLED_ADDRESS = 0x3C;
// Default finger-presence thresholds in raw ADC counts.
constexpr uint32_t FINGER_ON_IR = 5000;
constexpr uint32_t FINGER_OFF_IR = 3500;
constexpr uint32_t MINIMUM_RED = 1000;
constexpr uint16_t RAW_REFRESH_MS = 200;
constexpr uint16_t STATS_REFRESH_MS = 1000;
constexpr uint16_t HEART_FLASH_MS = 100;
constexpr uint8_t RED_LED_CODE = 31;
constexpr uint8_t IR_LED_CODE = 31;
}
