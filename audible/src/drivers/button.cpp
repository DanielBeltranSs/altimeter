#include "button.h"

void Button::begin(int pin, bool pullup) {
  _pin = pin; _pullup = pullup;
  if (_pullup) pinMode(_pin, INPUT_PULLUP);
  else         pinMode(_pin, INPUT);
  _last_raw = _pullup ? true : false;
  _last_change = 0;
  _pressed = false;
  _armed = false;
  _press_start = 0;
}

void Button::setTimings(uint32_t debounce_ms, uint32_t short_max_ms,
                        uint32_t long3_ms, uint32_t long8_ms) {
  _debounce_ms = debounce_ms;
  _short_max_ms = short_max_ms;
  _long3_ms = long3_ms;
  _long8_ms = long8_ms;
}

static inline bool _digitalReadBool(int pin) {
  // digitalRead devuelve HIGH/LOW; lo convertimos a bool
  return digitalRead(pin) == HIGH;
}

// Lee bruto y devuelve "true si nivel alto"
bool Button::_isPressedRaw() {
  bool raw_high = _digitalReadBool(_pin);
  // _pullup => activo-bajo: presionado cuando nivel es LOW (= !raw_high)
  return _pullup ? (!raw_high) : raw_high;
}

BtnEvent Button::poll(uint32_t now_ms) {
  if (_pin < 0) return BtnEvent::None;

  bool raw_high = _digitalReadBool(_pin);

  // Debounce por cambio de nivel
  if (raw_high != _last_raw) {
    _last_raw = raw_high;
    _last_change = now_ms;
  }
  if ((now_ms - _last_change) < _debounce_ms) return BtnEvent::None;

  // Interpretación estable tras debounce
  bool pressed = _pullup ? (!raw_high) : (raw_high);

  if (pressed && !_pressed) {
    // flanco bajada
    _pressed = true;
    _press_start = now_ms;
    _armed = true;
    return BtnEvent::None;
  }

  if (!pressed && _pressed) {
    // flanco subida => clasificar
    _pressed = false;
    if (!_armed) return BtnEvent::None;
    _armed = false;
    uint32_t dur = now_ms - _press_start;
    if (dur >= _long8_ms)     return BtnEvent::Long8;
    if (dur >= _long3_ms)     return BtnEvent::Long3;
    if (dur <= _short_max_ms) return BtnEvent::Short;
    return BtnEvent::None;
  }

  return BtnEvent::None;
}

// ====== Light sleep support ======

void Button::_setupSleepPulls() {
  // Asegura dirección/pulls también en sleep para evitar ruido
  gpio_sleep_set_direction((gpio_num_t)_pin, GPIO_MODE_INPUT);
  if (_pullup) {
    // activo-bajo: mantener línea en HIGH durante sleep
    gpio_sleep_set_pull_mode((gpio_num_t)_pin, GPIO_PULLUP_ONLY);
  } else {
    // activo-alto: mantener línea en LOW durante sleep
    gpio_sleep_set_pull_mode((gpio_num_t)_pin, GPIO_PULLDOWN_ONLY);
  }
}

void Button::enableGpioWakeForLightSleep() {
  // Configurar como input con el pull correspondiente en modo activo
  if (_pullup) {
    pinMode(_pin, INPUT_PULLUP);
  } else {
    pinMode(_pin, INPUT);
    // si activo-alto con pulldown interno deseado:
    // Arduino no ofrece INPUT_PULLDOWN en todos los cores;
    // usamos pulls de sleep y el pull externo si es necesario.
  }

  _setupSleepPulls();

  // Nivel que dispara el wake:
  // - activo-bajo → nivel bajo = pulsado
  // - activo-alto → nivel alto = pulsado
  gpio_int_type_t wake_level = _pullup ? GPIO_INTR_LOW_LEVEL : GPIO_INTR_HIGH_LEVEL;
  gpio_wakeup_enable((gpio_num_t)_pin, wake_level);

  // Habilitar GPIO wake como fuente global para light sleep
  esp_sleep_enable_gpio_wakeup();
}

BtnEvent Button::lightSleepWaitAndClassify(uint64_t max_us) {
  // Configurar timer wake si se usa timeout
  if (max_us > 0) {
    esp_sleep_enable_timer_wakeup(max_us);
  }

  // Pequeño guard para no dormir justo sobre un rebote
  vTaskDelay(pdMS_TO_TICKS(20));

  // Entrar a light sleep: retorna al producirse un wake
  esp_light_sleep_start();

  // ¿Qué causó el wake?
  esp_sleep_wakeup_cause_t cause = esp_sleep_get_wakeup_cause();
  if (cause != ESP_SLEEP_WAKEUP_GPIO) {
    return BtnEvent::None; // timer u otra causa
  }

  // ===== Debounce + medición de duración =====
  // Debounce inicial
  vTaskDelay(pdMS_TO_TICKS(_debounce_ms));

  auto pressedNow = [this](){ return _isPressedRaw(); };

  if (!pressedNow()) {
    // Rebote fugaz: nada que hacer
    return BtnEvent::None;
  }

  int64_t t0_us = esp_timer_get_time();
  // Medir mientras siga presionado
  while (pressedNow()) {
    vTaskDelay(pdMS_TO_TICKS(5));
  }
  int64_t dt_ms = (esp_timer_get_time() - t0_us) / 1000;

  // Clasificar con tus umbrales
  if ((uint32_t)dt_ms >= _long8_ms)     return BtnEvent::Long8;
  if ((uint32_t)dt_ms >= _long3_ms)     return BtnEvent::Long3;
  if ((uint32_t)dt_ms <= _short_max_ms) return BtnEvent::Short;
  return BtnEvent::None;
}
