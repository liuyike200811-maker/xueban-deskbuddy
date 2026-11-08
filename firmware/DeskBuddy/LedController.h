#pragma once
#include <Arduino.h>
#include <atomic>
#include <cstring>
#include "Control.h"

class LedController {
public:
  static constexpr uint8_t Pin = 7;
  static constexpr uint8_t Count = 6;

  bool begin() {
    ready_ = rmtInit(Pin, RMT_TX_MODE, RMT_MEM_NUM_BLOCKS_1, 10000000);
    if (ready_) renderFrame(nowFrame_);
    return ready_;
  }

  void signal(const char *event) {
    uint8_t next = static_cast<uint8_t>(Cue::None);
    if (!strcmp(event, "listening")) next = static_cast<uint8_t>(Cue::Listening);
    else if (!strcmp(event, "hearing") || !strcmp(event, "thinking") || !strcmp(event, "speaking"))
      next = static_cast<uint8_t>(Cue::Active);
    else if (!strcmp(event, "error")) next = static_cast<uint8_t>(Cue::Error);
    cue_.store(next, std::memory_order_relaxed);
  }

  void complete(uint32_t now) { completionStarted_ = now; }

  void update(bool active, bool networkLost, uint32_t now) {
    active_ = active;
    networkLost_ = networkLost;
    nowFrame_ = now;
    if (!ready_ || (lastFrame_ && now - lastFrame_ < 33)) return;
    lastFrame_ = now;
    renderFrame(now);
  }

private:
  enum class Cue : uint8_t { None, Listening, Active, Error };
  struct Rgb { uint8_t r, g, b; };

  rmt_data_t symbols_[Count * 24];
  size_t symbolCount_ = 0;
  std::atomic<uint8_t> cue_{static_cast<uint8_t>(Cue::None)};
  bool ready_ = false;
  bool networkLost_ = false;
  bool active_ = false;
  uint32_t completionStarted_ = 0;
  uint32_t lastFrame_ = 0;
  uint32_t nowFrame_ = 0;
  Rgb chase_[Count] = {};
  uint32_t chaseStep_ = 0;
  bool chasing_ = false;

  void addBit(bool one) {
    symbols_[symbolCount_].duration0 = one ? 8 : 4;
    symbols_[symbolCount_].level0 = 1;
    symbols_[symbolCount_].duration1 = one ? 4 : 8;
    symbols_[symbolCount_].level1 = 0;
    ++symbolCount_;
  }

  void send(const Rgb leds[Count]) {
    symbolCount_ = 0;
    for (uint8_t i = 0; i < Count; ++i) {
      const uint8_t grb[3] = {leds[i].g, leds[i].r, leds[i].b};
      for (uint8_t channel = 0; channel < 3; ++channel) {
        for (int8_t bit = 7; bit >= 0; --bit) addBit(grb[channel] & (1u << bit));
      }
    }
    if (!rmtWrite(Pin, symbols_, Count * 24, RMT_WAIT_FOR_EVER)) Serial.println("LED_WRITE_FAILED");
  }

  void idleFrame(Rgb leds[Count], uint32_t now) {
    uint32_t phase = now % 1400;
    uint8_t green = phase < 120 ? 36 : phase >= 220 && phase < 340 ? 20 : 3;
    for (auto i = 0; i < Count; ++i) leds[i] = {0, green, 0};
  }

  void chaseFrame(Rgb leds[Count], uint32_t now) {
    constexpr uint32_t stepMs = 120;
    if (!chasing_) {
      for (auto &color : chase_) color = {};
    }
    if (!chasing_ || now - chaseStep_ >= stepMs) {
      constexpr Rgb palette[] = {{42, 0, 0}, {34, 14, 0}, {26, 26, 0}, {0, 38, 0},
                                 {0, 27, 28}, {0, 7, 42}, {24, 0, 38}, {38, 0, 19}};
      for (uint8_t i = Count - 1; i > 0; --i) chase_[i] = chase_[i - 1];
      chase_[0] = palette[random(sizeof(palette) / sizeof(palette[0]))];
      chaseStep_ = now;
      chasing_ = true;
    }
    for (uint8_t i = 0; i < Count; ++i) leds[i] = chase_[i];
    uint32_t level = stepMs - (now - chaseStep_) / 2;
    leds[0] = {uint8_t(leds[0].r * level / stepMs), uint8_t(leds[0].g * level / stepMs),
               uint8_t(leds[0].b * level / stepMs)};
  }

  void errorFrame(Rgb leds[Count], uint32_t now) {
    uint32_t phase = now % 1400;
    uint8_t red = phase < 120 ? 36 : phase >= 220 && phase < 340 ? 20 : 0;
    for (uint8_t i = 0; i < Count; ++i) leds[i] = {red, 0, 0};
  }

  void renderFrame(uint32_t now) {
    Rgb leds[Count] = {};
    Cue cue = static_cast<Cue>(cue_.load(std::memory_order_relaxed));
    bool voiceError = networkLost_ || cue == Cue::Error;
    bool active = active_ || cue == Cue::Active || (completionStarted_ && now - completionStarted_ < 2600);
    if (voiceError || !active) chasing_ = false;
    if (voiceError) {
      errorFrame(leds, now);
    } else if (active) {
      chaseFrame(leds, now);
    } else {
      idleFrame(leds, now);
    }
    send(leds);
  }
};
