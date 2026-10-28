/*
 * Board application: optical measurement and user interface.
 * ATmega328P / 16 MHz; SSD1306 128x32 at 0x3C; MAX30102 at 0x57.
 * S2 restarts measurement, S3 selects raw mode, S4 selects statistics.
 * Boot mode: statistics. SpO2 is an uncalibrated engineering estimate.
 */
#include <Arduino.h>
#include <Wire.h>
#include <avr/pgmspace.h>
#include <stdio.h>
#include "BoardConfig.h"
#include "Buttons.h"
#include "Segment4.h"
#include "I2cBus.h"
#include "Max30102.h"
#include "Oled.h"
#include "Measurement.h"

enum class Mode : uint8_t { Raw, Stats };
enum class Phase : uint8_t { Waiting, Collecting, Measuring, Fault };
enum Fault : uint8_t { SensorFault = 1, DisplayFault = 2, NoSamples = 4 };

Buttons buttons(Board::KEY_PINS, Board::KEY_DEBOUNCE_MS, Board::KEY_LONG_MS);
Segment4 segment4(Board::SEGMENT_PINS, Board::DIGIT_PINS);
I2cBus bus(Wire);
Max30102 sensor(bus, Board::MAX30102_ADDRESS);
Oled oled(bus, Board::OLED_ADDRESS);
Measurement measurement(Board::FINGER_ON_IR, Board::FINGER_OFF_IR, Board::MINIMUM_RED);
SEGMENT4_TIMER2_ISR(segment4)

Mode mode = Mode::Stats;
Phase phase = Phase::Waiting;
uint8_t faultMask = 0;
uint32_t lastPoll = 0, lastSampleAt = 0, lastUiAt = 0, flashStarted = 0;
bool flashActive = false, uiUrgent = true, uiActive = false;
uint8_t uiRow = 0;
int16_t segmentValue = -32768;

struct UiFrame {
  Mode mode;
  Phase phase;
  Measurement::Quality quality;
  uint32_t red, ir;
  uint16_t bpm, spo2;
  bool hasSample, hrValid, spo2Valid;
  uint8_t faults;
} frame;

const char *qualityText(Measurement::Quality value) {
  switch (value) {
    case Measurement::Quality::NoFinger: return PSTR("WAIT FINGER");
    case Measurement::Quality::Warming: return PSTR("COLLECTING SIGNAL");
    case Measurement::Quality::Good: return PSTR("SIGNAL: OK");
    case Measurement::Quality::LowPulse: return PSTR("LOW SIGNAL");
    case Measurement::Quality::Unstable: return PSTR("IR IRREGULAR");
    case Measurement::Quality::HighVariation: return PSTR("SIGNAL TOO VARIABLE");
    case Measurement::Quality::Clipped: return PSTR("SIGNAL SATURATED");
    case Measurement::Quality::Uncorrelated: return PSTR("RED/IR MISMATCH");
    case Measurement::Quality::Range: return PSTR("ESTIMATE INVALID");
    default: return PSTR("NO PULSE YET");
  }
}

void copyFlash(char *destination, const char *source) {
  uint8_t i = 0;
  for (; i < 21; ++i) {
    const char c = char(pgm_read_byte(source + i));
    if (!c) break;
    destination[i] = c;
  }
  destination[i] = '\0';
}

void requestUi() { uiUrgent = true; }

void setMode(Mode next) {
  if (mode == next) return;
  mode = next;
  flashActive = false;
  requestUi();
}

void restartMeasurement() {
  measurement.reset();
  flashActive = false;
  faultMask = 0;
  bus.clearTimeout();
  if (!sensor.begin(Board::RED_LED_CODE, Board::IR_LED_CODE)) faultMask |= SensorFault;
  if (!oled.ready() && !oled.busy() && !oled.begin(Board::OLED_ADDRESS)) faultMask |= DisplayFault;
  lastPoll = lastSampleAt = millis();
  phase = faultMask ? Phase::Fault : Phase::Waiting;
  requestUi();
}

void serviceKeys() {
  buttons.update();
  Buttons::Event event;
  while (buttons.read(event)) {
    if (event.type != Buttons::Type::Press) continue;
    switch (event.key) {
      case 0: restartMeasurement(); break;
      case 1: setMode(Mode::Raw); break;
      case 2: setMode(Mode::Stats); break;
    }
  }
}

void serviceSensor() {
  if (faultMask & (SensorFault | NoSamples)) return;
  const uint32_t now = millis();
  if (uint32_t(now - lastPoll) < 10) return;
  lastPoll = now;

  Max30102::Sample samples[5];
  const uint32_t previousLost = sensor.lostMinimum();
  const int8_t count = sensor.readSamples(samples, 5);
  if (count < 0) {
    faultMask |= SensorFault;
    measurement.reset();
    flashActive = false;
    sensor.stop();
    requestUi();
    return;
  }
  if (sensor.lostMinimum() != previousLost) {
    // Missing samples break the algorithm's time base; rebuild the history.
    restartMeasurement();
    return;
  }
  if (count) {
    lastSampleAt = now;
    for (int8_t i = 0; i < count; ++i) measurement.push(samples[i].red, samples[i].ir);
  } else if (uint32_t(now - lastSampleAt) >= 1000) {
    faultMask |= NoSamples;
    measurement.reset();
    flashActive = false;
    sensor.stop();
    requestUi();
  }
  measurement.update();
}

void updateState() {
  const Phase previous = phase;
  if (faultMask) phase = Phase::Fault;
  else switch (phase) {
    case Phase::Waiting:
      if (measurement.finger()) phase = Phase::Collecting;
      break;
    case Phase::Collecting:
      if (!measurement.finger()) phase = Phase::Waiting;
      else if (measurement.hrValid() || measurement.spo2Valid()) phase = Phase::Measuring;
      break;
    case Phase::Measuring:
      if (!measurement.finger()) phase = Phase::Waiting;
      else if (!measurement.hrValid() && !measurement.spo2Valid()) phase = Phase::Collecting;
      break;
    case Phase::Fault:
      phase = measurement.finger() ? Phase::Collecting : Phase::Waiting;
      break;
  }
  if (phase != previous) requestUi();
}

void showSegmentValue(int16_t value) {
  if (segmentValue == value) return;
  segmentValue = value;
  if (value >= 0) segment4.showNumber(value);
  else {
    const uint8_t dashes[4] = {0x40, 0x40, 0x40, 0x40};
    segment4.showRaw(dashes);
  }
}

void serviceSegments() {
  const uint32_t now = millis();
  const bool beat = measurement.takeBeat();
  if (beat && mode == Mode::Raw && phase != Phase::Fault) {
    flashActive = true;
    flashStarted = now;
  }
  if (!measurement.finger() || phase == Phase::Fault ||
      (flashActive && uint32_t(now - flashStarted) >= Board::HEART_FLASH_MS)) flashActive = false;

  bool enabled = true;
  if (phase == Phase::Fault) showSegmentValue(-1);
  else if (mode == Mode::Stats) showSegmentValue(measurement.hrValid() ? int16_t(measurement.bpm()) : -1);
  else {
    showSegmentValue(-1);
    enabled = flashActive;
  }
  if (segment4.enabled() != enabled) segment4.enable(enabled);
}

void captureFrame() {
  frame.mode = mode;
  frame.phase = phase;
  frame.quality = measurement.quality();
  frame.red = measurement.red();
  frame.ir = measurement.ir();
  frame.bpm = measurement.bpm();
  frame.spo2 = measurement.spo2x10();
  frame.hasSample = measurement.hasSample();
  frame.hrValid = measurement.hrValid();
  frame.spo2Valid = measurement.spo2Valid();
  frame.faults = faultMask;
}

void renderRow(uint8_t row, char *text) {
  if (row == 0) {
    copyFlash(text, frame.mode == Mode::Raw ? PSTR("MODE: RAW") : PSTR("MODE: STATS"));
    return;
  }
  if (frame.mode == Mode::Raw) {
    if (row == 1) {
      if (frame.hasSample) snprintf_P(text, 22, PSTR("RED: %lu"), (unsigned long)frame.red);
      else copyFlash(text, PSTR("RED: --"));
      return;
    }
    if (row == 2) {
      if (frame.hasSample) snprintf_P(text, 22, PSTR("IR : %lu"), (unsigned long)frame.ir);
      else copyFlash(text, PSTR("IR : --"));
      return;
    }
  } else {
    if (row == 1) {
      if (frame.spo2Valid) snprintf_P(text, 22, PSTR("SpO2 EST: %u%%"), unsigned((frame.spo2 + 5) / 10));
      else copyFlash(text, PSTR("SpO2 EST: --%"));
      return;
    }
    if (row == 2) {
      if (frame.hrValid) snprintf_P(text, 22, PSTR("HR: %u BPM"), unsigned(frame.bpm));
      else copyFlash(text, PSTR("HR: -- BPM"));
      return;
    }
  }
  if (frame.faults & SensorFault) copyFlash(text, PSTR("ERROR: SENSOR"));
  else if (frame.faults & NoSamples) copyFlash(text, PSTR("ERROR: NO SAMPLES"));
  else if (frame.faults & DisplayFault) copyFlash(text, PSTR("ERROR: OLED"));
  else if (frame.quality == Measurement::Quality::Clipped) copyFlash(text, qualityText(frame.quality));
  else if (frame.phase == Phase::Waiting) copyFlash(text, PSTR("WAIT FINGER"));
  else if (frame.mode == Mode::Stats && frame.hrValid && !frame.spo2Valid &&
           frame.quality == Measurement::Quality::Good) copyFlash(text, PSTR("SpO2 COLLECTING"));
  else copyFlash(text, qualityText(frame.quality));
}

void serviceDisplay() {
  oled.update();
  if (oled.state() == Oled::State::Fault || oled.state() == Oled::State::AddressMissing) {
    faultMask |= DisplayFault;
    return;
  }
  if (!oled.ready() || oled.busy()) return;
  const uint32_t now = millis();
  const uint16_t interval = mode == Mode::Raw ? Board::RAW_REFRESH_MS : Board::STATS_REFRESH_MS;
  if (uiUrgent || (!uiActive && uint32_t(now - lastUiAt) >= interval)) {
    captureFrame();
    uiRow = 0;
    uiActive = true;
    uiUrgent = false;
    lastUiAt = now;
  }
  if (uiActive) {
    char text[22];
    renderRow(uiRow, text);
    if (oled.writeText(0, uiRow, text) && ++uiRow == 4) uiActive = false;
  }
}

void setup() {
  buttons.begin();
  segment4.begin();
  bus.begin(Board::I2C_HZ, Board::I2C_TIMEOUT_US);
  restartMeasurement();
}

void loop() {
  serviceKeys();
  serviceSensor();
  updateState();
  serviceSegments();
  serviceDisplay();
}
