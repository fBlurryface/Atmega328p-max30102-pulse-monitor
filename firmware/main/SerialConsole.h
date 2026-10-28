#pragma once
#include <Arduino.h>
#include <stdlib.h>
#include <errno.h>

// Fixed buffer, no Arduino String / heap. Commands are dispatched in loop().
// CR, LF and CRLF supported; overflow is discarded through the next newline.
class SerialConsole {
 public:
  using Handler = void (*)(char *line);
  SerialConsole(Stream &stream, Handler handler) : stream_(stream), handler_(handler) {}
  void update() {
    uint8_t budget = 24; // Give acquisition and keys CPU time even during RX bursts.
    while (budget-- && stream_.available()) {
      const char c = char(stream_.read());
      if (c == '\r' || c == '\n') {
        if (overflow_) stream_.println(F("ERR line too long; discarded"));
        else if (used_) { buffer_[used_] = '\0'; handler_(buffer_); }
        used_ = 0; overflow_ = false;
      } else if (!overflow_ && (c == '\b' || c == 127)) {
        if (used_) --used_;
      } else if (((c >= 32 && c <= 126) || c == '\t') && !overflow_) {
        if (used_ < sizeof(buffer_) - 1) buffer_[used_++] = c;
        else overflow_ = true;
      }
    }
  }
  // In-place tokenizer; returns 255 rather than silently truncating extra args.
  static char *token(char *&cursor) {
    while (*cursor == ' ' || *cursor == '\t') ++cursor;
    if (!*cursor) return nullptr;
    char *start = cursor;
    while (*cursor && *cursor != ' ' && *cursor != '\t') ++cursor;
    if (*cursor) *cursor++ = '\0';
    return start;
  }
  static uint8_t split(char *line, char **args, uint8_t capacity) {
    uint8_t count = 0;
    char *p = line;
    while (*p) {
      while (*p == ' ' || *p == '\t') ++p;
      if (!*p) break;
      if (count == capacity) return 255;
      args[count++] = p;
      while (*p && *p != ' ' && *p != '\t') ++p;
      if (*p) *p++ = '\0';
    }
    return count;
  }
  static bool number(const char *text, long low, long high, long &value) {
    if (!text || !*text) return false;
    char *end;
    errno = 0;
    const char *digits = (*text == '-' || *text == '+') ? text + 1 : text;
    const uint8_t base = digits[0] == '0' && (digits[1] == 'x' || digits[1] == 'X') ? 16 : 10;
    value = strtol(text, &end, base); // Decimal, or explicit 0x-prefixed hex.
    return errno != ERANGE && *end == '\0' && value >= low && value <= high;
  }

 private:
  Stream &stream_;
  Handler handler_;
  char buffer_[80];
  uint8_t used_ = 0;
  bool overflow_ = false;
};
