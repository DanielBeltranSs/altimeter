#include "display_mgr.h"

#ifdef ENABLE_DISPLAY

// ========= Helper de compatibilidad =========
//
// Si tu driver de bajo nivel (_disp) ofrece un método extendido
//   renderAltitude(float alt_m, const char* state, bool bleVisible)
// lo usaremos para dibujar la “B” junto a la unidad.
// Si no existe, caemos a showAltitude(alt_m, state).
//
// Esto evita romper builds existentes.
namespace {
  // SFINAE: detecta si T tiene renderAltitude(float,const char*,bool)
  template<typename T>
  class HasRenderAltitude {
    typedef char Yes[1];
    typedef char No[2];
    template<typename C>
    static auto test(C*) -> decltype( std::declval<C>().renderAltitude(0.f,(const char*)nullptr,false), std::true_type{} );
    template<typename C>
    static auto test(...) -> std::false_type;
  public:
    static constexpr bool value = decltype(test<T>(nullptr))::value;
  };

  template<typename T>
  inline typename std::enable_if< HasRenderAltitude<T>::value, void >::type
  renderAltitudeCompat(T& d, float alt_m, const char* state, bool bleVisible) {
    d.renderAltitude(alt_m, state, bleVisible);
  }

  template<typename T>
  inline typename std::enable_if<!HasRenderAltitude<T>::value, void>::type
  renderAltitudeCompat(T& d, float alt_m, const char* state, bool /*bleVisible*/) {
    // Fallback: sin “B”
    d.showAltitude(alt_m, state);
  }
}

// ========= Implementación DisplayMgr =========

void DisplayMgr::begin(uint8_t i2c_addr, uint32_t i2c_hz) {
  _addr = i2c_addr; _i2c_hz = i2c_hz;
  _init_ok = _disp.begin(_addr, _i2c_hz);
  _on = false;
  _off_deadline_ms = 0;

  // Estado BLE UI
  _bleOn = false;
  _bleBlinkState = false;
  _bleBlinkLast = 0;

  // Caché de último frame para re-render rápido en blink
  _last_alt_m = 0.f;
  _last_state_text[0] = '\0';

  if (_init_ok) _disp.powerOff();   // asegura panel off al boot
}

void DisplayMgr::on() {
  if (!_init_ok) return;
  if (!_on) {
    _on = true;
    _disp.powerOn();                // enciende el panel
    _disp.clear();
    _disp.showStatus("DISPLAY", "ON");
    // fuerza un render en el siguiente showAltitude
    _last_state_text[0] = '\0';
  }
}

void DisplayMgr::off() {
  if (!_init_ok) return;
  if (_on) {
    _disp.clear();
    _disp.powerOff();               // apaga el panel
    _on = false;
    _off_deadline_ms = 0;
  }
}

void DisplayMgr::bumpMinutes(uint32_t minutes, uint32_t max_minutes) {
  if (!_init_ok) return;
  on();
  uint32_t add_ms = minutes * 60UL * 1000UL;
  uint32_t now = millis();
  uint32_t remaining = (_off_deadline_ms > now) ? (_off_deadline_ms - now) : 0;
  uint32_t new_total = remaining + add_ms;
  uint32_t max_total = max_minutes * 60UL * 1000UL;
  if (new_total > max_total) new_total = max_total;
  _off_deadline_ms = now + new_total;
}

void DisplayMgr::tick(uint32_t now_ms) {
  if (!_on) return;

  // Auto-off
  if (_off_deadline_ms && now_ms >= _off_deadline_ms) {
    off();
    return;
  }

  // Blink B cada 500 ms si BLE activo
  if (_bleOn && (now_ms - _bleBlinkLast) >= 500) {
    _bleBlinkLast = now_ms;
    _bleBlinkState = !_bleBlinkState;

    // Re-render rápido usando el último alt/state guardado
    if (_last_state_text[0] != '\0') {
      char stateBuf[sizeof(_last_state_text) + 3]; // espacio para " B"
      if (_bleOn && _bleBlinkState) {
        snprintf(stateBuf, sizeof(stateBuf), "%s B", _last_state_text);
      } else {
        snprintf(stateBuf, sizeof(stateBuf), "%s", _last_state_text);
      }
      _disp.showAltitude(_last_alt_m, stateBuf);
    }
  }
}

void DisplayMgr::showStatus(const char* l1, const char* l2) {
  if (_init_ok && _on) _disp.showStatus(l1, l2);
}

void DisplayMgr::showAltitude(float alt_m, const char* stateText) {
  if (!(_init_ok && _on)) return;

  _last_alt_m = alt_m;

  // Guarda copia del estado base (sin “B”)
  if (stateText) {
    strncpy(_last_state_text, stateText, sizeof(_last_state_text)-1);
    _last_state_text[ sizeof(_last_state_text)-1 ] = '\0';
  } else {
    _last_state_text[0] = '\0';
  }

  // Construye el texto que se mostrará en este frame (con o sin “B”)
  char stateBuf[sizeof(_last_state_text) + 3];
  if (_bleOn && _bleBlinkState) {
    snprintf(stateBuf, sizeof(stateBuf), "%s B", _last_state_text);
  } else {
    snprintf(stateBuf, sizeof(stateBuf), "%s", _last_state_text);
  }

  _disp.showAltitude(alt_m, stateBuf);
}

void DisplayMgr::setBleIndicator(bool on) {
  if (!_init_ok) return;

  if (on != _bleOn) {
    _bleOn = on;
    // Reinicia el parpadeo al activarse
    if (_bleOn) {
      _bleBlinkState = true;
      _bleBlinkLast = 0;
      // Forzamos un re-render inmediato si ya hay un frame almacenado
      if (_on && _last_state_text[0] != '\0') {
        renderAltitudeCompat(_disp, _last_alt_m, _last_state_text, _bleBlinkState);
      }
    } else {
      // Al desactivar BLE, re-render sin “B”
      if (_on && _last_state_text[0] != '\0') {
        renderAltitudeCompat(_disp, _last_alt_m, _last_state_text, false);
      }
    }
  }
}

#endif // ENABLE_DISPLAY
