#include <Arduino.h>
#include <HTTPClient.h>
#include <WiFi.h>

void setup() {
  Serial.begin(115200);
  Serial.setTimeout(1500);
  WiFi.persistent(false);
  WiFi.mode(WIFI_STA);
}

void loop() {
  if (!Serial.available()) {
    delay(20);
    return;
  }

  String ssid = Serial.readStringUntil('\n');
  String password = Serial.readStringUntil('\n');
  if (ssid.isEmpty() || password.isEmpty()) {
    Serial.println("INPUT_INCOMPLETE");
    return;
  }

  for (int attempt = 0; attempt < 2; ++attempt) {
    int16_t count = WiFi.scanNetworks(false, false, true, 450);
    for (int16_t i = 0; i < count; ++i) {
      if (WiFi.SSID(i) == ssid) {
        Serial.printf("TARGET_2G channel=%ld rssi=%ld\n",
                      static_cast<long>(WiFi.channel(i)),
                      static_cast<long>(WiFi.RSSI(i)));
      }
    }
    WiFi.scanDelete();
  }

  WiFi.begin(ssid.c_str(), password.c_str());
  const unsigned long start = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - start < 20000) {
    delay(100);
  }

  if (WiFi.status() != WL_CONNECTED) {
    Serial.printf("JOIN_FAILED status=%d\n", WiFi.status());
    WiFi.disconnect();
    return;
  }

  Serial.printf("JOIN_OK channel=%ld rssi=%ld ip=%s gateway=%s\n",
                static_cast<long>(WiFi.channel()), static_cast<long>(WiFi.RSSI()),
                WiFi.localIP().toString().c_str(), WiFi.gatewayIP().toString().c_str());
  HTTPClient probe;
  probe.setConnectTimeout(5000);
  probe.setTimeout(5000);
  probe.begin("http://connectivitycheck.gstatic.com/generate_204");
  Serial.printf("INTERNET_HTTP %d\n", probe.GET());
  probe.end();
}
