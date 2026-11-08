#include <Arduino.h>
#include <ESP_I2S.h>
#include <math.h>

I2SClass mic;
int32_t samples[256];

void setup() {
  Serial.begin(115200);
  mic.setPins(10, 11, -1, 12);
  if (!mic.begin(I2S_MODE_STD, 16000, I2S_DATA_BIT_WIDTH_32BIT,
                 I2S_SLOT_MODE_MONO, I2S_STD_SLOT_LEFT)) {
    while (true) {
      Serial.println("INMP441: I2S initialization failed");
      delay(1000);
    }
  }
}

void loop() {
  // About 256 ms per report; remove DC offset from the RMS reading.
  double sum = 0, squares = 0;
  size_t count = 0;
  for (int block = 0; block < 16; ++block) {
    size_t bytes = mic.readBytes(reinterpret_cast<char *>(samples), sizeof(samples));
    if (bytes != sizeof(samples)) {
      Serial.printf("INMP441: short read (%u bytes)\n", static_cast<unsigned>(bytes));
      delay(250);
      return;
    }
    for (int32_t word : samples) {
      double sample = word >> 8;
      sum += sample;
      squares += sample * sample;
      ++count;
    }
  }
  double mean = sum / count;
  double variance = squares / count - mean * mean;
  Serial.printf("INMP441 relative RMS: %.1f\n", sqrt(fmax(0.0, variance)));
}
