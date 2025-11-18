#pragma once
#include <stdint.h>

enum class FlightMode : uint8_t {
  GROUND = 0,
  CLIMB,
  FREEFALL,
  CANOPY
};

struct FlightModeConfig {
  // Umbrales (m/s) y altitudes (m AGL) para detección
  float climb_vz_enter     = +0.6f;   // entrar a CLIMB si vz > +0.6 m/s
  float climb_vz_exit      = +0.15f;  // volver a GROUND si |vz| < +0.15 m/s (histeresis)

  float ff_vz_enter        = -15.0f;  // entrar a FREEFALL si vz < -15 m/s
  float ff_vz_exit         = -9.0f;   // salir de FREEFALL si vz > -9 m/s

  float canopy_vz_enter    = -6.0f;   // entrar a CANOPY si vz > -6 m/s (después de FF)
  float canopy_vz_exit     = -10.0f;  // volver a FF si cae más rápido otra vez

  float canopy_min_alt_agl = 150.0f;  // no CANOPY si aún demasiado alto (seguridad simple)

  // Ventana para considerar “cerca del suelo” (AGL)
  float ground_window_agl  = 2.0f;    // ±2 m alrededor de la base

  // Duraciones mínimas (ms) para confirmar transiciones
  uint32_t min_dwell_ground  = 3000;  // regresar a GROUND: condición mantenida ≥ 3 s
  uint32_t min_dwell_climb   = 2000;  // entrar a CLIMB: condición mantenida ≥ 2 s
  uint32_t min_dwell_ff      = 500;   // FF entra rápido
  uint32_t min_dwell_canopy  = 2000;

  // Filtro EMA para vz (suaviza decisiones)
  float vz_alpha = 0.20f;
};

class FlightModeDetector {
public:
  void begin(float ground_alt_m, const FlightModeConfig& cfg = FlightModeConfig());
  // Llama en cada muestra con altitud (m) y timestamp (ms). Devuelve modo actual.
  FlightMode update(float alt_m, uint32_t now_ms);

  FlightMode mode() const { return _mode; }
  float vz_mps() const { return _vz_ema; }         // velocidad vertical filtrada (m/s)
  float groundAlt() const { return _ground_alt; }  // referencia AGL
  void shiftGroundBase(float delta_m);


private:
  FlightMode _mode = FlightMode::GROUND;
  FlightModeConfig _cfg{};
  float _ground_alt = 0.0f; // altitud “0” en despegue
  float _prev_alt = 0.0f;
  uint32_t _prev_ms = 0;
  uint32_t _enter_ts = 0;   // tiempo de entrada al modo actual
  float _vz_ema = 0.0f;

  // Dwell a nivel de modo (tiempo mínimo en el modo actual)
  bool dwellOK(uint32_t now, uint32_t min_dwell) const { return (now - _enter_ts) >= min_dwell; }
};