#pragma once
#ifdef ENABLE_DISPLAY

#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>

class DisplaySSD1306 {
public:
  DisplaySSD1306(uint8_t w=128, uint8_t h=32, TwoWire* bus=&Wire, int8_t rst=-1)
  : _oled(w, h, bus, rst), _bus(bus) {}

  bool begin(uint8_t i2cAddr, uint32_t i2cHz);
  void powerOn();
  void powerOff();
  void showAltitude(float alt_m, const char* stateText);
  void showStatus(const char* line1, const char* line2);
  void clear();

private:
  Adafruit_SSD1306 _oled;
  TwoWire* _bus;
};

#endif
