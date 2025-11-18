#pragma once
#include <stdint.h>
#include <math.h>
#include "app/flight_mode.h"

// 1 Pa ≈ 0.083 m de altitud (aprox ISA cerca del nivel del mar)
static inline float pa_to_m(float pa) { return pa * 0.083f; }

struct AutoGZConfig {
  // Estabilidad (varianza sobre altitud filtrada)
  float alt_sigma_thresh_m = 0.20f;

  // Deadband absoluto sobre altitud (no corrijas si |alt| ≤ deadband)
  float alt_deadband_m     = 0.8f;    // ~2.6 ft

  // Ventana de estabilidad y espaciamiento entre ajustes "normales"
  uint32_t stable_time_ms  = 60000;   // 60 s quieto antes de corregir
  uint32_t min_interval_ms = 60000;   // 60 s entre pasos

  // Promedios (EMA)
  float alpha_alt   = 0.10f;          // EMA(alt)
  float alpha_alt2  = 0.10f;          // EMA(alt^2)
  float alpha_press = 0.10f;          // EMA(pressure)

  // Límite de paso de p0 por ajuste "normal" (Pa). 1 Pa ≈ 0.083 m
  float max_step_pa = 6.0f;           // ~0.5 m por minuto de calma

  // Filtro anti-tendencia meteo: si el barómetro se mueve rápido, no corregir
  float trend_pa_per_min_max = 2.0f;  // si |dP/dt| > 2 Pa/min, pausar

  // ===== Creep dentro del deadband (micro-corrección lenta) =====
  float   creep_deadband_m  = 0.30f;          // por encima de esto sí “rampea” suave
  float   creep_pa_per_min  = 0.50f;          // Pa/min dentro del deadband
  uint32_t creep_after_ms   = 10 * 60 * 1000; // tras 10 min de calma
};

class AutoGroundZero {
public:
  void begin(float p0) {
    _p0 = p0;
    _ema_alt = 0.0f; _ema_alt2 = 0.0f; _ema_p = p0;
    _last_apply_ms = 0;
    _stable_enter_ms = 0;
    _have_sample = false;
    _last_p = NAN; _last_ms = 0;
  }

  // Inyecta el FSM para compensar AGL al ajustar p0
  void setFsm(class FlightModeDetector* fsm) { _fsm = fsm; }

  // Ajusta p0 suavemente cuando estás quieto en GROUND y la señal es estable.
  // Devuelve true si aplicó un paso a p0.
  bool update(float pressurePa, float alt_m, FlightMode mode, uint32_t now_ms) {
    // Solo opera en GROUND (en main ya filtras "still" por vz_ema)
    if (mode != FlightMode::GROUND) {
      _stable_enter_ms = 0;
      _have_sample = false;
      _last_p = NAN; _last_ms = 0;
      return false;
    }

    // EMAs
    if (!_have_sample) {
      _ema_alt = alt_m;
      _ema_alt2 = alt_m * alt_m;
      _ema_p = pressurePa;
      _have_sample = true;
    } else {
      _ema_alt  = _cfg.alpha_alt  * alt_m          + (1.0f - _cfg.alpha_alt)  * _ema_alt;
      _ema_alt2 = _cfg.alpha_alt2 * (alt_m*alt_m)  + (1.0f - _cfg.alpha_alt2) * _ema_alt2;
      _ema_p    = _cfg.alpha_press* pressurePa     + (1.0f - _cfg.alpha_press)* _ema_p;
    }

    // Varianza local (ruido) → sigma
    const float var   = fmaxf(0.0f, _ema_alt2 - _ema_alt * _ema_alt);
    const float sigma = sqrtf(var);
    const bool  low_noise = (sigma <= _cfg.alt_sigma_thresh_m);

    // Tendencia barométrica (Pa/min)
    float dpa_per_min = 0.0f;
    if (!isnan(_last_p) && _last_ms > 0) {
      float dt_min = (now_ms - _last_ms) / 60000.0f;
      if (dt_min > 0.0f) dpa_per_min = (pressurePa - _last_p) / dt_min;
    }
    _last_p = pressurePa;
    _last_ms = now_ms;

    // Requisitos básicos: señal estable y sin tendencia fuerte
    if (!low_noise || fabsf(dpa_per_min) > _cfg.trend_pa_per_min_max) {
      _stable_enter_ms = 0;
      return false;
    }

    // Ventana de estabilidad
    if (_stable_enter_ms == 0) _stable_enter_ms = now_ms;
    const bool time_ok    = (now_ms - _stable_enter_ms) >= _cfg.stable_time_ms;
    const bool spacing_ok = (now_ms - _last_apply_ms)   >= _cfg.min_interval_ms;
    if (!time_ok) return false;

    // ===== Deadband cerca de cero =====
    if (fabsf(_ema_alt) <= _cfg.alt_deadband_m) {
      // CREEP ULTRA-LENTO DENTRO DEL DEADBAND
      const bool can_creep = (fabsf(_ema_alt) > _cfg.creep_deadband_m) &&
                             ((now_ms - _stable_enter_ms) >= _cfg.creep_after_ms) &&
                             spacing_ok;
      if (!can_creep) return false;

      // Corrige muy lento en la dirección que lleva a alt=0
      float dt_min = (now_ms - _last_apply_ms) / 60000.0f;
      if (dt_min <= 0.0f) return false;

      float delta = (_ema_alt > 0 ? -1.0f : +1.0f) * (_cfg.creep_pa_per_min * dt_min);

      // Aplica el paso y compensa AGL
      _p0 += delta;
      _last_apply_ms = now_ms;
      if (_fsm) _fsm->shiftGroundBase(pa_to_m(delta));
      return true;
    }

    // ===== Corrección normal (fuera del deadband) =====
    if (!spacing_ok) return false;

    // Objetivo: alt=0 ⇒ p0 ≈ presión local (suavizada)
    const float p0_target = _ema_p;

    // Paso limitado (Pa) por intervalo
    float delta = p0_target - _p0;
    if (delta >  _cfg.max_step_pa) delta =  _cfg.max_step_pa;
    if (delta < -_cfg.max_step_pa) delta = -_cfg.max_step_pa;

    if (fabsf(delta) > 0.0001f) {
      _p0 += delta;
      _last_apply_ms = now_ms;
      if (_fsm) _fsm->shiftGroundBase(pa_to_m(delta)); // Consistencia AGL
      return true;
    }
    return false;
  }

  float p0() const { return _p0; }
  void setConfig(const AutoGZConfig& c) { _cfg = c; }

private:
  AutoGZConfig _cfg{};
  float _p0 = 101325.0f;

  // EMAs
  float _ema_alt = 0.0f, _ema_alt2 = 0.0f, _ema_p = 101325.0f;

  // Estado
  uint32_t _last_apply_ms = 0;
  uint32_t _stable_enter_ms = 0;
  bool _have_sample = false;

  // Tendencia
  float _last_p = NAN;
  uint32_t _last_ms = 0;

  // FSM para compensar AGL (inyectado)
  class FlightModeDetector* _fsm = nullptr;
};
