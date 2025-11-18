
#pragma once
#include <U8g2lib.h>

// Abre el submenú de Bitácora (posición = último salto)
void logbookUiOpen();

// ¿El submenú está activo?
bool logbookUiIsActive();

// Dibuja y maneja botones del submenú (llámala desde updateUI cuando el menú esté activo)
void logbookUiDrawAndHandle(U8G2 &u8g2);

static inline String logbookFormatAltCm(int32_t alt_cm, bool enMetros, int decimales) {
  if (enMetros) {
    float m = alt_cm / 100.0f;
    char buf[24];
    dtostrf(m, 0, decimales, buf);
    return String(buf) + F(" m");
  } else {
    // cm -> ft  (1 cm = 0.032808399 ft)
    float ft = alt_cm * 0.032808399f;
    char buf[24];
    dtostrf(ft, 0, decimales, buf);
    return String(buf) + F(" ft");
  }
}

static inline String logbookFormatFF(uint16_t ff_ds) {
  // ff_ds = decisegundos -> preferir "M:SS" si >= 60 s, si no "S.s s"
  uint32_t total_cs = (uint32_t)ff_ds * 10u;     // centisegundos (para 1 dec)
  uint32_t total_s  = total_cs / 100u;
  if (total_s >= 60u) {
    uint32_t m  = total_s / 60u;
    uint32_t s  = total_s % 60u;
    char buf[16];
    snprintf(buf, sizeof(buf), "%lu:%02lu", (unsigned long)m, (unsigned long)s);
    return String(buf);
  } else {
    // Mostrar con 1 decimal (x.y s)
    // decimas = (total_cs % 100) / 10
    uint32_t s  = total_cs / 100u;
    uint32_t d1 = (total_cs % 100u) / 10u;
    char buf[16];
    snprintf(buf, sizeof(buf), "%lu.%lu s", (unsigned long)s, (unsigned long)d1);
    return String(buf);
  }
}

static inline String logbookFormatVelKmh(uint16_t v_cmps, int decimales) {
  // cm/s -> km/h  (1 cm/s = 0.036 km/h)
  float kmh = v_cmps * 0.036f;
  char buf[24];
  dtostrf(kmh, 0, decimales, buf);
  return String(buf) + F(" km/h");
}