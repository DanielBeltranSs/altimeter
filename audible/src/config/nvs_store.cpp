#include "nvs_store.h"
#include <Preferences.h>
#include <algorithm>

static const char* NS = "cfg";

// ---- Cambia estos dos para fijar el valor de fábrica ----
#ifndef NVS_DEFAULT_OFFSET_MM
  #define NVS_DEFAULT_OFFSET_MM   91440     // +300 ft ≈ 91 440 mm
#endif
#ifndef NVS_DEFAULT_UNITS
  #define NVS_DEFAULT_UNITS       Units::FEET
#endif
// ---------------------------------------------------------

static inline int32_t clampMm(int32_t mm) {
  const int32_t LIM = 1500000;              // ±1500 m
  return std::max(-LIM, std::min(LIM, mm));
}

bool cfgLoad(DeviceConfig& out) {
  Preferences p;
  if (!p.begin(NS, /*readOnly=*/true)) return false;

  uint8_t ver = p.getUChar("ver", 0xFF);
  if (ver == 0xFF) {
    // No hay NVS todavía: inicializamos con defaults y guardamos.
    p.end();
    out = DeviceConfig{};
    out.schema_ver = 1;
    out.units      = NVS_DEFAULT_UNITS;
    out.offset_mm  = clampMm(NVS_DEFAULT_OFFSET_MM);
    return cfgSave(out);                    // crea la NVS con estos valores
  }

  out.schema_ver = ver;
  out.units      = (Units)p.getUChar("units", (uint8_t)Units::FEET);
  out.offset_mm  = clampMm(p.getInt("offmm", 0));
  p.end();
  return true;
}

bool cfgSave(const DeviceConfig& cfg) {
  Preferences p;
  if (!p.begin(NS, /*readOnly=*/false)) return false;
  p.putUChar("ver",   cfg.schema_ver);
  p.putUChar("units", (uint8_t)cfg.units);
  p.putInt("offmm",   (int)clampMm(cfg.offset_mm));
  p.end();
  return true;
}

// Helper: cambiar offset y persistir (útil para “hardcodear” rápido)
bool cfgSetOffsetMm(int32_t off_mm) {
  DeviceConfig cfg;
  if (!cfgLoad(cfg)) {
    cfg = DeviceConfig{};
    cfg.schema_ver = 1;
  }
  cfg.offset_mm = clampMm(off_mm);
  return cfgSave(cfg);
}
