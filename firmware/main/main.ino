/*
 * Board application v0.3.0: S2 restart, S3 raw, S4 rolling statistics (default).
 * Target: ATmega328P, 16 MHz. Keep your working bootloader/upload settings.
 * 0.91-inch white SSD1306 128x32 at 0x3C. Serial: 115200 + newline, type help.
 * SpO2 is an uncalibrated engineering estimate; validate on the physical board.
 */
#include <Arduino.h>
#include <Wire.h>
#include <avr/pgmspace.h>
#include <stdio.h>
#include <string.h>
#include "BoardConfig.h"
#include "Buttons.h"
#include "Segment4.h"
#include "I2cBus.h"
#include "Max30102.h"
#include "Oled.h"
#include "Measurement.h"
#include "SerialConsole.h"

void onCommand(char *line);
enum class Mode : uint8_t { Raw, Stats };
enum class Phase : uint8_t { Waiting, Collecting, Measuring, Fault };
enum Fault : uint8_t { SensorFault = 1, DisplayFault = 2, NoSamples = 4 };

Buttons buttons(Board::KEY_PINS, Board::KEY_DEBOUNCE_MS, Board::KEY_LONG_MS);
Segment4 segment4(Board::SEGMENT_PINS, Board::DIGIT_PINS);
I2cBus bus(Wire);
Max30102 sensor(bus, Board::MAX30102_ADDRESS);
Oled oled(bus, Board::OLED_ADDRESS);
Measurement measurement(Board::FINGER_ON_IR, Board::FINGER_OFF_IR, Board::MINIMUM_RED);
SerialConsole console(Serial, onCommand);
SEGMENT4_TIMER2_ISR(segment4)
#define EQ(word, literal) (strcmp_P((word), PSTR(literal)) == 0)

Mode mode = Mode::Stats;
Phase phase = Phase::Waiting;
uint8_t faultMask = 0;
uint8_t redLedCode = Board::RED_LED_CODE, irLedCode = Board::IR_LED_CODE;
uint32_t lastPoll = 0, lastSampleAt = 0, lastUiAt = 0, lastStreamAt = 0;
uint32_t flashStarted = 0, maximumCalcUs = 0;
uint16_t overflowRestarts = 0;
bool flashActive = false, streamEnabled = false;
bool uiUrgent = true, uiActive = false;
uint8_t uiRow = 0;
int16_t segmentValue = -32768;

struct UiFrame {
  Mode mode; Phase phase; Measurement::Quality quality;
  uint32_t red, ir;
  uint16_t bpm, spo2;
  bool hasSample, hrValid, spo2Valid;
  uint8_t faults;
} frame;

void printFlash(const char *text) {
  Serial.print(reinterpret_cast<const __FlashStringHelper *>(text));
}
const char *phaseText(Phase value) {
  switch (value) {
    case Phase::Waiting: return PSTR("WAIT FINGER");
    case Phase::Collecting: return PSTR("COLLECTING");
    case Phase::Measuring: return PSTR("MEASURING");
    default: return PSTR("FAULT");
  }
}
const char *qualityText(Measurement::Quality value) {
  switch (value) {
    case Measurement::Quality::NoFinger: return PSTR("WAIT FINGER");
    case Measurement::Quality::Warming: return PSTR("COLLECTING SIGNAL");
    case Measurement::Quality::Good: return PSTR("SIGNAL: OK");
    case Measurement::Quality::LowPulse: return PSTR("LOW SIGNAL");
    case Measurement::Quality::Unstable: return PSTR("MOVE / LOW SIGNAL");
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
  mode = next; flashActive = false;
  requestUi();
  Serial.println(mode == Mode::Raw ? F("MODE RAW") : F("MODE STATS"));
}
void restartMeasurement(bool manual = true) {
  // Mode and configured thresholds survive S2; sampling/algorithm history does not.
  measurement.reset(); flashActive = false;
  if (manual) { overflowRestarts = 0; maximumCalcUs = 0; }
  faultMask = 0;
  bus.clearTimeout();
  if (!sensor.begin(redLedCode, irLedCode)) faultMask |= SensorFault;
  if (!oled.ready() && !oled.busy() && !oled.begin(Board::OLED_ADDRESS)) faultMask |= DisplayFault;
  lastPoll = lastSampleAt = millis();
  phase = faultMask ? Phase::Fault : Phase::Waiting;
  requestUi();
  Serial.print(F("RESTART mode=")); Serial.println(mode == Mode::Raw ? F("RAW") : F("STATS"));
  if (faultMask) { Serial.print(F("FAULT mask=")); Serial.println(faultMask); }
}
void serviceKeys() {
  buttons.update(); Buttons::Event event;
  while (buttons.read(event)) {
    if (event.type != Buttons::Type::Press) continue;
    switch (event.key) {
      case 0: restartMeasurement(); break; // S2, one action per press.
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
    faultMask |= SensorFault; measurement.reset(); flashActive = false;
    Serial.print(F("ERR MAX bus=")); Serial.println(sensor.lastBusStatus());
    sensor.stop(); requestUi(); return;
  }
  if (sensor.lostMinimum() != previousLost) {
    ++overflowRestarts;
    Serial.println(F("WARN FIFO gap; restarting measurement history"));
    restartMeasurement(false); return; // Never time missing samples as contiguous.
  }
  if (count) {
    lastSampleAt = now;
    for (int8_t i = 0; i < count; ++i) measurement.push(samples[i].red, samples[i].ir);
  } else if (uint32_t(now - lastSampleAt) >= 1000) {
    faultMask |= NoSamples; measurement.reset(); flashActive = false;
    sensor.stop(); requestUi(); Serial.println(F("ERR no samples; press S2 to retry"));
  }
  const uint32_t started = micros();
  if (measurement.update()) {
    const uint32_t duration = micros() - started;
    if (duration > maximumCalcUs) maximumCalcUs = duration;
  }
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
  if (phase != previous) {
    requestUi(); Serial.print(F("STATE ")); printFlash(phaseText(phase)); Serial.println();
  }
}
void showSegmentValue(int16_t value) {
  if (segmentValue == value) return;
  segmentValue = value;
  if (value >= 0) segment4.showNumber(value);
  else { const uint8_t dashes[4] = {0x40, 0x40, 0x40, 0x40}; segment4.showRaw(dashes); }
}
void serviceSegments() {
  const uint32_t now = millis();
  const bool beat = measurement.takeBeat();
  if (beat && mode == Mode::Raw && phase != Phase::Fault) {
    flashActive = true; flashStarted = now;
  }
  if (!measurement.finger() || phase == Phase::Fault ||
      (flashActive && uint32_t(now - flashStarted) >= Board::HEART_FLASH_MS)) flashActive = false;
  bool enabled = true;
  if (phase == Phase::Fault) showSegmentValue(-1);
  else if (mode == Mode::Stats) showSegmentValue(measurement.hrValid() ? int16_t(measurement.bpm()) : -1);
  else { showSegmentValue(-1); enabled = flashActive; }
  if (segment4.enabled() != enabled) segment4.enable(enabled);
}
void captureFrame() {
  frame.mode = mode; frame.phase = phase; frame.quality = measurement.quality();
  frame.red = measurement.red(); frame.ir = measurement.ir();
  frame.bpm = measurement.bpm(); frame.spo2 = measurement.spo2x10();
  frame.hasSample = measurement.hasSample();
  frame.hrValid = measurement.hrValid(); frame.spo2Valid = measurement.spo2Valid();
  frame.faults = faultMask;
}
void renderRow(uint8_t row, char *text) {
  if (row == 0) { copyFlash(text, frame.mode == Mode::Raw ? PSTR("MODE: RAW") : PSTR("MODE: STATS")); return; }
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
    if (!(faultMask & DisplayFault)) { faultMask |= DisplayFault; Serial.println(F("ERR OLED; press S2 to retry")); }
    return;
  }
  if (!oled.ready() || oled.busy()) return;
  const uint32_t now = millis();
  const uint16_t interval = mode == Mode::Raw ? Board::RAW_REFRESH_MS : Board::STATS_REFRESH_MS;
  if (uiUrgent || (!uiActive && uint32_t(now - lastUiAt) >= interval)) {
    captureFrame(); uiRow = 0; uiActive = true; uiUrgent = false; lastUiAt = now;
  }
  if (uiActive) {
    char text[22]; renderRow(uiRow, text);
    if (oled.writeText(0, uiRow, text)) { if (++uiRow == 4) uiActive = false; }
  }
}
void printStatus() {
  Serial.print(F("mode=")); Serial.print(mode == Mode::Raw ? F("RAW") : F("STATS"));
  Serial.print(F(" state=")); printFlash(phaseText(phase));
  Serial.print(F(" finger=")); Serial.print(measurement.finger() ? 1 : 0);
  Serial.print(F(" red=")); Serial.print(measurement.red());
  Serial.print(F(" ir=")); Serial.print(measurement.ir());
  Serial.print(F(" bpm=")); if (measurement.hrValid()) Serial.print(measurement.bpm()); else Serial.print(F("--"));
  Serial.print(F(" spo2_x10=")); if (measurement.spo2Valid()) Serial.print(measurement.spo2x10()); else Serial.print(F("--"));
  Serial.print(F(" q=")); Serial.print(uint8_t(measurement.quality()));
  Serial.print(F(" R_x1000=")); Serial.print(measurement.ratiox1000());
  Serial.print(F(" corr_x1000=")); Serial.print(measurement.correlationx1000());
  Serial.print(F(" periodic_x1000=")); Serial.print(measurement.periodicityx1000());
  Serial.print(F(" pi_x10000=")); Serial.print(measurement.perfusionx10000());
  Serial.print(F(" window=")); Serial.print(measurement.windowSamples());
  Serial.print(F(" samples=")); Serial.print(measurement.samples());
  Serial.print(F(" faults=")); Serial.println(faultMask);
}
void onCommand(char *line) {
  char *args[4]; const uint8_t n = SerialConsole::split(line, args, 4);
  if (!n) return;
  if (n == 1 && EQ(args[0], "help")) {
    Serial.println(F("Application v0.3.0: S2 restart, S3 RAW, S4 STATS (boot default)."));
    Serial.println(F("help | status | reset | mode raw | mode stats | stream on | stream off"));
    Serial.println(F("threshold <enterIR> <leaveIR>   (0 < leave < enter < 260000)"));
    Serial.println(F("params   (thresholds are volatile; S2 preserves them)"));
    Serial.println(F("led <red 0..255> <ir 0..255>   (register codes, not mA)"));
    return;
  }
  if (n == 1 && EQ(args[0], "status")) { printStatus(); return; }
  if (n == 1 && EQ(args[0], "reset")) { restartMeasurement(); return; }
  if (n == 2 && EQ(args[0], "mode")) {
    if (EQ(args[1], "raw")) setMode(Mode::Raw);
    else if (EQ(args[1], "stats")) setMode(Mode::Stats);
    else Serial.println(F("ERR mode raw|stats"));
    return;
  }
  if (n == 2 && EQ(args[0], "stream")) {
    if (EQ(args[1], "on")) streamEnabled = true;
    else if (EQ(args[1], "off")) streamEnabled = false;
    else { Serial.println(F("ERR stream on|off")); return; }
    Serial.println(F("OK stream")); return;
  }
  if (n == 3 && EQ(args[0], "threshold")) {
    long on, off;
    if (!SerialConsole::number(args[1], 2, 259999, on) ||
        !SerialConsole::number(args[2], 1, 259998, off) || off >= on) {
      Serial.println(F("ERR 0 < leave < enter < 260000")); return;
    }
    measurement.setThresholds(on, off); restartMeasurement(); return;
  }
  if (n == 3 && EQ(args[0], "led")) {
    long red, ir;
    if (!SerialConsole::number(args[1], 0, 255, red) || !SerialConsole::number(args[2], 0, 255, ir)) {
      Serial.println(F("ERR led codes 0..255")); return;
    }
    redLedCode = red; irLedCode = ir; restartMeasurement(); return;
  }
  if (n == 1 && EQ(args[0], "params")) {
    Serial.print(F("enterIR=")); Serial.print(measurement.onThreshold());
    Serial.print(F(" leaveIR=")); Serial.print(measurement.offThreshold());
    Serial.print(F(" redLED=")); Serial.print(redLedCode);
    Serial.print(F(" irLED=")); Serial.print(irLedCode);
    Serial.print(F(" overflow_restarts=")); Serial.print(overflowRestarts);
    Serial.print(F(" calc_us_max=")); Serial.println(maximumCalcUs); return;
  }
  Serial.println(F("ERR command; type help"));
}
void setup() {
  Serial.begin(Board::SERIAL_BAUD);
  buttons.begin(); segment4.begin();
  bus.begin(Board::I2C_HZ, Board::I2C_TIMEOUT_US);
  Serial.println(F("\nBoard application v0.3.0; 115200 baud; type help"));
  restartMeasurement();
}
void loop() {
  serviceKeys(); serviceSensor(); updateState(); serviceSegments(); serviceDisplay(); console.update();
  if (streamEnabled && uint32_t(millis() - lastStreamAt) >= 100) { lastStreamAt = millis(); printStatus(); }
}
