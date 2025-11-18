#pragma once
#include <stdint.h>

#ifdef ENABLE_DISPLAY
#include <Wire.h>
#include "drivers/display_ssd1306.h"
#include "../board_pins.h"

// Gestor de pantalla: encendido/apagado lógico + timeout + helpers
class DisplayMgr {
public:
  void begin(uint8_t i2c_addr, uint32_t i2c_hz);
  void on();                         // enciende lógicamente (clear + status)
  void off();                        // apaga lógicamente
  bool isOn() const { return _on; }

  // Aumenta el tiempo encendida (minutos), con tope (minutos)
  void bumpMinutes(uint32_t minutes, uint32_t max_minutes);

  // Debe llamarse en cada loop con millis()
  void tick(uint32_t now_ms);

  // Helpers de UI
  void showStatus(const char* l1, const char* l2);
  void showAltitude(float alt_m, const char* stateText);

  void setBleIndicator(bool on);

private:
  DisplaySSD1306 _disp{128, 32, &Wire, OLED_RESET_PIN};
  bool     _init_ok = false;
  bool     _on = false;
  uint32_t _off_deadline_ms = 0;
  uint8_t  _addr = 0x3C;
  uint32_t _i2c_hz = 400000;
  bool     _bleOn = false;
  bool     _bleBlinkState = false;
  uint32_t _bleBlinkLast = 0;

  float    _last_alt_m = 0.f;
  char     _last_state_text[24] = {0}; 
};

#endif // ENABLE_DISPLAY
