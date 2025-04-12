#include "buzzer_module.h"
#include <driver/ledc.h>

#define BUZZER_PIN     10
#define LEDC_CHANNEL_0 0
#define LEDC_TIMER_0   0

// Inicialización del PWM (LEDC) para el buzzer.
void initBuzzer() {
  // Configurar LEDC: Temporizador LEDC_TIMER_0 con frecuencia inicial de 2000 Hz y 8 bits de resolución.
  ledcSetup(LEDC_TIMER_0, 2000, 8);
  // Asignar el pin del buzzer al canal LEDC_CHANNEL_0.
  ledcAttachPin(BUZZER_PIN, LEDC_CHANNEL_0);
}

// Función que hace sonar el buzzer.
void buzzerBeep(uint32_t frequency, uint8_t dutyCycle, uint32_t durationMs) {
  // Se puede reconfigurar la frecuencia si se desea cambiar "al vuelo"
  ledcSetup(LEDC_TIMER_0, frequency, 8);
  
  // Activar el buzzer escribiendo el duty cycle
  ledcWrite(LEDC_CHANNEL_0, dutyCycle);
  
  // Esperar la duración requerida
  delay(durationMs);
  
  // Apagar el buzzer
  ledcWrite(LEDC_CHANNEL_0, 0);
}
