// buzzer_module.h

#ifndef BUZZER_MODULE_H
#define BUZZER_MODULE_H

#include <Arduino.h>

// Declaración de la función buzzerBeep
// frequency: frecuencia en Hz a la que sonará el buzzer.
// dutyCycle: valor entre 0 y 255 (si se usa una resolución de 8 bits).
// durationMs: duración del sonido en milisegundos.
void buzzerBeep(uint32_t frequency, uint8_t dutyCycle, uint32_t durationMs);

// Función opcional de inicialización para el buzzer.
void initBuzzer();

#endif // BUZZER_MODULE_H
