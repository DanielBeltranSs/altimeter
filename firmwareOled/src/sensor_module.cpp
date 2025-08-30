#include "sensor_module.h"       // Declaraciones públicas de este módulo
#include "config.h"              // BMP_ADDR, pines, etc.
#include "ui_module.h"           // u8g2, pantallaEncendida, menuActivo, ahorroTimeoutMs
#include <Arduino.h>
#include <Wire.h>
#include <driver/adc.h>
#include <math.h>
#include <Preferences.h>
#include "ble_module.h"

// ------------------------------
// Simulación de altitud (OPCIÓN A)
// ------------------------------
// 1 = activar simulación fija, 0 = desactivar
#ifndef ALT_SIM
#define ALT_SIM 1
#endif
// Altura fija en pies para pruebas (p. ej. 0, 80, 2500, 12050)
#ifndef ALT_SIM_FT
#define ALT_SIM_FT 12050.0f
#endif

// ------------------------------
// Estado de modo (solo en este .cpp)
// ------------------------------
static SensorMode currentMode = SENSOR_MODE_AHORRO;   // inicia en Ahorro
static unsigned long lastForcedReadingTime = 0;       // para "lectura cada 60s" en Ahorro

// Exponer el modo actual (misma linkage C++)
SensorMode getSensorMode() {
  return currentMode;
}

// ------------------------------
// Variables externas definidas en otros módulos
// ------------------------------
extern bool calibracionRealizada;
extern float alturaOffset;
extern void onSampleAccepted();



// ------------------------------
// Objeto para el sensor BMP390 y variables de altitud
// ------------------------------
Adafruit_BMP3XX bmp;
float altitudReferencia = 0.0f;
float altCalculada      = 0.0f;
float altitud           = 0.0f;

// ------------------------------
// Variables para el manejo de la batería
// ------------------------------
int cachedBatteryPercentage = 0;
unsigned long lastBatteryUpdate = 0;
const unsigned long batteryUpdateInterval = 60000; // 60 segundos

// ------------------------------
// Variables para el modo ahorro (basado en cambios de altitud)
// ------------------------------
float lastAltForAhorro = 0.0f;
unsigned long lastAltChangeTime = 0;
const float ALT_CHANGE_THRESHOLD = 2.0f;  // Umbral en metros

// ------------------------------
// Variables para lectura de batería
// ------------------------------
int   lecturaADC   = 0;
float voltajeADC   = 0.0f;
float v_adc        = 0.0f;
float v_bat        = 0.0f;

// ------------------------------
// Variables para la detección del salto
// ------------------------------
bool enSalto = false;
bool ultraPreciso = false;
static Preferences prefsSaltos;
uint32_t jumpCount = 0;

// ====================================================
// Inicialización del sensor
// ====================================================
void initSensor() {
  if (!bmp.begin_I2C(BMP_ADDR)) {
    Serial.println("¡Sensor BMP390L no encontrado!");
    while (1) { delay(10); }
  }

  // Configuración inicial por defecto
  bmp.setTemperatureOversampling(BMP3_OVERSAMPLING_2X);
  bmp.setPressureOversampling(BMP3_OVERSAMPLING_4X);
  bmp.setIIRFilterCoeff(BMP3_IIR_FILTER_COEFF_3);
  bmp.setOutputDataRate(BMP3_ODR_25_HZ);

  // Lectura inicial para fijar la altitud de referencia
  if (bmp.performReading()) {
    altitudReferencia = bmp.readAltitude(1013.25);
    lastAltForAhorro  = altitudReferencia;
  }
  lastAltChangeTime = millis();

  // Inicializar el contador de saltos desde NVS (si no existe, se usa 0)
  prefsSaltos.begin("saltos", false);
  jumpCount = prefsSaltos.getUInt("jumpCount", 0);
  prefsSaltos.end();
}

// ====================================================
// Actualización de datos del sensor
// ====================================================
void updateSensorData() {
  // --------------------------------------------------
  // 1) Lectura del sensor
  //    - En Ahorro: cada 60 s (simula FORCED).
  //    - En otros modos: en cada ciclo (lectura frecuente).
  // --------------------------------------------------
  static bool firstReadingDone = false;

  const bool debeLeer =
      !firstReadingDone ||
      (currentMode == SENSOR_MODE_AHORRO && (millis() - lastForcedReadingTime >= 60000UL)) ||
      (currentMode != SENSOR_MODE_AHORRO);

  if (debeLeer) {
    const bool sensorOk = bmp.performReading();
    if (sensorOk) {
      const float altActual = bmp.readAltitude(1013.25);
      altitud       = altActual;
      altCalculada  = altActual - altitudReferencia + alturaOffset;

#if ALT_SIM
      // -------- Opción A: inyectar altitud fija de prueba --------
      // Forzamos una altura absoluta simulada (en pies) convertida a metros
      const float simAltM = ALT_SIM_FT / 3.281f;
      altitud      = simAltM;                                       // absoluto en metros
      altCalculada = simAltM - altitudReferencia + alturaOffset;    // relativa (lo que usa tu lógica)
#endif

      // Contador de Hz (si lo estás usando en main.cpp)
      onSampleAccepted();
    }

    if (!firstReadingDone) firstReadingDone = true;

    if (currentMode == SENSOR_MODE_AHORRO) {
      lastForcedReadingTime = millis();
    }
  }

  // Convertir la altitud relativa (en metros) a pies
  const float altEnPies = altCalculada * 3.281f;
  //comprovar altura
  //Serial.print("Altitud: ");
  //Serial.print(altEnPies);
  //Serial.println(" ft");

  // --------------------------------------------------
  // 2) Configuración de modos según la altitud
  // --------------------------------------------------
  if (altEnPies < 60.0f) {
    // AHORRO
    if (currentMode != SENSOR_MODE_AHORRO) {
      currentMode = SENSOR_MODE_AHORRO;
      bmp.setTemperatureOversampling(BMP3_OVERSAMPLING_2X);
      bmp.setPressureOversampling(BMP3_OVERSAMPLING_4X);
      bmp.setIIRFilterCoeff(BMP3_IIR_FILTER_COEFF_3);
      bmp.setOutputDataRate(BMP3_ODR_25_HZ);
      Serial.println("Modo Ahorro activado (Altitud < 60 ft)");
      lastForcedReadingTime = millis();
    }
  }
  else if (altEnPies <= 10000.0f) {
    // ULTRA PRECISO
    if (currentMode != SENSOR_MODE_ULTRA_PRECISO) {
      currentMode = SENSOR_MODE_ULTRA_PRECISO;
      bmp.setTemperatureOversampling(BMP3_OVERSAMPLING_16X);
      bmp.setPressureOversampling(BMP3_OVERSAMPLING_16X);
      bmp.setIIRFilterCoeff(BMP3_IIR_FILTER_COEFF_7); // filtro alto para estabilidad
      bmp.setOutputDataRate(BMP3_ODR_50_HZ);
      Serial.println("Modo Ultra Preciso activado (60 ft <= Altitud <= 10000 ft)");
    }
  }
  else {
    // FREEFALL
    if (currentMode != SENSOR_MODE_FREEFALL) {
      currentMode = SENSOR_MODE_FREEFALL;
      ultraPreciso = false; // coherencia semántica
      bmp.setTemperatureOversampling(BMP3_NO_OVERSAMPLING);   // baja latencia
      bmp.setPressureOversampling(BMP3_OVERSAMPLING_2X);
      bmp.setIIRFilterCoeff(BMP3_IIR_FILTER_DISABLE);         // IIR OFF
      //bmp.setOutputDataRate(BMP3_ODR_200_HZ); //NO AFECTA EN FORCED
      Serial.println("Modo Freefall activado (Altitud > 10000 ft)");
    }

    // Importante: NO llamar performReading() aquí inmediatamente tras reconfigurar.

    // Reversión a ULTRA_PRECISO si baja de 3000 ft (pero mantiene >= 60 ft)
    if (altEnPies < 3000.0f && altEnPies >= 60.0f) {
      currentMode = SENSOR_MODE_ULTRA_PRECISO;
      bmp.setTemperatureOversampling(BMP3_OVERSAMPLING_16X);
      bmp.setPressureOversampling(BMP3_OVERSAMPLING_16X);
      bmp.setIIRFilterCoeff(BMP3_IIR_FILTER_COEFF_7); // solo coeficiente válido
      bmp.setOutputDataRate(BMP3_ODR_50_HZ);
      Serial.println("Reversión de Freefall: Cambio a Ultra Preciso (Altitud < 3000 ft)");
    }
  }

  // --------------------------------------------------
  // 3) Control de la UI: Ahorro de energía en la pantalla
  // --------------------------------------------------
  {
    // Actualizar pantalla cuando no se está en menú, la lectura es correcta
    // y está habilitado el ahorro.
    const bool sensorOk = true; // asumimos OK (ya leímos arriba)
    if (!menuActivo && sensorOk && ahorroTimeoutMs > 0) {
      if (fabs(altitud - lastAltForAhorro) > ALT_CHANGE_THRESHOLD) {
        lastAltForAhorro = altitud;
        lastAltChangeTime = millis();
        // Si la pantalla estaba suspendida, reactivarla
        if (!pantallaEncendida) {
          u8g2.setPowerSave(false);
          pantallaEncendida = true;
          Serial.println("Pantalla reactivada: cambio significativo detectado.");
        }
      } else {
        if ((millis() - lastAltChangeTime) >= ahorroTimeoutMs && pantallaEncendida) {
          u8g2.setPowerSave(true);
          pantallaEncendida = false;
          Serial.println("Modo ahorro: Pantalla suspendida por inactividad.");
        }
      }
    }
  }

  // --------------------------------------------------
  // 4) Notificación vía BLE (si pCharacteristic está inicializado)
  // --------------------------------------------------
if (pCharacteristic != nullptr) {
  static uint32_t t_last_ble = 0;
  const uint32_t ble_interval = (currentMode == SENSOR_MODE_FREEFALL) ? 100 : 250;
  if (millis() - t_last_ble >= ble_interval) {
    long altInt = (long) (altCalculada * 3.281f);
    String altStr = String(altInt);
    pCharacteristic->setValue(altStr.c_str());
    pCharacteristic->notify();
    t_last_ble = millis();
  }
}

}

// ====================================================
// Porcentaje de batería a partir del voltaje
// ====================================================
int calcularPorcentajeBateria(float voltaje) {
  if (voltaje >= 4.2f) return 100;
  if (voltaje >= 4.1f) return 95;
  if (voltaje >= 4.0f) return 90;
  if (voltaje >= 3.9f) return 85;
  if (voltaje >= 3.8f) return 80;
  if (voltaje >= 3.7f) return 75;
  if (voltaje >= 3.6f) return 50;
  if (voltaje >= 3.5f) return 25;
  if (voltaje >= 3.4f) return 10;
  return 5;
}

// ====================================================
// Actualización de la lectura de la batería
// ====================================================
void updateBatteryReading() {
  if (lastBatteryUpdate == 0 || (millis() - lastBatteryUpdate >= batteryUpdateInterval)) {
    lecturaADC = adc1_get_raw(ADC1_CHANNEL_1);
    voltajeADC = (lecturaADC / 4095.0f) * 2.6f;
    const float factorCorreccion = 1.2f;
    v_adc = voltajeADC;
    v_bat = v_adc * 2.0f * factorCorreccion;
    cachedBatteryPercentage = calcularPorcentajeBateria(v_bat);
    lastBatteryUpdate = millis();
  }
}
