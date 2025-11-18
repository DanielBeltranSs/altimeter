#pragma once
#include <Arduino.h>

class Buzzer {
public:
  void begin(int pin, bool active_high = true) {
    _pin = pin; _active_high = active_high;
    pinMode(_pin, OUTPUT);
    off();
  }

  // Bloqueantes cortos (simples y robustos)
  void playCalibrationOk() {
    // beep-beep (2 x 80 ms con 70 ms de pausa)
    beep(80); delay(70); beep(80);
  }

  void playBleEnabled() {
    // beep largo (200 ms), pausa 120 ms, beep corto (90 ms)
    beep(200); delay(120); beep(90);
  }

  void off() { digitalWrite(_pin, _active_high ? LOW : HIGH); }

private:
  void beep(uint16_t ms) {
    digitalWrite(_pin, _active_high ? HIGH : LOW);
    delay(ms);
    digitalWrite(_pin, _active_high ? LOW : HIGH);
  }

  int  _pin = -1;
  bool _active_high = true;
};
