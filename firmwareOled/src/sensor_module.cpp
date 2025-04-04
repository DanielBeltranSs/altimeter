#include "sensor_module.h"
#include "config.h"       // Para usar BMP_ADDR, etc.
#include "ui_module.h"    // Si es necesario acceder a variables de UI
#include <Arduino.h>
#include <Wire.h>
#include <driver/adc.h>
#include <math.h>

// Definición del objeto sensor y variable de altitud
Adafruit_BMP3XX bmp;
float altitudReferencia = 0.0;
float altCalculada = 0.0;
float altitud = 0.0;

// Variables para la batería
int cachedBatteryPercentage = 0;
unsigned long lastBatteryUpdate = 0;
const unsigned long batteryUpdateInterval = 5000; // 5 segundos

// Variables para el modo ahorro basadas en altitud
float lastAltForAhorro = 0;
unsigned long lastAltChangeTime = 0;
const float ALT_CHANGE_THRESHOLD = 1.0;  // Umbral en metros

// Variables para lectura de batería
int lecturaADC = 0;
float voltajeADC = 0.0;
float v_adc = 0.0;
float v_bat = 0.0;

// NUEVAS VARIABLES PARA DETECCIÓN DEL SALTO
bool enSalto = false;
bool ultraPreciso = false;

// Inicialización del sensor
void initSensor() {
  if (!bmp.begin_I2C(BMP_ADDR)) {
    Serial.println("¡Sensor BMP390L no encontrado!");
    while (1);
  }
  bmp.setTemperatureOversampling(BMP3_OVERSAMPLING_2X);
  bmp.setPressureOversampling(BMP3_OVERSAMPLING_4X);
  bmp.setIIRFilterCoeff(BMP3_IIR_FILTER_COEFF_3);
  bmp.setOutputDataRate(BMP3_ODR_25_HZ);
  if (bmp.performReading()) {
    lastAltForAhorro = altitudReferencia;
  }
  lastAltChangeTime = millis();
}

// Función para actualizar los datos del sensor
void updateSensorData() {
  bool sensorOk = bmp.performReading();
  // Altitud absoluta del sensor (en metros)
  float altActual = sensorOk ? bmp.readAltitude(1013.25) : altitud;
  altitud = altActual;
  // Altitud relativa (en metros) = lectura actual - referencia (0 antes del vuelo)
  altCalculada = altActual - altitudReferencia;
  
  // Convertir la altitud relativa a pies para detección (1 m = 3.281 ft)
  float altEnPies = altCalculada * 3.281;
  
  // Detectar inicio del salto: si se supera 60 ft
  if (!enSalto && altEnPies > 60) {
    enSalto = true;
    Serial.println("¡Salto iniciado! (Altitud > 60 ft)");
  }
  // Activar modo ultra preciso al superar 12,000 ft
  if (enSalto && !ultraPreciso && altEnPies > 12000) {
    ultraPreciso = true;
    // Reconfiguración para máxima precisión:
    bmp.setTemperatureOversampling(BMP3_OVERSAMPLING_16X);   // Máximo oversampling para temperatura
    bmp.setPressureOversampling(BMP3_OVERSAMPLING_16X);      // Máximo oversampling para presión
    bmp.setIIRFilterCoeff(BMP3_IIR_FILTER_COEFF_7);           // Filtro IIR al máximo (si es el valor más alto)
    bmp.setOutputDataRate(BMP3_ODR_50_HZ);                    // Reducir la tasa de actualización para mayor promediado
    Serial.println("Modo ultra preciso activado (Altitud > 12,000 ft)");
}

  // Al descender por debajo de 60 ft, se termina el salto
  if (enSalto && altEnPies < 60) {
    enSalto = false;
    ultraPreciso = false;
    // Reconfigurar el sensor a modo normal:
    bmp.setTemperatureOversampling(BMP3_OVERSAMPLING_2X);
    bmp.setPressureOversampling(BMP3_OVERSAMPLING_4X);
    bmp.setIIRFilterCoeff(BMP3_IIR_FILTER_COEFF_3);
    bmp.setOutputDataRate(BMP3_ODR_25_HZ);
    Serial.println("Fin del salto (Altitud < 60 ft), modo normal activado.");
}

  
  // (Opcional) Actualización para modo ahorro en periodos de inactividad, etc.
  if (!menuActivo && sensorOk && ahorroTimeoutMs > 0) {
    if (fabs(altActual - lastAltForAhorro) > ALT_CHANGE_THRESHOLD) {
      lastAltForAhorro = altActual;
      lastAltChangeTime = millis();
    } else {
      if ((millis() - lastAltChangeTime) >= ahorroTimeoutMs && pantallaEncendida) {
        u8g2.setPowerSave(true);
        pantallaEncendida = false;
        Serial.println("Modo ahorro: Pantalla suspendida por inactividad.");
      }
    }
  }
}

// Función para calcular el porcentaje de batería (sin cambios)
int calcularPorcentajeBateria(float voltaje) {
  if (voltaje >= 4.2) return 100;
  if (voltaje >= 4.1) return 95;
  if (voltaje >= 4.0) return 90;
  if (voltaje >= 3.9) return 85;
  if (voltaje >= 3.8) return 80;
  if (voltaje >= 3.7) return 75;
  if (voltaje >= 3.6) return 50;
  if (voltaje >= 3.5) return 25;
  if (voltaje >= 3.4) return 10;
  return 5;
}

// Función para actualizar la lectura de la batería
void updateBatteryReading() {
  if (millis() - lastBatteryUpdate >= batteryUpdateInterval) {
    lecturaADC = adc1_get_raw(ADC1_CHANNEL_1);
    voltajeADC = (lecturaADC / 4095.0) * 2.6;
    float factorCorreccion = 1.2;
    v_adc = voltajeADC;
    v_bat = v_adc * 2.0 * factorCorreccion;
    cachedBatteryPercentage = calcularPorcentajeBateria(v_bat);
    lastBatteryUpdate = millis();
  }
}
