// =============================
// alarm.cpp  (NO BLOQUEANTE, SIMPLE)
// =============================
#include "alarm.h"

// ---------- Estado interno ----------
static bool     s_enabled = (VIBRO_PIN >= 0);

enum Stage : uint8_t { ST_IDLE = 0, ST_ON, ST_GAP };
static Stage    s_stage = ST_IDLE;

static uint8_t  s_pendingPulses = 0;         // cuántos pulsos faltan (corto/medio)
static uint16_t s_nextPulseMs   = VIBRO_MS_SHORT; // duración del PRÓXIMO pulso (solo el primero puede variar)
static uint32_t s_stageEndMs    = 0;         // cuándo termina el ON/GAP en curso (millis)

static int      s_lastBuzzedPercent = 101;   // lógica batería

// ---------- Helpers ----------
static inline void motorWrite(bool on){
  if (VIBRO_PIN < 0 || !s_enabled) return;
  // Ajuste activo-alto/activo-bajo
  bool level = VIBRO_ACTIVE_HIGH ? on : !on;
  digitalWrite(VIBRO_PIN, level ? HIGH : LOW);
}

static inline void motorOn(){  motorWrite(true);  }
static inline void motorOff(){ motorWrite(false); }

// ---------- API ----------
void alarmInit(){
  if (VIBRO_PIN < 0){
    s_enabled = false;
    return;
  }
  pinMode(VIBRO_PIN, OUTPUT);
  motorOff();

  s_enabled = true;
  s_stage   = ST_IDLE;
  s_pendingPulses = 0;
  s_nextPulseMs   = VIBRO_MS_SHORT;
  s_stageEndMs    = 0;
  s_lastBuzzedPercent = 101;
}

void alarmSetEnabled(bool en){
  s_enabled = en && (VIBRO_PIN >= 0);
  if (!s_enabled){
    alarmClearAll();
  }
}

bool alarmIsEnabled(){
  return s_enabled && (VIBRO_PIN >= 0);
}

bool alarmReady(){
  // Listo cuando no hay pulsos pendientes ni activos
  return (s_stage == ST_IDLE) && (s_pendingPulses == 0);
}

void alarmClearAll(){
  s_pendingPulses = 0;
  s_stage = ST_IDLE;
  motorOff();
}

// Encola N pulsos. El PRIMER pulso usa first_pulse_ms; los demás usan VIBRO_MS_SHORT.
// Si ya había pulsos pendientes, se respetan y solo se suman más (sin FIFO de duraciones).
bool alarmEnqueue(uint8_t count, uint16_t first_pulse_ms){
  if (!alarmIsEnabled() || count == 0) return false;
  uint16_t total = (uint16_t)s_pendingPulses + count;
  if (total > VIBRO_MAX_PULSES) total = VIBRO_MAX_PULSES;

  // Si no hay nada en curso, el primer pulso que se ejecute tomará esta duración
  if (s_pendingPulses == 0 && s_stage == ST_IDLE){
    s_nextPulseMs = (first_pulse_ms == 0) ? VIBRO_MS_SHORT : first_pulse_ms;
  }
  s_pendingPulses = (uint8_t)total;
  return true;
}

// Máquina de estados no bloqueante (llamar siempre desde loop)
void alarmService(){
  if (!alarmIsEnabled()) return;

  uint32_t now = millis();

  switch (s_stage){
    case ST_IDLE:
      if (s_pendingPulses > 0){
        // Arrancar un pulso
        motorOn();
        s_stage = ST_ON;
        s_stageEndMs = now + s_nextPulseMs;
        // Los siguientes pulsos, si quedan, usarán duración corta estándar
        s_nextPulseMs = VIBRO_MS_SHORT;
      }
      break;

    case ST_ON:
      if ((int32_t)(now - s_stageEndMs) >= 0){
        motorOff();
        s_stage = ST_GAP;
        s_stageEndMs = now + VIBRO_MS_GAP;
      }
      break;

    case ST_GAP:
      if ((int32_t)(now - s_stageEndMs) >= 0){
        // Finalizó el gap: decrementar y volver a IDLE
        if (s_pendingPulses > 0) s_pendingPulses--;
        s_stage = ST_IDLE;
      }
      break;
  }
}

// ---------- Eventos de alto nivel ----------
void alarmOnBatteryPercent(int now_percent){
  if (!alarmIsEnabled()) return;
  if (now_percent < 0 || now_percent > 100) return;

  // Si sube >5%, resetea la lógica
  if (now_percent > 5){
    s_lastBuzzedPercent = 101;
    return;
  }

  // Primera entrada a <=5%: un pulso
  if (now_percent <= 5 && s_lastBuzzedPercent > 5){
    alarmEnqueue(1, VIBRO_MS_SHORT);
    s_lastBuzzedPercent = now_percent;
    return;
  }

  // Ya en crítico: 1 pulso por cada % adicional perdido
  if (s_lastBuzzedPercent <= 5 && now_percent < s_lastBuzzedPercent){
    int delta = s_lastBuzzedPercent - now_percent;   // p.ej., 5->4 => 1
    if (delta > 0){
      alarmEnqueue((uint8_t)delta, VIBRO_MS_SHORT);
      s_lastBuzzedPercent = now_percent;
    }
  }
}

void alarmOnEnterDeepSleep(){
  // Dos pulsos cortos
  alarmEnqueue(2, VIBRO_MS_SHORT);
  // Nota: no bloquea; espera a alarmReady() antes de esp_deep_sleep_start().
}

void alarmOnWakeFromDeepSleep(){
  // Un pulso medio (solo el primero de la cola puede variar)
  alarmEnqueue(1, VIBRO_MS_MED);
}

void alarmOnLockAltitude(){
  alarmEnqueue(1, VIBRO_MS_SHORT);
}

void alarmOnDateSaved(){
  alarmEnqueue(1, VIBRO_MS_DATE);
}

void alarmOnOffsetSaved(){
  alarmEnqueue(2, VIBRO_MS_SHORT);
}

void alarmOnLogbookCleared(){
  alarmEnqueue(3, VIBRO_MS_SHORT);
}
