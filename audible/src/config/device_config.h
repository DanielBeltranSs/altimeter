#pragma once
#include <stdint.h>

// en algún header común, p.ej. config/device_config.h
#define DEFAULT_OFFSET_MM   91440   // +300 ft ≈ 91 440 mm
#define DEFAULT_UNITS_FEET  1       // 1=feet, 0=meters (ajusta a tu enum si difiere)


enum class Units : uint8_t { FEET = 0, METERS = 1 };

struct DeviceConfig {
  uint8_t  schema_ver = 1;     // subir si cambia la estructura
  Units    units      = Units::FEET;
  int32_t  offset_mm  = 0;     // + arriba, - abajo
  // futuros: harddeck_mm, etc.
};
