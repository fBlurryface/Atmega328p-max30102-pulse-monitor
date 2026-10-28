#pragma once
#include <Arduino.h>

// Header-only; no dependency on another peripheral or main.ino globals.
class Buttons {
 public:
  enum class Type : uint8_t { Press, Release, Click, LongPress };
  struct Event { uint8_t key; Type type; uint32_t heldMs; }; // key=0..2 => S2..S4.

  explicit Buttons(const uint8_t (&pins)[3], uint16_t debounceMs = 25,
                   uint16_t longMs = 800)
      : debounceMs_(debounceMs), longMs_(longMs) {
    for (uint8_t i = 0; i < 3; ++i) pins_[i] = pins[i];
  }
  void begin() {
    head_ = tail_ = 0;
    dropped_ = 0;
    const uint32_t now = millis();
    for (uint8_t i = 0; i < 3; ++i) {
      pinMode(pins_[i], INPUT); // Board already has external 10k pull-ups.
      State &s = states_[i];
      s.raw = s.down = digitalRead(pins_[i]) == LOW;
      s.rawSince = s.pressedAt = now;
      s.longSent = false;
    }
  }
  void update() {
    const uint32_t now = millis();
    for (uint8_t i = 0; i < 3; ++i) {
      State &s = states_[i];
      const bool raw = digitalRead(pins_[i]) == LOW;
      if (raw != s.raw) { s.raw = raw; s.rawSince = now; }
      if (s.down != s.raw && uint32_t(now - s.rawSince) >= debounceMs_) {
        s.down = s.raw;
        if (s.down) {
          s.pressedAt = now;
          s.longSent = false;
          push(i, Type::Press, 0);
        } else {
          const uint32_t held = now - s.pressedAt;
          if (!s.longSent && held >= longMs_) {
            s.longSent = true;
            push(i, Type::LongPress, held);
          }
          push(i, Type::Release, held);
          if (!s.longSent) push(i, Type::Click, held);
        }
      }
      if (s.down && s.raw && !s.longSent &&
          uint32_t(now - s.pressedAt) >= longMs_) {
        s.longSent = true;
        push(i, Type::LongPress, now - s.pressedAt);
      }
    }
  }
  bool read(Event &event) {
    if (tail_ == head_) return false;
    event = queue_[tail_];
    tail_ = (tail_ + 1) & 7;
    return true;
  }
  bool pressed(uint8_t key) const { return key < 3 && states_[key].down; }
  uint16_t droppedEvents() const { return dropped_; }

 private:
  struct State {
    uint32_t rawSince = 0, pressedAt = 0;
    bool raw = false, down = false, longSent = false;
  };
  uint8_t pins_[3];
  State states_[3];
  Event queue_[8]; // Ring capacity is 7 events; overflow is observable.
  uint8_t head_ = 0, tail_ = 0;
  uint16_t debounceMs_, longMs_, dropped_ = 0;
  void push(uint8_t key, Type type, uint32_t held) {
    const uint8_t next = (head_ + 1) & 7;
    if (next == tail_) { ++dropped_; return; }
    queue_[head_] = Event{key, type, held};
    head_ = next;
  }
};
