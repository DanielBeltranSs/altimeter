#pragma once
#include <Arduino.h>

// Nota: añade estas cabeceras (IDF) en el .h porque las usamos en la firma nueva
#include "driver/gpio.h"
#include "esp_sleep.h"
#include "esp_timer.h"

enum class BtnEvent : uint8_t {
  None = 0,
  Short,
  Long3,
  Long8
};

class Button {
public:
  void begin(int pin, bool pullup);
  void setTimings(uint32_t debounce_ms, uint32_t short_max_ms,
                  uint32_t long3_ms, uint32_t long8_ms);

  // Poll clásico para uso en modo activo (tu implementación original)
  BtnEvent poll(uint32_t now_ms);

  // ===== NUEVO: soporte light-sleep =====
  // Configura el pin como fuente de wake para light sleep
  // Llama a esto una vez en setup() tras begin().
  void enableGpioWakeForLightSleep();

  // Entra en light sleep y vuelve con un evento clasificado si el wake fue el botón.
  // max_us: timeout opcional de temporizador (0 = sin timeout).
  // Devuelve BtnEvent::None si despertó por timer u otra causa o si la pulsación no cruzó umbrales.
  BtnEvent lightSleepWaitAndClassify(uint64_t max_us);

  // Helpers
  inline bool isPullup() const { return _pullup; }
  inline int  pin() const { return _pin; }

private:
  // Estado original
  int _pin = -1;
  bool _pullup = true;
  bool _last_raw = true;
  uint32_t _last_change = 0;
  bool _pressed = false;
  bool _armed = false;
  uint32_t _press_start = 0;

  // Timings
  uint32_t _debounce_ms = 30;
  uint32_t _short_max_ms = 800;
  uint32_t _long3_ms = 3000;
  uint32_t _long8_ms = 5000;

  // Internos
  bool _isPressedRaw();       // lee nivel y aplica convención activo-bajo/alto
  void _setupSleepPulls();    // asegura pulls en modo sleep
};
