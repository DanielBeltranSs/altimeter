#include "sensor_module.h"       // :contentReference[oaicite:0]{index=0}&#8203;:contentReference[oaicite:1]{index=1}
#include "config.h"              // Para usar BMP_ADDR, etc.
#include "ui_module.h"           // Para acceder a u8g2, pantallaEncendida, menuActivo
#include <Arduino.h>
#include <Wire.h>
#include <driver/adc.h>
#include <math.h>
#include <Preferences.h>
#include "ble_module.h"

// Variables externas definidas en otros módulos
extern bool calibracionRealizada;
extern float alturaOffset;

// Objeto para el sensor BMP390 y variables de altitud
Adafruit_BMP3XX bmp;
float altitudReferencia = 0.0;
float altCalculada = 0.0;
float altitud = 0.0;

// Variables para el manejo de la batería
int cachedBatteryPercentage = 0;
unsigned long lastBatteryUpdate = 0;
const unsigned long batteryUpdateInterval = 60000; // 60 segundos

// Variables para el modo ahorro (basado en cambios de altitud)
float lastAltForAhorro = 0;
unsigned long lastAltChangeTime = 0;
const float ALT_CHANGE_THRESHOLD = 2.0;  // Umbral en metros

// Variables para lectura de batería
int lecturaADC = 0;
float voltajeADC = 0.0;
float v_adc = 0.0;
float v_bat = 0.0;

// Variables para la detección del salto
bool enSalto = false;
bool ultraPreciso = false;
static Preferences prefsSaltos;
uint32_t jumpCount = 0;

// ====================================================
// NUEVA LÓGICA PARA CONFIGURACIÓN DE MODOS BASADA EN ALTITUD
// ====================================================
// Se definen tres modos según la altitud (convertida a pies):
//   • Ahorro:         Altitud < 60 ft – se simula un FORCED mode, realizando lectura cada 60 s.
//   • Ultra Preciso:   60 ft <= Altitud <= 10,000 ft – modo NORMAL con 50 Hz y oversampling 16x.
//   • Freefall:        Altitud > 10,000 ft – modo NORMAL con 200 Hz y oversampling reducido (se usa 2X).
// Además, si en Freefall la altitud cae por debajo de 3,000 ft (pero sigue siendo ≥ 60 ft),
// se revierte a Ultra Preciso.
enum SensorMode {
  SENSOR_MODE_AHORRO,
  SENSOR_MODE_ULTRA_PRECISO,
  SENSOR_MODE_FREEFALL
};

static SensorMode currentMode = SENSOR_MODE_AHORRO;  // Se inicializa en Ahorro
static unsigned long lastForcedReadingTime = 0;      // Para simular lectura forzada cada 60 s

// ====================================================
// Función de inicialización del sensor
// ====================================================
void initSensor() {
  if (!bmp.begin_I2C(BMP_ADDR)) {
    Serial.println("¡Sensor BMP390L no encontrado!");
    while (1);
  }
  // Configuración inicial por defecto
  bmp.setTemperatureOversampling(BMP3_OVERSAMPLING_2X);
  bmp.setPressureOversampling(BMP3_OVERSAMPLING_4X);
  bmp.setIIRFilterCoeff(BMP3_IIR_FILTER_COEFF_3);
  bmp.setOutputDataRate(BMP3_ODR_25_HZ);
  
  // Lectura inicial para fijar la altitud de referencia
  if (bmp.performReading()) {
    altitudReferencia = bmp.readAltitude(1013.25);
    lastAltForAhorro = altitudReferencia;
  }
  lastAltChangeTime = millis();
  
  // Inicializar el contador de saltos desde NVS (si no existe, se usa 0)
  prefsSaltos.begin("saltos", false);
  jumpCount = prefsSaltos.getUInt("jumpCount", 0);
  prefsSaltos.end();
}

// ====================================================
// Función para actualizar los datos del sensor
// ====================================================
void updateSensorData() {
  // --------------------------------------------------
  // 1. Lectura del sensor: Realizar la lectura inicial para fijar la altitud de referencia
  // o, si estamos en modo ahorro, solo actualizar cada 60 segundos.
  // --------------------------------------------------
  static bool firstReadingDone = false;
  
  // Si aún no se realizó la lectura inicial o no estamos en modo ahorro, o si ya han pasado 60 s en modo ahorro
  if (!firstReadingDone || (currentMode == SENSOR_MODE_AHORRO && (millis() - lastForcedReadingTime >= 20000)) ||
      (currentMode != SENSOR_MODE_AHORRO)) {
      
    bool sensorOk = bmp.performReading();
    if (sensorOk) {
      float altActual = bmp.readAltitude(1013.25);
      altitud = altActual;
      // Se calcula la altitud relativa: lectura actual menos la referencia más el offset
      altCalculada = altActual - altitudReferencia + alturaOffset;
    }
    if (!firstReadingDone) {
      firstReadingDone = true;
    }
    // Si se está en modo Ahorro, se actualiza el tiempo de lectura forzada.
    if (currentMode == SENSOR_MODE_AHORRO) {
      lastForcedReadingTime = millis();
    }
  }
  
  // Convertir la altitud relativa (en metros) a pies
  float altEnPies = altCalculada * 3.281;
  Serial.print("Altitud: ");
  Serial.print(altEnPies);
  Serial.println(" ft");
  
  // --------------------------------------------------
  // 2. Configuración de modos según la altitud
  // --------------------------------------------------
  if (altEnPies < 60) {
    // Modo Ahorro: Altitud menor a 60 ft
    if (currentMode != SENSOR_MODE_AHORRO) {
      currentMode = SENSOR_MODE_AHORRO;
      // Configuración básica que simula un FORCED mode
      bmp.setTemperatureOversampling(BMP3_OVERSAMPLING_2X);
      bmp.setPressureOversampling(BMP3_OVERSAMPLING_4X);
      bmp.setIIRFilterCoeff(BMP3_IIR_FILTER_COEFF_3);
      bmp.setOutputDataRate(BMP3_ODR_25_HZ);
      Serial.println("Modo Ahorro activado (Altitud < 60 ft)");
      enSalto = false;
      ultraPreciso = false;
      lastForcedReadingTime = millis();
    }
    // En este modo se actualiza la lectura solo cada 60 segundos
  }
  else if (altEnPies <= 10000) {
    // Modo Ultra Preciso: Altitud entre 60 ft y 10,000 ft (lectura continua)

    if (currentMode != SENSOR_MODE_ULTRA_PRECISO) {
      currentMode = SENSOR_MODE_ULTRA_PRECISO;
      bmp.setTemperatureOversampling(BMP3_OVERSAMPLING_16X);
      bmp.setPressureOversampling(BMP3_OVERSAMPLING_16X);
      bmp.setIIRFilterCoeff(BMP3_IIR_FILTER_COEFF_7);
      bmp.setOutputDataRate(BMP3_ODR_50_HZ);
      enSalto = true;
      jumpCount++;
      prefsSaltos.begin("saltos", false);
      prefsSaltos.putUInt("jumpCount", jumpCount);
      prefsSaltos.end();
      Serial.println("Modo Ultra Preciso activado (60 ft <= Altitud <= 10000 ft)");
    }
  }
  else { // altEnPies > 10000
    // Modo Freefall: Altitud mayor a 10,000 ft
    if (currentMode != SENSOR_MODE_FREEFALL) {
      currentMode = SENSOR_MODE_FREEFALL;
      ultraPreciso = true;
      bmp.setTemperatureOversampling(BMP3_OVERSAMPLING_2X); // Se usa 2X (en lugar de 1X)
      bmp.setPressureOversampling(BMP3_OVERSAMPLING_2X);
      // Desactivar o ajustar el filtro según se requiera (en este ejemplo se desactiva)
      bmp.setIIRFilterCoeff(BMP3_IIR_FILTER_DISABLE);
      bmp.setOutputDataRate(BMP3_ODR_200_HZ);
      Serial.println("Modo Freefall activado (Altitud > 10000 ft)");
    }
    bmp.performReading();
    // Reversión: si en Freefall y la altitud cae por debajo de 3000 ft (pero sigue siendo ≥ 60 ft)
    if (altEnPies < 3000 && altEnPies >= 60) {
      currentMode = SENSOR_MODE_ULTRA_PRECISO;
      bmp.setTemperatureOversampling(BMP3_OVERSAMPLING_16X);
      bmp.setPressureOversampling(BMP3_OVERSAMPLING_16X);
      bmp.setIIRFilterCoeff(BMP3_OVERSAMPLING_16X); // Notar: acá se usa el coeficiente definido para alta precisión
      bmp.setIIRFilterCoeff(BMP3_IIR_FILTER_COEFF_7);
      bmp.setOutputDataRate(BMP3_ODR_50_HZ);
      bmp.performReading();
      Serial.println("Reversión de Freefall: Cambio a Ultra Preciso (Altitud < 3000 ft)");
    }
  }
  
  // --------------------------------------------------
  // 3. Control de la UI: Ahorro de energía en la pantalla
  // --------------------------------------------------
  {
    // Se actualiza la pantalla cuando no se está en el menú, la lectura es correcta y está habilitado el ahorro.
    bool sensorOk = true; // Se asume lectura correcta en este punto (ya leímos)
    if (!menuActivo && sensorOk && ahorroTimeoutMs > 0) {
      if (fabs(altitud - lastAltForAhorro) > ALT_CHANGE_THRESHOLD) {
        lastAltForAhorro = altitud;
        lastAltChangeTime = millis();
        // Si la pantalla estaba suspendida, se reactiva automáticamente.
        if (!pantallaEncendida) {
          u8g2.setPowerSave(false);
          pantallaEncendida = true;
          Serial.println("Pantalla reactivada automáticamente: cambio significativo detectado.");
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
  // 4. Notificación vía BLE (si pCharacteristic está inicializado)
  // --------------------------------------------------
  if (pCharacteristic != nullptr) {
    long altInt = (long)altEnPies;
    String altStr = String(altInt);
    pCharacteristic->setValue(altStr.c_str());
    pCharacteristic->notify();
  } else {
    Serial.println("pCharacteristic es null");
  }
}

// ====================================================
// Función para calcular el porcentaje de batería a partir del voltaje
// ====================================================
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

// ====================================================
// Función para actualizar la lectura de la batería
// ====================================================
void updateBatteryReading() {
  if (lastBatteryUpdate == 0 || (millis() - lastBatteryUpdate >= batteryUpdateInterval)) {
    lecturaADC = adc1_get_raw(ADC1_CHANNEL_1);
    voltajeADC = (lecturaADC / 4095.0) * 2.6;
    float factorCorreccion = 1.2;
    v_adc = voltajeADC;
    v_bat = v_adc * 2.0 * factorCorreccion;
    cachedBatteryPercentage = calcularPorcentajeBateria(v_bat);
    lastBatteryUpdate = millis();
  }
}
