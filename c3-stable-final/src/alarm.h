// =============================
// alarm.h  (NO BLOQUEANTE, SIMPLE)
// Motor vibrador 3.3 V con transistor/MOSFET (low-side recomendado).
// Requiere: alarmInit() en setup() y alarmService() en cada loop().
// =============================
#pragma once
#include <Arduino.h>

// ---------- Config (puedes sobreescribir antes de incluir) ----------
#ifndef VIBRO_PIN
  #define VIBRO_PIN -1       // GPIO que controla el transistor/MOSFET. -1 = deshabilitado
#endif

#ifndef VIBRO_ACTIVE_HIGH
  // 1: HIGH enciende motor (low-side típico).
  // 0: activo-bajo (HIGH apaga, LOW enciende).
  #define VIBRO_ACTIVE_HIGH 1
#endif

// Duraciones por defecto (ms)
#ifndef VIBRO_MS_SHORT
  #define VIBRO_MS_SHORT 90
#endif
#ifndef VIBRO_MS_MED
  #define VIBRO_MS_MED   160
#endif
#ifndef VIBRO_MS_GAP
  #define VIBRO_MS_GAP   90
#endif
#ifndef VIBRO_MS_DATE      // para “guardar fecha”
  #define VIBRO_MS_DATE   80
#endif

// Límite de pulsos encolados (simple contador, no FIFO de patrones)
#ifndef VIBRO_MAX_PULSES
  #define VIBRO_MAX_PULSES 20
#endif

// ---------- API ----------
void alarmInit();
void alarmSetEnabled(bool en);
bool alarmIsEnabled();

// Llamar en cada loop (no bloquea)
void alarmService();

// Estado / control
bool alarmReady();      // true si no hay pulsos en curso/pendientes (OK para dormir)
void alarmClearAll();   // cancelar todo y apagar

// Encolar pulsos (no bloqueante)
bool alarmEnqueue(uint8_t count, uint16_t first_pulse_ms = VIBRO_MS_SHORT);

// Eventos de alto nivel (llama cuando ocurran)
void alarmOnBatteryPercent(int now_percent); // <=5%: 1 pulso; luego 1 por % perdido
void alarmOnEnterDeepSleep();                // 2 cortos
void alarmOnWakeFromDeepSleep();             // 1 med
void alarmOnLockAltitude();                  // 1 corto
void alarmOnDateSaved();                     // 1 corto (80 ms)
void alarmOnOffsetSaved();                   // 2 cortos
void alarmOnLogbookCleared();                // 3 cortos
