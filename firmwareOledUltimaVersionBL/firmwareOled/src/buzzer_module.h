// buzzer_module.h

#ifndef BUZZER_MODULE_H
#define BUZZER_MODULE_H

#include <Arduino.h>

// Inicializa el buzzer (configura LEDC y asegura silencio al inicio)
void initBuzzer();

/**
 * Emite un beep bloqueante.
 * @param frequency   Frecuencia en Hz (si es 0, se apaga durante durationMs).
 * @param dutyCycle   Ciclo de trabajo [0..255] para resolución de 8 bits.
 * @param durationMs  Duración del beep en milisegundos.
 */
void buzzerBeep(uint32_t frequency, uint8_t dutyCycle, uint32_t durationMs);

// Apaga inmediatamente el buzzer (duty = 0). Útil antes de entrar en deep sleep.
void stopBuzzer();

#endif // BUZZER_MODULE_H
