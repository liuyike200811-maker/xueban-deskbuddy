#include <Arduino.h>

// Six WS2812 LEDs, data input on GPIO7.
constexpr uint8_t LedPin = 7;
constexpr uint8_t LedCount = 6;
constexpr size_t SymbolCount = LedCount * 24;
constexpr uint8_t HeadBrightness = 72;
constexpr uint8_t Tail1Brightness = 20;
constexpr uint8_t Tail2Brightness = 5;

rmt_data_t ledData[SymbolCount];

void addBit(size_t &symbol, bool one) {
  ledData[symbol].duration0 = one ? 8 : 4;
  ledData[symbol].level0 = 1;
  ledData[symbol].duration1 = one ? 4 : 8;
  ledData[symbol].level1 = 0;
  ++symbol;
}

void fillMarquee(uint8_t head) {
  size_t symbol = 0;
  for (uint8_t led = 0; led < LedCount; ++led) {
    uint8_t distance = (head + LedCount - led) % LedCount;
    uint8_t green = distance == 0 ? HeadBrightness : distance == 1 ? Tail1Brightness : distance == 2 ? Tail2Brightness : 0;
    uint8_t red = distance == 0 ? 8 : distance == 1 ? 3 : 0;
    uint8_t blue = 0;
    const uint8_t grb[3] = {green, red, blue};
    for (uint8_t color = 0; color < 3; ++color) {
      for (int8_t bit = 7; bit >= 0; --bit) {
        bool one = grb[color] & (1u << bit);
        addBit(symbol, one);
      }
    }
  }
}

void show() {
  if (!rmtWrite(LedPin, ledData, SymbolCount, RMT_WAIT_FOR_EVER)) {
    Serial.println("LED_RMT_WRITE_FAILED");
  }
}

void showOff() {
  size_t symbol = 0;
  for (uint8_t led = 0; led < LedCount; ++led) {
    for (uint8_t color = 0; color < 3; ++color) {
      for (uint8_t bit = 0; bit < 8; ++bit) addBit(symbol, false);
    }
  }
  show();
}

void setup() {
  Serial.begin(115200);
  if (!rmtInit(LedPin, RMT_TX_MODE, RMT_MEM_NUM_BLOCKS_1, 10000000)) {
    Serial.println("LED_RMT_INIT_FAILED");
    while (true) delay(1000);
  }
  showOff();
  Serial.printf("LED_MARQUEE_READY gpio=%u leds=%u\n", LedPin, LedCount);
}

void loop() {
  static uint8_t head = 0;
  fillMarquee(head);
  show();
  Serial.printf("LED_HEAD %u\n", head);
  head = (head + 1) % LedCount;
  delay(140);
}