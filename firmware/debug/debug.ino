/*
 * Hardware debug console.
 * Open debug/debug.ino in Arduino IDE; keep the complete debug sketch folder.
 * Keep the board/processor/upload settings that already work with your bootloader.
 * Serial Monitor: 115200 baud, Newline or Both NL & CR; type help.
 * Only Arduino AVR core + its bundled Wire library are required.
 */
#include <Arduino.h>
#include <Wire.h>
#include <avr/pgmspace.h>
#include <string.h>
#include "BoardConfig.h"
#include "Buttons.h"
#include "Segment4.h"
#include "I2cBus.h"
#include "Max30102.h"
#include "Oled.h"
#include "SerialConsole.h"

void onCommand(char *line);

Buttons buttons(Board::KEY_PINS, Board::KEY_DEBOUNCE_MS, Board::KEY_LONG_MS);
Segment4 segment4(Board::SEGMENT_PINS, Board::DIGIT_PINS);
I2cBus bus(Wire);
Max30102 sensor(bus, Board::MAX30102_ADDRESS);
Oled oled(bus, Board::OLED_ADDRESS);
SerialConsole console(Serial, onCommand);
SEGMENT4_TIMER2_ISR(segment4)

// Literal command words stay in flash, not the ATmega328P's 2 KB SRAM.
#define EQ(word, literal) (strcmp_P((word), PSTR(literal)) == 0)

bool keyEvents = true;
bool scanning = false;
uint8_t scanAddress = 1, scanFound = 0;
bool segmentTest = false;
uint8_t testStep = 0;
uint32_t testDue = 0;
bool streaming = false, haveSample = false;
uint16_t streamInterval = 100;
uint32_t lastPoll = 0, lastPrint = 0, lastSampleMs = 0, sampleCount = 0;
Max30102::Sample latest = {0, 0};

void hexByte(uint8_t value) {
  if (value < 16) Serial.print('0');
  Serial.print(value, HEX);
}

void usageError() { Serial.println(F("ERR arguments; type help")); }

void busError() {
  Serial.print(F("ERR I2C status=")); Serial.print(bus.status());
  Serial.print(F(" timeout=")); Serial.println(bus.timedOut() ? 1 : 0);
}

void maxError() {
  Serial.print(F("ERR MAX code=")); Serial.print(uint8_t(sensor.error()));
  Serial.print(F(" last_bus_error=")); Serial.println(sensor.lastBusStatus());
}

void oledError() {
  Serial.print(F("ERR OLED code=")); Serial.print(uint8_t(oled.error()));
  Serial.print(F(" bus=")); Serial.println(bus.status());
}

void printHelp() {
  Serial.println(F("Commands (case sensitive; send a newline):"));
  Serial.println(F(" help | status | pins"));
  Serial.println(F(" keys on | off | read"));
  Serial.println(F(" seg number <-999..9999> | digits <0123 or 1-2_>"));
  Serial.println(F(" seg raw <digit 1..4> <mask 0..0x7F>"));
  Serial.println(F(" seg test | clear | on | off"));
  Serial.println(F(" i2c scan"));
  Serial.println(F(" max id | begin | stop | read | stats"));
  Serial.println(F(" max stream on [20..5000 ms] | stream off"));
  Serial.println(F(" max led <red 0..255> <ir 0..255>"));
  Serial.println(F(" oled status | probe <0x3C or 0x3D> | begin (fixed 0x3C)"));
  Serial.println(F(" oled test | clear | on | off"));
  Serial.println(F(" oled pattern border | checker"));
  Serial.println(F(" oled text <row 0..3> <ASCII text, max 21 chars>"));
  Serial.println(F(" oled fill <0|1> | invert <0|1> | flip <0|1>"));
  Serial.println(F(" oled contrast <0..255>"));
  Serial.println(F("S1=hardware RESET. keys events: PRESS/RELEASE/CLICK/LONG."));
}

void printPins() {
  Serial.println(F("S1=RESET/PC6 (29), not a user GPIO key"));
  Serial.println(F("S2=A1/PC1 (24), S3=A2/PC2 (25), S4=A3/PC3 (26)"));
  Serial.println(F("Keys: external 10k pull-ups, pressed=LOW"));
  Serial.println(F("Segments a..g: D2,D3,D4,D5,D6,D7,D8; HIGH=on"));
  Serial.println(F("DIG1..4: D10,D11,D12,D13; LOW=selected; no DP wire"));
  Serial.println(F("I2C: SDA=A4/PC4 (27), SCL=A5/PC5 (28), 100kHz"));
  Serial.println(F("UART: D0=RX, D1=TX via CH340C; keep free for Serial"));
  Serial.println(F("Timer2=segment scan. D10..13 also share ISP/SPI nets."));
}

void printKeys() {
  for (uint8_t i = 0; i < 3; ++i) {
    Serial.print('S'); Serial.print(i + 2);
    Serial.print(F(" raw=")); Serial.print(digitalRead(Board::KEY_PINS[i]));
    Serial.print(F(" pressed=")); Serial.println(buttons.pressed(i) ? 1 : 0);
  }
}

void printSample() {
  if (!haveSample) { Serial.println(F("MAX no sample yet; use max begin")); return; }
  Serial.print(F("MAX seq=")); Serial.print(sampleCount);
  Serial.print(F(" rx_ms=")); Serial.print(lastSampleMs);
  Serial.print(F(" red=")); Serial.print(latest.red);
  Serial.print(F(" ir=")); Serial.print(latest.ir);
  Serial.print(F(" age_ms=")); Serial.println(uint32_t(millis() - lastSampleMs));
}

void printMaxStats() {
  Serial.print(F("MAX running=")); Serial.print(sensor.ready() ? 1 : 0);
  Serial.print(F(" samples=")); Serial.print(sampleCount);
  Serial.print(F(" lost_min=")); Serial.print(sensor.lostMinimum());
  Serial.print(F(" stream=")); Serial.print(streaming ? 1 : 0);
  Serial.print(F(" interval_ms=")); Serial.println(streamInterval);
}

void printOledStatus() {
  Serial.print(F("OLED profile=SSD1306 128x32 white address=0x")); hexByte(oled.address());
  Serial.print(F(" state=")); Serial.print(uint8_t(oled.state()));
  Serial.print(F(" ready=")); Serial.print(oled.ready() ? 1 : 0);
  Serial.print(F(" busy=")); Serial.println(oled.busy() ? 1 : 0);
  Serial.print(F("OLED on=")); Serial.print(oled.displayOn() ? 1 : 0);
  Serial.print(F(" invert=")); Serial.print(oled.inverted() ? 1 : 0);
  Serial.print(F(" flip=")); Serial.println(oled.flipped() ? 1 : 0);
  Serial.print(F("Last probe address=0x")); hexByte(oled.lastProbeAddress());
  Serial.print(F(" ACK=")); Serial.println(oled.lastProbeAck() ? 1 : 0);
}

void onCommand(char *line) {
  char *args[6];
  char *cursor = line;
  uint8_t n = 0;
  while (n < 2) {
    char *word = SerialConsole::token(cursor);
    if (!word) break;
    args[n++] = word;
  }
  if (!n) return;
  // Keep all spaces in an OLED message, rather than splitting each word.
  if (n == 2 && EQ(args[0], "oled") && EQ(args[1], "text")) {
    char *row = SerialConsole::token(cursor);
    while (*cursor == ' ' || *cursor == '\t') ++cursor;
    long rowNumber;
    if (!row || !*cursor || !SerialConsole::number(row, 0, 3, rowNumber)) {
      usageError(); return;
    }
    if (!oled.writeText(0, rowNumber, cursor)) { oledError(); return; }
    Serial.println(F("OK OLED text queued")); return;
  }
  const uint8_t extra = SerialConsole::split(cursor, args + n, 6 - n);
  if (extra == 255) { usageError(); return; }
  n += extra;
  long value, other;
  if (EQ(args[0], "help") && n == 1) { printHelp(); return; }
  if (EQ(args[0], "pins") && n == 1) { printPins(); return; }
  if (EQ(args[0], "status") && n == 1) {
    Serial.print(F("Board console uptime_ms=")); Serial.println(millis());
    Serial.print(F("Key events=")); Serial.print(keyEvents ? 1 : 0);
    Serial.print(F(" queue_drops=")); Serial.println(buttons.droppedEvents());
    Serial.print(F("Segment enabled=")); Serial.print(segment4.enabled() ? 1 : 0);
    Serial.print(F(" test=")); Serial.println(segmentTest ? 1 : 0);
    printMaxStats(); printOledStatus(); return;
  }
  if (EQ(args[0], "keys") && n == 2) {
    if (EQ(args[1], "read")) { printKeys(); return; }
    if (EQ(args[1], "on")) keyEvents = true;
    else if (EQ(args[1], "off")) keyEvents = false;
    else { usageError(); return; }
    Serial.println(F("OK keys")); return;
  }
  if (EQ(args[0], "seg") && n >= 2) {
    if (EQ(args[1], "number") && n == 3 &&
        SerialConsole::number(args[2], -999, 9999, value)) {
      segmentTest = false; segment4.showNumber(int16_t(value)); segment4.enable(true);
    } else if (EQ(args[1], "digits") && n == 3 && strlen(args[2]) == 4) {
      uint8_t masks[4];
      for (uint8_t i = 0; i < 4; ++i) {
        const char c = args[2][i];
        if (c >= '0' && c <= '9') masks[i] = Segment4::digitMask(c - '0');
        else if (c == '-') masks[i] = 0x40;
        else if (c == '_') masks[i] = 0;
        else { usageError(); return; }
      }
      segmentTest = false; segment4.showRaw(masks); segment4.enable(true);
    } else if (EQ(args[1], "raw") && n == 4 &&
        SerialConsole::number(args[2], 1, 4, value) &&
        SerialConsole::number(args[3], 0, 127, other)) {
      segmentTest = false; segment4.showDigitMask(value, other); segment4.enable(true);
    } else if (EQ(args[1], "test") && n == 2) {
      segmentTest = true; testStep = 0; testDue = millis(); segment4.enable(true);
      Serial.println(F("SEG walking a..g on DIG1..4; about 7 seconds")); return;
    } else if (EQ(args[1], "clear") && n == 2) {
      segmentTest = false; segment4.clear();
    } else if (EQ(args[1], "off") && n == 2) {
      segmentTest = false; segment4.enable(false);
    } else if (EQ(args[1], "on") && n == 2) {
      segmentTest = false; segment4.enable(true);
    } else { usageError(); return; }
    Serial.println(F("OK seg")); return;
  }
  if (EQ(args[0], "i2c") && n == 2 && EQ(args[1], "scan")) {
    if (scanning) { Serial.println(F("ERR scan already running")); return; }
    scanning = true; scanAddress = 1; scanFound = 0; bus.clearTimeout();
    Serial.println(F("I2C scan started")); return;
  }
  if (EQ(args[0], "max") && n >= 2) {
    if (EQ(args[1], "id") && n == 2) {
      uint8_t part, revision;
      if (!sensor.identify(part, revision)) { maxError(); return; }
      Serial.print(F("MAX part=0x")); hexByte(part);
      Serial.print(F(" revision=0x")); hexByte(revision); Serial.println(); return;
    }
    if (EQ(args[1], "begin") && n == 2) {
      streaming = false; haveSample = false; sampleCount = 0;
      bus.clearTimeout();
      if (!sensor.begin()) { maxError(); return; }
      lastPoll = millis();
      Serial.println(F("OK MAX red+IR, 100sps, 18bit, LED codes 31/31")); return;
    }
    if (EQ(args[1], "stop") && n == 2) {
      streaming = false;
      if (!sensor.stop()) { maxError(); return; }
      Serial.println(F("OK MAX shutdown")); return;
    }
    if (EQ(args[1], "read") && n == 2) { printSample(); return; }
    if (EQ(args[1], "stats") && n == 2) { printMaxStats(); return; }
    if (EQ(args[1], "led") && n == 4 &&
        SerialConsole::number(args[2], 0, 255, value) &&
        SerialConsole::number(args[3], 0, 255, other)) {
      if (!sensor.ready()) { Serial.println(F("ERR use max begin first")); return; }
      if (!sensor.setLedCodes(value, other)) { maxError(); return; }
      Serial.println(F("OK LED codes")); return;
    }
    if (EQ(args[1], "stream") && (n == 3 || n == 4)) {
      if (EQ(args[2], "off") && n == 3) {
        streaming = false; Serial.println(F("OK stream off")); return;
      }
      if (EQ(args[2], "on") && (n == 3 ||
          SerialConsole::number(args[3], 20, 5000, value))) {
        if (!sensor.ready()) { Serial.println(F("ERR use max begin first")); return; }
        if (n == 4) streamInterval = value;
        streaming = true; lastPrint = millis();
        Serial.println(F("OK stream latest sample; FIFO is drained independently")); return;
      }
    }
    usageError(); return;
  }
  if (EQ(args[0], "oled") && n >= 2) {
    if (EQ(args[1], "status") && n == 2) { printOledStatus(); return; }
    if (EQ(args[1], "probe") && n == 3 &&
        SerialConsole::number(args[2], 0x3C, 0x3D, value)) {
      bus.clearTimeout();
      if (!oled.probe(value)) { oledError(); return; }
      Serial.println(F("OLED address ACK; run oled begin then oled test")); return;
    }
    if (EQ(args[1], "begin") && (n == 2 || n == 3)) {
      if (n == 3 && !SerialConsole::number(args[2], 0x3C, 0x3D, value)) {
        usageError(); return;
      }
      if (n == 3 && value != Board::OLED_ADDRESS) {
        Serial.println(F("ERR this board OLED address is fixed at 0x3C; use oled begin"));
        return;
      }
      bus.clearTimeout();
      if (!oled.begin(Board::OLED_ADDRESS)) { oledError(); return; }
      Serial.println(F("OK OLED init queued; wait for OLED job done")); return;
    }
    bool ok;
    if (EQ(args[1], "clear") && n == 2) ok = oled.clear();
    else if (EQ(args[1], "test") && n == 2)
      ok = oled.setInverted(false) && oled.setFlipped(false) && oled.setDisplayOn(true) && oled.test();
    else if (EQ(args[1], "pattern") && n == 3 && EQ(args[2], "border"))
      ok = oled.test(Oled::Pattern::Border);
    else if (EQ(args[1], "pattern") && n == 3 && EQ(args[2], "checker"))
      ok = oled.test(Oled::Pattern::Checker);
    else if (EQ(args[1], "fill") && n == 3 && SerialConsole::number(args[2], 0, 1, value))
      ok = oled.fill(value);
    else if (EQ(args[1], "invert") && n == 3 && SerialConsole::number(args[2], 0, 1, value))
      ok = oled.setInverted(value);
    else if (EQ(args[1], "flip") && n == 3 && SerialConsole::number(args[2], 0, 1, value))
      ok = oled.setFlipped(value);
    else if (EQ(args[1], "contrast") && n == 3 && SerialConsole::number(args[2], 0, 255, value))
      ok = oled.setContrast(value);
    else if (EQ(args[1], "on") && n == 2) ok = oled.setDisplayOn(true);
    else if (EQ(args[1], "off") && n == 2) ok = oled.setDisplayOn(false);
    else { usageError(); return; }
    if (!ok) { oledError(); return; }
    if (oled.busy()) Serial.println(F("OK OLED job queued"));
    else Serial.println(F("OK OLED"));
    return;
  }
  Serial.println(F("ERR unknown command; type help"));
}

void serviceKeys() {
  buttons.update();
  Buttons::Event e;
  while (buttons.read(e)) {
    if (!keyEvents) continue;
    Serial.print(F("KEY S")); Serial.print(e.key + 2); Serial.print(' ');
    switch (e.type) {
      case Buttons::Type::Press: Serial.print(F("PRESS")); break;
      case Buttons::Type::Release: Serial.print(F("RELEASE")); break;
      case Buttons::Type::Click: Serial.print(F("CLICK")); break;
      case Buttons::Type::LongPress: Serial.print(F("LONG")); break;
    }
    Serial.print(F(" held_ms=")); Serial.println(e.heldMs);
  }
}

void serviceSegments() {
  if (!segmentTest || int32_t(millis() - testDue) < 0) return;
  testDue = millis() + 250;
  if (testStep < 28) {
    segment4.showDigitMask(testStep / 7 + 1, 1U << (testStep % 7));
    ++testStep;
  } else {
    segmentTest = false; segment4.showNumber(1234);
    Serial.println(F("SEG test done; displaying 1234"));
  }
}

void serviceMax() {
  const uint32_t now = millis();
  if (!sensor.ready() || uint32_t(now - lastPoll) < 10) return;
  lastPoll = now;
  Max30102::Sample batch[5];
  const uint32_t previousLost = sensor.lostMinimum();
  const int8_t count = sensor.readSamples(batch, 5);
  if (count < 0) {
    maxError(); streaming = false;
    sensor.stop(); // Requires explicit max begin to recover a broken session.
    return;
  }
  if (count) {
    latest = batch[count - 1]; haveSample = true;
    sampleCount += count; lastSampleMs = now;
  }
  if (sensor.lostMinimum() != previousLost) {
    Serial.print(F("WARN MAX FIFO lost_min=")); Serial.println(sensor.lostMinimum());
  }
  if (streaming && haveSample && uint32_t(now - lastPrint) >= streamInterval) {
    lastPrint = now; printSample();
  }
}

void serviceScan() {
  if (!scanning) return;
  bus.clearTimeout();
  if (bus.probe(scanAddress)) {
    Serial.print(F("I2C ACK 0x")); hexByte(scanAddress); Serial.println(); ++scanFound;
  } else if (bus.timedOut() || bus.status() == 4 || bus.status() == 5) {
    busError(); scanning = false; return;
  }
  if (++scanAddress > 126) {
    scanning = false;
    Serial.print(F("I2C scan done; devices=")); Serial.println(scanFound);
  }
}

void serviceOled() {
  const bool wasBusy = oled.busy();
  oled.update();
  if (wasBusy && !oled.busy()) {
    if (oled.error() == Oled::Error::None) Serial.println(F("OLED job done"));
    else oledError();
  }
}

void setup() {
  Serial.begin(Board::SERIAL_BAUD);
  buttons.begin();
  segment4.begin(); // Starts blank; no autonomous test or sensor LED activation.
  bus.begin(Board::I2C_HZ, Board::I2C_TIMEOUT_US);
  Serial.println(F("\nBoard peripheral console; 115200 baud; type help"));
  Serial.println(F("Keys active; segment blank; MAX stopped; OLED idle (use oled begin)."));
}

void loop() {
  serviceKeys();
  serviceMax();
  serviceSegments();
  console.update();
  serviceScan();
  serviceOled();
}
