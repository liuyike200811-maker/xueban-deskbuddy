#include <Arduino.h>
#include <ESP_I2S.h>
#include "Clip.h"

constexpr unsigned ClipRate = 24000, OutputRate = 48000;
I2SClass speaker(I2S_NUM_1);
bool previousLevel;
size_t cursor = 0;
bool playing = false;
bool ready = false;

void report(const char *state) {
  Serial.printf("SPEAKER %s %lu/%lu ms\n", state,
                static_cast<unsigned long>(cursor * 1000 / ClipRate),
                static_cast<unsigned long>(ClipSamples * 1000 / ClipRate));
}

void setup() {
  Serial.begin(921600);
  pinMode(4, INPUT);
  previousLevel = digitalRead(4) == HIGH;
  speaker.setPins(13, 14, 15);
  ready = speaker.begin(I2S_MODE_STD, OutputRate, I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO);
  report(ready ? "READY" : "I2S_ERROR");
}

void loop() {
  bool level = digitalRead(4) == HIGH;
  bool pressed = level && !previousLevel;
  if (level != previousLevel) {
    Serial.printf("TOUCH GPIO4 %u\n", level ? 1 : 0);
    previousLevel = level;
  }
  bool serialTest = Serial.available() && Serial.read() == 'p';
  if (pressed || serialTest) {
    if (!ready) {
      report("I2S_ERROR");
    } else {
      playing = !playing;
      if (playing && cursor == ClipSamples) cursor = 0;
      report(playing ? "PLAY" : "PAUSE");
    }
  }

  if (!playing) {
    delay(3);
    return;
  }

  // Keep writes short so the touch detector still samples while playing.
  int16_t stereo[240 * 4];
  size_t n = min(size_t(240), ClipSamples - cursor);
  for (size_t i = 0; i < n; ++i) {
    int32_t stored = static_cast<int16_t>(pgm_read_word(&Clip[cursor + i]));
    int16_t sample = static_cast<int16_t>(stored * 32767 / 3200);
    stereo[4 * i] = stereo[4 * i + 1] = stereo[4 * i + 2] = stereo[4 * i + 3] = sample;
  }
  if (speaker.write(stereo, n * 8) != n * 8) {
    playing = false;
    report("WRITE_ERROR");
    return;
  }
  cursor += n;
  if (cursor == ClipSamples) {
    playing = false;
    report("END");
  }
}

