#ifndef SENSOR_MODULE_H
#define SENSOR_MODULE_H

#include <Adafruit_BMP3XX.h>  // Necesario porque otros módulos usan 'bmp' y sus métodos
// (No es necesario incluir <driver/adc.h> en el header)

// ----- Modo del sensor -----
enum SensorMode {
  SENSOR_MODE_AHORRO = 0,
  SENSOR_MODE_ULTRA_PRECISO = 1,
  SENSOR_MODE_FREEFALL = 2
};

// Exponer el modo actual
SensorMode getSensorMode();

// Objeto para el sensor BMP390 y variables de altitud
extern Adafruit_BMP3XX bmp;
extern float altitudReferencia;
extern float altCalculada;   // relativa (m)
extern float altitud;        // absoluta (m)

// Variables para lectura de batería
extern int cachedBatteryPercentage;
extern unsigned long lastBatteryUpdate;
extern const unsigned long batteryUpdateInterval;

// Ahorro de pantalla por inactividad (basado en cambios de altitud)
extern float lastAltForAhorro;
extern unsigned long lastAltChangeTime;
extern const float ALT_CHANGE_THRESHOLD;

// Señales auxiliares de batería (rellenadas en el .cpp)
extern int   lecturaADC;
extern float voltajeADC;
extern float v_adc;
extern float v_bat;

// Flags y contadores de salto / precisión (usados por la UI)
extern bool     enSalto;         // true si altitud > 60 ft
extern bool     ultraPreciso;    // true en modo ULTRA
extern uint32_t jumpCount;

// Funciones públicas del módulo
void initSensor();
void updateSensorData();
int  calcularPorcentajeBateria(float voltaje);
void updateBatteryReading();

#endif // SENSOR_MODULE_H
