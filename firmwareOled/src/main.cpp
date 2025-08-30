#include <Arduino.h>
#include <Wire.h>
#include "config.h"
#include "ble_module.h"
#include "sensor_module.h"
#include "ui_module.h"
#include "buzzer_module.h"

// ==========================
// Instrumentación de Hz (Opción 1)
// ==========================
#ifndef DEBUG_HZ
#define DEBUG_HZ 1
#endif

#if DEBUG_HZ
volatile uint32_t g_samples = 0;     // incrementa al aceptar una lectura válida
unsigned long g_t_last = 0;          // millis() del último reporte

// Altitud relativa calculada (viene de sensor_module.cpp)
extern float altCalculada;

// ¡Función normal (NO inline)!
void onSampleAccepted() {
  g_samples++;
}

static void hzReportTick(int modoEstimado /* 0: Ahorro, 1: Ultra, 2: Freefall */) {
  unsigned long now = millis(); // patrón rollover-safe
  if (now - g_t_last >= 1000UL) {
    Serial.printf("[HZ] modo=%d  Hz=%lu\n", modoEstimado, (unsigned long)g_samples);
    g_samples = 0;
    g_t_last = now;
  }
}
#endif
// ==========================

// Variables globales para la UI y menú
bool pantallaEncendida = true;

// Variable para la calibración automática al inicio
bool calibracionRealizada = false;

extern float alturaOffset;

// Referencias a globals del sensor (definidas en sensor_module.cpp)
extern Adafruit_BMP3XX bmp;
extern float altitudReferencia;

void setup() {
  Serial.begin(115200);
  delay(1000);
  Serial.println("Setup iniciado");
  
  // Marcar firmware actual como válido
  markFirmwareAsValid();
  
  Wire.begin(SDA_PIN, SCL_PIN);
  Wire.setClock(400000);  // 400 kHz; el BMP390 soporta hasta 3.4 MHz

  
  // Cargar configuración y datos de usuario
  loadConfig();
  loadUserConfig();
  
  initBuzzer();

  setupBLE();
  initUI();
  initSensor();
  
  // Configurar pines de botones
  pinMode(BUTTON_ALTITUDE, INPUT_PULLUP);
  pinMode(BUTTON_OLED, INPUT_PULLUP);
  pinMode(BUTTON_MENU, INPUT_PULLUP);
  
  // Configuración del ADC para lectura de batería
  adc1_config_width(ADC_WIDTH_BIT_12);
  adc1_config_channel_atten(ADC1_CHANNEL_1, ADC_ATTEN_DB_12);
  
  Serial.println("Setup completado");
}

void loop() {
  updateSensorData();
  updateUI();
  updateBatteryReading();
  
  // Calibración automática al inicio (se realiza solo una vez)
  if (!calibracionRealizada) {
    if (bmp.performReading()) {
      altitudReferencia = bmp.readAltitude(1013.25);
      Serial.println("Calibración inicial: altitud reiniciada a cero.");
#if DEBUG_HZ
      onSampleAccepted(); // contamos también esta lectura válida
#endif
    } else {
      Serial.println("Error al leer sensor en calibración inicial.");
    }
    calibracionRealizada = true;
  }
  
  // Procesar menú si está activo
  if (menuActivo) {
    processMenu();
  } else {
    // Manejar botón MENU para activar el menú
    if (digitalRead(BUTTON_MENU) == LOW) {
      menuActivo = true;
      menuOpcion = 0;
      lastMenuInteraction = millis();
      delay(50);
    }
    
    // Procesar botón ALTITUDE para recalibración:
    // mantener presionado ~1 s (ajusta a 3000 ms si quieres 3 s)
    if (digitalRead(BUTTON_ALTITUDE) == LOW) {
      unsigned long startTime = millis();
      // Espera mientras se mantiene presionado el botón
      while (digitalRead(BUTTON_ALTITUDE) == LOW) {
        if (millis() - startTime >= 1000) {  // 1 segundo
          if (bmp.performReading()) {
            altitudReferencia = bmp.readAltitude(1013.25);
            alturaOffset = 0.0; // Reiniciar offset a cero
            Serial.println("Altitud reiniciada a cero por botón tras 1 segundo.");
#if DEBUG_HZ
            onSampleAccepted(); // contamos también esta lectura válida
#endif
            buzzerBeep(2000, 240, 1000); // Sonido de confirmación
          } else {
            Serial.println("Error al leer sensor en recalibración manual.");
          }
          // Esperar a que se libere el botón para evitar múltiples activaciones
          while (digitalRead(BUTTON_ALTITUDE) == LOW) {
            delay(10);
          }
          delay(50);
          break;
        }
      }
    }
    
    // Manejar botón OLED para alternar encendido/apagado de la pantalla (acción inmediata)
    if (digitalRead(BUTTON_OLED) == LOW) {
      pantallaEncendida = !pantallaEncendida;
      lastAltChangeTime = millis();
      if (pantallaEncendida) {
        u8g2.setPowerSave(false);
      } else {
        u8g2.setPowerSave(true);
      }
      Serial.println("Pantalla alternada por botón OLED.");
      // Esperar a que se libere el botón
      while (digitalRead(BUTTON_OLED) == LOW) {
        delay(10);
      }
      delay(50);
    }
  }

  // ==========================
  // Reporte de Hz 1 vez/segundo (estimación por altitud relativa)
  // ==========================
#if DEBUG_HZ
  int modoEstimado = 1; // por defecto Ultra Preciso
  float altFeet = altCalculada * 3.281f; // altitud relativa (m) -> pies
  if (altFeet < 60.0f)       modoEstimado = 0;     // Ahorro
  else if (altFeet > 10000)  modoEstimado = 2;     // Freefall
  hzReportTick(modoEstimado);
#endif
  
  // ==========================
  // Delay dinámico según el modo real del sensor
  // ==========================
  SensorMode modo = getSensorMode();

  // No limitar el muestreo en caída libre:
  uint16_t baseDelay = 101;                    // Ahorro: UI tranquila
  if (modo == SENSOR_MODE_ULTRA_PRECISO) baseDelay = 10; // Ultra: pequeño respiro a la UI
  if (modo == SENSOR_MODE_FREEFALL)     baseDelay = 0;   // Freefall: máxima cadencia
  delay(baseDelay);
}
