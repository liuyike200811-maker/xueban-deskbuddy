#pragma once
#include <stdint.h>

enum class Mode : uint8_t { Idle, Focus, Rest };
enum class Gesture : uint8_t { None, Single, Double, Long };

class Timer {
  Mode mode_ = Mode::Idle;
  uint32_t end_ = 0;
  uint32_t total_ = 0;
public:
  Mode mode() const { return mode_; }
  uint32_t total() const { return total_; }
  bool start(Mode mode, uint32_t seconds, uint32_t now) {
    if (mode == Mode::Idle || seconds < 1 || seconds > 7200) return false;
    mode_ = mode;
    end_ = now + seconds * 1000;
    total_ = seconds;
    return true;
  }
  void stop() { mode_ = Mode::Idle; }
  uint32_t remaining(uint32_t now) const {
    int32_t ms = static_cast<int32_t>(end_ - now);
    return mode_ != Mode::Idle && ms > 0 ? (ms + 999) / 1000 : 0;
  }
  Mode tick(uint32_t now) {
    if (remaining(now)) return Mode::Idle;
    Mode finished = mode_;
    stop();
    return finished;
  }
};

class Touch {
  static constexpr uint32_t DebounceMs = 15, DoubleClickMs = 450, LongPressMs = 800;
  bool raw_ = false, down_ = false, held_ = false, second_ = false, pending_ = false;
  uint32_t changed_ = 0, pressed_ = 0, released_ = 0;
public:
  Gesture update(bool raw, uint32_t now) {
    if (raw != raw_) { raw_ = raw; changed_ = now; }
    if (raw_ != down_ && now - changed_ >= DebounceMs) {
      down_ = raw_;
      if (down_) {
        second_ = pending_ && changed_ - released_ <= DoubleClickMs;
        pressed_ = now;
        held_ = false;
      } else if (!held_) {
        if (second_) { pending_ = false; return Gesture::Double; }
        pending_ = true;
        released_ = now;
      }
    }
    if (down_ && !held_ && now - pressed_ >= LongPressMs) {
      held_ = true;
      pending_ = false;
      return Gesture::Long;
    }
    if (pending_ && !down_ && !raw_ && now - released_ >= DoubleClickMs) {
      pending_ = false;
      return Gesture::Single;
    }
    return Gesture::None;
  }
};