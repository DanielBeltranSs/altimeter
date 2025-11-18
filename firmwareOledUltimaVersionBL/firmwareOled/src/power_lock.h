#pragma once
#include <Arduino.h>

// Duración fija del lock (25 min), independiente de INACTIVITY_MS
#ifndef SLEEP_LOCK_MS_FIXED
#define SLEEP_LOCK_MS_FIXED (25UL * 60UL * 1000UL)
#endif

void powerLockActivate(uint32_t ms = SLEEP_LOCK_MS_FIXED); // activa/renueva lock
void powerLockClear();                                     // desactiva lock
void powerLockUpdate();                                    // expira lock si corresponde
bool powerLockActive();                                    // ¿lock activo?
