#pragma once
#include <cmath>

class AltitudeEstimator {
public:
  void setSeaLevelPressure(float p0Pa) { p0_ = p0Pa; }
  float seaLevelPressure() const { return p0_; }

  // Altitud (m) usando ISA estándar
  float toAltitudeMeters(float pressurePa) const {
    if (pressurePa <= 0.0f || p0_ <= 0.0f) return NAN;
    const float ratio = pressurePa / p0_;
    // 1/5.255... = 0.190294957
    return 44330.0f * (1.0f - std::pow(ratio, 0.190294957f));
  }

  // Suavizado EMA opcional (para imprimir más estable)
  void setEmaAlpha(float a) { alpha_ = (a <= 0.f) ? 1.f : (a >= 1.f ? 1.f : a); ema_ = NAN; }
  float filter(float value) {
    if (std::isnan(ema_)) { ema_ = value; return value; }
    ema_ = alpha_ * value + (1.f - alpha_) * ema_;
    return ema_;
  }

private:
  float p0_    = 101325.0f; // Presión nivel del mar por defecto
  float alpha_ = 1.0f;      // 1.0 = sin filtro, 0.1..0.3 típico
  float ema_   = NAN;
};
