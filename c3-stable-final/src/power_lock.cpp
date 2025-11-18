// ===================== power_lock.cpp =====================
#include <Arduino.h>   // para millis() y Serial
#include "power_lock.h"

static bool     g_lock   = false;
static uint32_t g_until  = 0;

void powerLockActivate(uint32_t ms) {
  g_lock  = true;
  g_until = millis() + ms;
  Serial.printf("Sleep-lock ACTIVADO por %lu min\n", ms / 60000UL);
}

void powerLockClear() {
  if (g_lock) {
    g_lock = false;
    Serial.println("Sleep-lock DESACTIVADO");
  }
}

void powerLockUpdate() {
  if (g_lock && (int32_t)(millis() - g_until) >= 0) {
    g_lock = false;
    Serial.println("Sleep-lock EXPIRÓ");
  }
}

bool powerLockActive() {
  return g_lock;
}
