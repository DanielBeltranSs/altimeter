#pragma once
#include "config/device_config.h"

class AltitudeFrame {
public:
  void setConfig(const DeviceConfig* c) { cfg = c; }

  // agl_raw_m: AGL física (msl-ground_ref), en metros
  float aglIndicated_m(float agl_raw_m) const {
    const float off_m = cfg ? (cfg->offset_mm / 1000.0f) : 0.0f;
    return agl_raw_m + off_m;
  }

  // Valor para mostrar (según unidades)
  float displayValue(float agl_raw_m) const {
    float a = aglIndicated_m(agl_raw_m);
    return (cfg && cfg->units == Units::METERS) ? a : a * 3.280839895f;
  }

private:
  const DeviceConfig* cfg = nullptr;
};
