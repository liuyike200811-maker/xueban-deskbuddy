#pragma once
#include <Wire.h>
#include "Control.h"

enum class PetMood : uint8_t { Idle, Hearing, Thinking, Speaking, Error, Sleeping, Happy };

class DeskDisplay {
  uint8_t address_ = 0;
  uint8_t frame_[1024]{};
  uint32_t nextIdle_ = 0, idleUntil_ = 0, nextFrame_ = 0;
  uint8_t idleMove_ = 0;
  uint32_t hopStarted_ = 0;
  bool hopping_ = false;

  void box(int x, int y, int w, int h) {
    for (int row = y; row < y + h; ++row)
      for (int col = x; col < x + w; ++col)
        frame_[(row / 8) * 128 + col] |= 1 << (row % 8);
  }

  void pet(PetMood mood, uint32_t now, int hop) {
    auto draw = [this, hop](int x, int y, int w, int h) {
      int left = x * 3 / 2, top = y * 5 / 4;
      box(24 + left, top + 9 - hop, (x + w) * 3 / 2 - left, (y + h) * 5 / 4 - top);
    };
    int glance = mood == PetMood::Thinking ? 4 : mood == PetMood::Idle && idleMove_ == 2 ? -4 :
      mood == PetMood::Idle && idleMove_ == 3 ? 4 : 0;
    bool blink = mood == PetMood::Idle && idleMove_ == 1;
    // Pixel crab: low, stepped shell, two claws and four little feet.
    draw(13,10,27,2); draw(10,12,3,3); draw(40,12,3,3);
    draw(8,15,2,20); draw(43,15,2,20);
    draw(10,35,33,2);
    for (int foot : {12,20,30,38}) draw(foot,37,3,5);
    int lift = mood == PetMood::Happy ? 2 + hop : mood == PetMood::Hearing ? 5 :
      mood == PetMood::Speaking ? int((now / 110) % 3)*2 :
      mood == PetMood::Idle && idleMove_ == 4 ? 6 : 0;
    draw(2,26-lift,6,3+lift);
    draw(45,26-lift,6,3+lift);
    draw(1,20-lift,2,7); draw(5,21-lift,2,6);
    draw(46,21-lift,2,6); draw(50,20-lift,2,7);
    int eyeY = 19+(mood == PetMood::Hearing ? -2 : 0);
    for (int ex : {14+glance, 31+glance}) {
      if (blink || mood == PetMood::Error || mood == PetMood::Sleeping) draw(ex,eyeY+3,7,2);
      else if (mood == PetMood::Happy) {
        draw(ex,eyeY+4,2,3); draw(ex+2,eyeY+2,3,2); draw(ex+5,eyeY+4,2,3);
      }
      else {
        draw(ex+1,eyeY,5,9); draw(ex,eyeY+2,7,5);
      }
    }
    if (mood == PetMood::Speaking) {
      int h = 2 + int((now / 110) % 3) * 2;
      draw(23,33-h/2,8,h);
    } else if (mood == PetMood::Thinking) {
      draw(25,33,5,2);
    } else {
      draw(22,31,2,2); draw(24,33,6,2); draw(30,31,2,2);
    }
  }

  bool send(uint8_t prefix, const uint8_t *bytes, size_t count) {
    Wire.beginTransmission(address_);
    Wire.write(prefix);
    Wire.write(bytes, count);
    return Wire.endTransmission() == 0;
  }

  void label(int x, int y, const char *s, int scale) {
    static constexpr char keys[] = "0123456789:";
    static constexpr uint8_t glyph[][5] = {
      {62,81,73,69,62},{0,66,127,64,0},{66,97,81,73,70},{33,65,69,75,49},
      {24,20,18,127,16},{39,69,69,69,57},{60,74,73,73,48},{1,113,9,5,3},
      {54,73,73,73,54},{6,73,73,41,30},{0,54,54,0,0}
    };
    for (; *s; ++s, x += 6 * scale) {
      const char *key = strchr(keys, *s);
      if (!key) continue;
      for (int col = 0; col < 5 * scale; ++col)
        for (int row = 0; row < 7 * scale; ++row)
          if (glyph[key - keys][col / scale] & (1 << (row / scale))) {
            int py = y + row, px = x + col;
            frame_[(py / 8) * 128 + px] |= 1 << (py % 8);
          }
    }
  }
public:
  bool begin() {
    if (!Wire.begin(8, 9, 400000)) return false;
    Wire.setTimeOut(10);
    const uint8_t init[] = {0xAE,0xD5,0x80,0xA8,0x3F,0xD3,0,0x40,0x8D,0x14,
      0x20,0,0xA1,0xC8,0xDA,0x12,0x81,0x5F,0xD9,0xF1,0xDB,0x40,0xA4,0xA6,0xAF};
    for (uint8_t address : {0x3C, 0x3D}) {
      address_ = address;
      if (send(0, init, sizeof(init))) {
        Serial.printf("OLED_READY 0x%02X\n", address_);
        return true;
      }
    }
    address_ = 0;
    return false;
  }

  void update(Mode mode, uint32_t seconds, uint32_t total, PetMood mood, bool overtime, bool playing) {
    uint32_t now = millis();
    if (!address_ || int32_t(now - nextFrame_) < 0) return;
    nextFrame_ = now + 80;
    if (playing && !hopping_) hopStarted_ = now;
    hopping_ = playing;
    constexpr uint8_t heights[] = {0, 3, 5, 5, 3, 0, 0, 0};
    int hop = playing ? heights[((now - hopStarted_) / 80) % 8] : 0;
    if (idleMove_ && int32_t(now - idleUntil_) >= 0) idleMove_ = 0;
    if (mood == PetMood::Idle && int32_t(now - nextIdle_) >= 0) {
      idleMove_ = random(1, 5);
      idleUntil_ = now + (idleMove_ == 1 ? 200 : 720);
      nextIdle_ = now + random(2200, 5500);
    }
    uint32_t pose = mood == PetMood::Speaking ? (now / 110) % 3 :
      mood == PetMood::Thinking || mood == PetMood::Hearing ? (now / 240) % 3 : idleMove_;
    uint32_t framePose = pose | (uint32_t(hop) << 8);
    if (mode == mode_ && seconds == seconds_ && total == total_ && mood == mood_ &&
        overtime == overtime_ && framePose == pose_) return;
    mode_ = mode; seconds_ = seconds; total_ = total; mood_ = mood; overtime_ = overtime; pose_ = framePose;
    memset(frame_, 0, sizeof(frame_));
    pet(mood, now, hop);
    if (mode != Mode::Idle) {
      char time[8];
      snprintf(time, sizeof(time), "%02u:%02u", unsigned(seconds / 60), unsigned(seconds % 60));
      label((130 - int(strlen(time))*12)/2,0,time,2);
      int ground = total ? seconds * 46 / total : 0;
      if (ground) box((128-ground)/2,15,ground,1);
    } else if (overtime) {
      box(62,3,4,6); box(62,11,4,2);
    }
    if (mood == PetMood::Hearing || mood == PetMood::Thinking) {
      for (unsigned i = 0; i <= pose; ++i) box(107+i*6,35-i*3,3,3);
    }
    const uint8_t window[] = {0x21,0,127,0x22,0,7};
    bool ok = send(0, window, sizeof(window));
    for (size_t i = 0; ok && i < sizeof(frame_); i += 64) ok = send(0x40, frame_ + i, 64);
    if (!ok) Serial.println("OLED_WRITE_FAILED");
  }
private:
  Mode mode_ = Mode::Idle;
  uint32_t seconds_ = UINT32_MAX;
  uint32_t total_ = 0;
  bool overtime_ = false;
  PetMood mood_ = PetMood::Idle;
  uint32_t pose_ = UINT32_MAX;
};
