#include <Arduino.h>
#include <Wire.h>
#include "config.h"
#include "ble_module.h"
#include "sensor_module.h"
#include "ui_module.h"
#include "buzzer_module.h"
#include "config.h"

// Variables globales para la UI y menú
bool pantallaEncendida = true;

// Variable para la calibración automática al inicio
bool calibracionRealizada = false;

extern float alturaOffset;

void setup() {
  Serial.begin(115200);
  delay(1000);
  Serial.println("Setup iniciado");
  
  // Marcar firmware actual como válido
  markFirmwareAsValid();
  
  Wire.begin(SDA_PIN, SCL_PIN);
  
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
    
    // Procesar botón ALTITUDE para recalibración: se requiere mantenerlo presionado 3 segundos
    if (digitalRead(BUTTON_ALTITUDE) == LOW) {
      unsigned long startTime = millis();
      // Espera mientras se mantiene presionado el botón
      while(digitalRead(BUTTON_ALTITUDE) == LOW) {
        if (millis() - startTime >= 1000) {  // Si se mantiene por 2 segundos o más
          if (bmp.performReading()) {
            altitudReferencia = bmp.readAltitude(1013.25);
            alturaOffset = 0.0; // Reiniciar offset a cero
            Serial.println("Altitud reiniciada a cero por botón tras 3 segundos.");
            buzzerBeep(2000, 240, 1000); // Sonido de confirmación
          } else {
            Serial.println("Error al leer sensor en recalibración manual.");
          }
          // Esperar a que se libere el botón para evitar múltiples activaciones
          while(digitalRead(BUTTON_ALTITUDE) == LOW) {
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
      while(digitalRead(BUTTON_OLED) == LOW) {
        delay(10);
      }
      delay(50);
    }
  }
  
  delay(101);
}
