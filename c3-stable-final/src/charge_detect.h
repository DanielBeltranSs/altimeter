#pragma once
#include <Arduino.h>

// GPIO conectado al divisor desde VBUS (5V) -> ADC.
// ⚠️ NO uses BATTERY_PIN. Cambia aquí si usaste otro.
// En tu montaje: GPIO 2.
#ifndef CHARGE_ADC_PIN
#define CHARGE_ADC_PIN  0
#endif

// Inicializa el ADC para detectar VBUS por divisor
void chargeDetectBegin();

// Actualiza el filtro/histéresis (llamar en cada loop)
void chargeDetectUpdate();

// true si se considera que hay USB presente (VBUS alto)
bool isUsbPresent();

// ---------- DEBUG opcional ----------
int   chargeDebugRaw();      // Lectura ADC cruda
float chargeDebugVadc();     // Voltaje en el pin (≈0..3.3V)
float chargeDebugVbus();     // Estimación de VBUS (a partir del divisor)
