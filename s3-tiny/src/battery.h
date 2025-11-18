#pragma once
#include <Arduino.h>

// Inicialización / ciclo
void  batteryInit();
void  batteryUpdate();

// Lecturas
float batteryGetVoltage();   // Voltaje estimado de batería (V)
int   batteryGetPercent();   // 0..100 según regla 4.15V->100 ... 3.40V->0

// Flags de estado para la UI / sistema
bool  batteryIsLowPercent();     // true si % <= 5 (parpadeo en UI)
bool  batteryShouldDeepSleep();  // true si Vbat <= 3.36 V
//battery.h