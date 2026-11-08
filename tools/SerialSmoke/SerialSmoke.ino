#include <Arduino.h>

void setup() {
  Serial.begin(115200);
}

void loop() {
  Serial.printf("DeskBuddy ESP32-S3 | uptime=%lu ms\n", millis());
  delay(1000);
}
