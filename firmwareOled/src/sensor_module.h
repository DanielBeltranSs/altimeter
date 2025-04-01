#ifndef SENSOR_MODULE_H
#define SENSOR_MODULE_H

#include <Adafruit_BMP3XX.h>

// Objeto para el sensor BMP390 y altitud de referencia
extern Adafruit_BMP3XX bmp;
extern float altitudReferencia;
extern float altCalculada;
extern float altitud;
// Variables para lectura de batería
extern int cachedBatteryPercentage;
extern unsigned long lastBatteryUpdate;
extern const unsigned long batteryUpdateInterval;
extern unsigned long lastAltChangeTime;

// Funciones para inicializar y actualizar el sensor y la batería
void initSensor();
void updateSensorData();
int calcularPorcentajeBateria(float voltaje);
void updateBatteryReading();

#endif
