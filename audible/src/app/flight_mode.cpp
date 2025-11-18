#include "flight_mode.h"
#include <math.h>

void FlightModeDetector::begin(float ground_alt_m, const FlightModeConfig& cfg) {
  _cfg = cfg;
  _ground_alt = ground_alt_m;
  _prev_alt = ground_alt_m;
  _prev_ms = 0;
  _enter_ts = 0;
  _mode = FlightMode::GROUND;
  _vz_ema = 0.0f;
}

FlightMode FlightModeDetector::update(float alt_m, uint32_t now_ms) {
  if (_prev_ms == 0) { // primera muestra
    _prev_ms = now_ms;
    _prev_alt = alt_m;
    _enter_ts = now_ms;
    return _mode;
  }

  // ---- Cálculo de velocidad vertical (m/s) ----
  const float dt = (now_ms - _prev_ms) / 1000.0f;
  float vz = 0.0f;
  if (dt > 0.0f)
    vz = (alt_m - _prev_alt) / dt;

  // DEADband: ignora pequeñas variaciones por ruido
  if (fabsf(vz) < 0.05f)
    vz = 0.0f;

  // EMA sobre vz para suavizar
  _vz_ema = _cfg.vz_alpha * vz + (1.0f - _cfg.vz_alpha) * _vz_ema;

  // ---- Altitud sobre el punto de referencia (AGL) ----
  const float agl = alt_m - _ground_alt;

  // ---- FSM: Transiciones con histéresis y dwell ----
  switch (_mode) {

    // ===================================================
    case FlightMode::GROUND: {
      // Entrar a CLIMB: velocidad sostenida positiva durante dwell
      if (_vz_ema > _cfg.climb_vz_enter &&
          dwellOK(now_ms, _cfg.min_dwell_climb)) {
        _mode = FlightMode::CLIMB;
        _enter_ts = now_ms;
      }
      break;
    }

    // ===================================================
    case FlightMode::CLIMB: {
      // ===== Regreso estable a GROUND (histeresis + dwell + cerca de base) =====
      {
        bool low_speed = (fabsf(_vz_ema) < _cfg.climb_vz_exit);
        bool near_base = (fabsf(agl) < _cfg.ground_window_agl);
        static uint32_t hold_start_ms = 0;

        if (low_speed && near_base) {
          if (hold_start_ms == 0)
            hold_start_ms = now_ms;
          if ((now_ms - hold_start_ms) >= _cfg.min_dwell_ground) {
            _mode = FlightMode::GROUND;
            _enter_ts = now_ms;
            hold_start_ms = 0;
            break;
          }
        } else {
          hold_start_ms = 0;
        }
      }

      // Transición a FREEFALL si vz_ema cambia de signo bruscamente (sube → cae)
      if (_vz_ema < _cfg.ff_vz_enter &&
          dwellOK(now_ms, _cfg.min_dwell_climb)) {
        _mode = FlightMode::FREEFALL;
        _enter_ts = now_ms;
      }
      break;
    }

    // ===================================================
    case FlightMode::FREEFALL: {
      // Salida a CANOPY cuando se desacelera y altura > mínimo
      if (_vz_ema > _cfg.canopy_vz_enter &&
          agl > _cfg.canopy_min_alt_agl &&
          dwellOK(now_ms, _cfg.min_dwell_ff)) {
        _mode = FlightMode::CANOPY;
        _enter_ts = now_ms;
      }
      break;
    }

    // ===================================================
    case FlightMode::CANOPY: {
      // Reentrada a FREEFALL si vuelve a caer rápido
      if (_vz_ema < _cfg.canopy_vz_exit &&
          dwellOK(now_ms, _cfg.min_dwell_canopy)) {
        _mode = FlightMode::FREEFALL;
        _enter_ts = now_ms;
      }
      // Regreso a GROUND si velocidad muy baja y cerca del suelo
      else {
        bool low_speed = (fabsf(_vz_ema) < _cfg.climb_vz_exit);
        bool near_base = (fabsf(agl) < _cfg.ground_window_agl);
        if (low_speed && near_base &&
            dwellOK(now_ms, _cfg.min_dwell_ground)) {
          _mode = FlightMode::GROUND;
          _enter_ts = now_ms;
        }
      }
      break;
    }
  }

  // Actualiza referencias
  _prev_alt = alt_m;
  _prev_ms = now_ms;
  return _mode;
}

void FlightModeDetector::shiftGroundBase(float delta_m) {
  _ground_alt += delta_m;
}
