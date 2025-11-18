#ifndef DATETIME_MODULE_H
#define DATETIME_MODULE_H

#include <Arduino.h>

// ============================================================
// Inicialización y base de tiempo
// ============================================================

// Inicializa: carga base desde NVS y, si venimos de DS, ajusta según el tiempo dormido.
void datetimeInit();

// Establece la fecha/hora manual (local) y zona horaria (minutos, puede ser negativa).
void datetimeSetManual(int year, int month, int day,
                       int hour, int minute, int second,
                       int tz_minutes);

// Epoch "ahora" en segundos (UTC). Devuelve -1 si no hay base válida.
int64_t datetimeNowEpoch();

// Zona horaria actual (minutos).
int32_t datetimeGetTZMinutes();
void    datetimeSetTZMinutes(int32_t tz_minutes);

// ============================================================
// Formatos "ahora" para UI (sin parámetro de epoch)
// ============================================================

// "HH:MM" de la hora actual (según base de tiempo disponible).
void datetimeFormatHHMM(char* buf, size_t n);

// "YYYY-MM-DD" de la fecha actual.
void datetimeFormatYMD(char* buf, size_t n);

// ============================================================
// Formatos a partir de un timestamp (epoch en segundos)
// ============================================================
//
// Notas:
// - Si ts == 0 o no hay base válida para convertir, se escribirá "--".
// - Todos los formateadores recortan con terminador '\0' si n es pequeño.
//

// "YYYY-MM-DD HH:MM"
void datetimeFormatEpoch(uint32_t ts, char* out, size_t n);

// "DD/MM HH:MM"
void datetimeFormatEpochShort(uint32_t ts, char* out, size_t n);

// "HH:MM" (solo hora/minuto) para un epoch dado.
void datetimeFormatEpoch_HHMM(uint32_t ts, char* out, size_t n);

// "DD/MM/YY" (dos dígitos de año) para un epoch dado.
void datetimeFormatEpoch_DDMMYY(uint32_t ts, char* out, size_t n);

// ============================================================
// Deep Sleep helpers
// ============================================================

// Llamar justo antes de entrar a deep sleep.
// - En IDF 5.x, el parámetro se ignora (usaremos el tiempo real dormido).
// - En IDF 4.x, si duermes por TIMER, pasa los microsegundos programados.
//   Si duermes por botón (sin TIMER), pasa 0 (no hay forma fiable de medir Δ).
void datetimeOnBeforeDeepSleep(uint64_t planned_sleep_us = 0);

// Helper para la UI: true si la API de duración de deep sleep está disponible (IDF ≥ 5).
bool datetimeHasWakeupDurationAPI();

// ============================================================
// Submenú de configuración (UI)
// ============================================================

bool datetimeMenuActive();
void datetimeMenuOpen();
void datetimeMenuClose();
void datetimeMenuDrawAndHandle();

#endif // DATETIME_MODULE_H
