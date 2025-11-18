#include "buzzer_module.h"
#include <driver/ledc.h>

// --- Configuración de hardware ---
#define BUZZER_PIN       10

// En Arduino-ESP32, ledcSetup usa "channel" (0..15), no el timer.
// Usamos el canal 0 con resolución de 8 bits.
#define BUZZER_LEDC_CHANNEL   0
#define BUZZER_LEDC_RES_BITS  8

// Estado simple para evitar reconfigurar de más
static bool s_ledc_init = false;
static uint32_t s_currentFreq = 0;

// Inicialización del PWM (LEDC) para el buzzer.
void initBuzzer() {
  // Configura el canal con una frecuencia inicial (2 kHz por defecto)
  if (!s_ledc_init) {
    ledcSetup(BUZZER_LEDC_CHANNEL, 2000 /*Hz*/, BUZZER_LEDC_RES_BITS);
    ledcAttachPin(BUZZER_PIN, BUZZER_LEDC_CHANNEL);
    s_ledc_init = true;
    s_currentFreq = 2000;
  }
  // Asegura silencio al inicio
  ledcWrite(BUZZER_LEDC_CHANNEL, 0);
}

// Apaga inmediatamente el buzzer
void stopBuzzer() {
  if (!s_ledc_init) return;
  ledcWrite(BUZZER_LEDC_CHANNEL, 0);
}

// Beep bloqueante sencillo
void buzzerBeep(uint32_t frequency, uint8_t dutyCycle, uint32_t durationMs) {
  if (!s_ledc_init) {
    initBuzzer();
  }

  // Ajusta frecuencia solo si cambia (pequeña optimización)
  if (frequency == 0) {
    // Si piden 0 Hz, equivale a apagar
    stopBuzzer();
    delay(durationMs);
    return;
  }
  if (frequency != s_currentFreq) {
    ledcSetup(BUZZER_LEDC_CHANNEL, frequency, BUZZER_LEDC_RES_BITS);
    s_currentFreq = frequency;
  }

  // Limita duty a 8 bits (0..255)
  if (dutyCycle > 255) dutyCycle = 255;

  // Enciende
  ledcWrite(BUZZER_LEDC_CHANNEL, dutyCycle);

  // Mantiene durante durationMs
  delay(durationMs);

  // Apaga
  ledcWrite(BUZZER_LEDC_CHANNEL, 0);
}
