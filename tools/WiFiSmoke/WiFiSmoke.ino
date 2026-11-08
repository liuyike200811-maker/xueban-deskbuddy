#include <Arduino.h>
#include <WiFi.h>

void setup() {
  Serial.begin(115200);
  WiFi.mode(WIFI_STA);
}

void loop() {
  // Passive scan: listen for beacons without joining nearby networks.
  int16_t count = WiFi.scanNetworks(false, false, true, 350);
  if (count < 0) {
    Serial.printf("WiFi scan failed: %d\n", count);
  } else {
    int32_t strongest = -127;
    for (int16_t i = 0; i < count; ++i) {
      strongest = max(strongest, WiFi.RSSI(i));
    }
    Serial.printf("WiFi scan: %d networks", count);
    if (count > 0) {
      Serial.printf(", strongest %ld dBm", static_cast<long>(strongest));
    }
    Serial.println();
    WiFi.scanDelete();
  }
  delay(3000);
}
