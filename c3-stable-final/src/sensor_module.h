#ifndef SENSOR_MODULE_H
#define SENSOR_MODULE_H

#include <Adafruit_BMP3XX.h>  // Otros módulos usan 'bmp' y sus métodos

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

// Flags y contadores de salto / precisión (usados por la UI)
extern bool     enSalto;         // actividad (armado/seguimiento)
extern bool     ultraPreciso;    // true en modo ULTRA (contorno)
extern bool     jumpArmed;       // armado por altura
extern bool     inJump;          // freefall confirmado (relleno)

// Funciones públicas del módulo
void initSensor();
void updateSensorData();

#endif // SENSOR_MODULE_H