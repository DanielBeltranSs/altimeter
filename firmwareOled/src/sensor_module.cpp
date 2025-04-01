#include "sensor_module.h"
#include "config.h"       // Para usar BMP_ADDR
#include "ui_module.h"    // Para acceder a: menuActivo, pantallaEncendida y u8g2
#include <Arduino.h>
#include <Wire.h>
#include <driver/adc.h>
#include <math.h>

// Definición del objeto sensor y variable de altitud
Adafruit_BMP3XX bmp;
float altitudReferencia = 0.0;

// Variables para la batería
int cachedBatteryPercentage = 0;
unsigned long lastBatteryUpdate = 0;
const unsigned long batteryUpdateInterval = 5000; // 5 segundos

// Variables para el modo ahorro basadas en altitud
// Estas variables se usan para detectar cambios en la altitud y activar el ahorro si no hay cambios
float lastAltForAhorro = 0;
unsigned long lastAltChangeTime = 0;
const float ALT_CHANGE_THRESHOLD = 1.0;  // Umbral (por ejemplo, 1 metro)

void initSensor() {
  if (!bmp.begin_I2C(BMP_ADDR)) {
    Serial.println("¡Sensor BMP390L no encontrado!");
    while (1);
  }
  bmp.setTemperatureOversampling(BMP3_OVERSAMPLING_4X);
  bmp.setPressureOversampling(BMP3_OVERSAMPLING_8X);
  bmp.setIIRFilterCoeff(BMP3_IIR_FILTER_COEFF_7);
  bmp.setOutputDataRate(BMP3_ODR_50_HZ);
  if (bmp.performReading()) {
    lastAltForAhorro = bmp.readAltitude(1013.25);
  }
  lastAltChangeTime = millis();
}

void updateSensorData() {
  bool sensorOk = bmp.performReading();
  float altitudActual = sensorOk ? bmp.readAltitude(1013.25) : 0;
  
  // Si no estamos en el menú y se ha configurado el modo ahorro, comprobamos inactividad
  // Se asume que 'menuActivo' y 'pantallaEncendida' están declarados como extern en ui_module.h
  if (!menuActivo && sensorOk && ahorroTimeoutMs > 0) {
    if (fabs(altitudActual - lastAltForAhorro) > ALT_CHANGE_THRESHOLD) {
      lastAltForAhorro = altitudActual;
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

void updateBatteryReading() {
  if (millis() - lastBatteryUpdate >= batteryUpdateInterval) {
    int lecturaADC = adc1_get_raw(ADC1_CHANNEL_1);
    float voltajeADC = (lecturaADC / 4095.0) * 2.6;
    float factorCorreccion = 1.238;
    float v_adc = voltajeADC;
    float v_bat = v_adc * 2.0 * factorCorreccion;
    cachedBatteryPercentage = calcularPorcentajeBateria(v_bat);
    lastBatteryUpdate = millis();
  }
}
