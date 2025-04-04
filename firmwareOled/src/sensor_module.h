#ifndef SENSOR_MODULE_H
#define SENSOR_MODULE_H

#include <Adafruit_BMP3XX.h>
#include <driver/adc.h>

// Objeto para el sensor BMP390 y altitud de referencia
extern Adafruit_BMP3XX bmp;
extern float altitudReferencia;
extern float altCalculada;
extern float altitud;
// Variables para lectura de batería
extern int cachedBatteryPercentage;
extern unsigned long lastBatteryUpdate;
extern const unsigned long batteryUpdateInterval;

extern float lastAltForAhorro;
extern unsigned long lastAltChangeTime;
extern const float ALT_CHANGE_THRESHOLD;

extern int lecturaADC;
extern float voltajeADC;
extern float v_adc;
extern float v_bat;

extern bool enSalto;         // Indica si se está en salto (altitud > 60 ft)
extern bool ultraPreciso;    // Indica si se activa el modo ultra preciso (altitud > 12,000 ft)


// Funciones para inicializar y actualizar el sensor y la batería
void initSensor();
void updateSensorData();
int calcularPorcentajeBateria(float voltaje);
void updateBatteryReading();

#endif
