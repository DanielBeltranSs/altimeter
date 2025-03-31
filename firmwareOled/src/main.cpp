#include <Arduino.h>
#include <Wire.h>
#include "config.h"
#include "ble_module.h"
#include "sensor_module.h"
#include "ui_module.h"

// Variables globales para la UI y menú (pueden definirse en ui_module.cpp también)
int menuOpcion = 0;
bool menuActivo = false;
bool pantallaEncendida = true;


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
  
  setupBLE();
  initUI();
  initSensor();
  
  // Configurar pines de botones
  pinMode(BUTTON_ALTITUDE, INPUT_PULLUP);
  pinMode(BUTTON_OLED, INPUT_PULLUP);
  pinMode(BUTTON_MENU, INPUT_PULLUP);
  
  // Lectura inicial del sensor para fijar altitud de referencia (se realiza en initSensor)
  
  Serial.println("Setup completado");
}

void loop() {
  // Actualizar datos del sensor (lectura del BMP390 y otros si se requieren)
  updateSensorData();
  
  // Actualizar la UI (puede mostrar menú, altitud, batería, etc.)
  updateUI();
  
  // Manejo básico de botones para el menú y otras funciones
  if (digitalRead(BUTTON_MENU) == LOW) {
    menuActivo = !menuActivo;
    delay(50);
  }
  if (digitalRead(BUTTON_ALTITUDE) == LOW) {
    if (menuActivo) {
      menuOpcion = (menuOpcion + 1) % TOTAL_OPCIONES;
      delay(50);
    } else {
      // Por ejemplo, reiniciar altitud de referencia
      if (bmp.performReading()) {
        altitudReferencia = bmp.readAltitude(1013.25);
        Serial.println("Altitud reiniciada a cero.");
      }
      delay(50);
    }
  }
  if (digitalRead(BUTTON_OLED) == LOW) {
    if (!menuActivo) {
      pantallaEncendida = !pantallaEncendida;
      if (pantallaEncendida)
        u8g2.setPowerSave(false);
      else
        u8g2.setPowerSave(true);
      delay(50);
    }
  }
  
  updateBatteryReading();
  
  // Aquí se podría actualizar la característica BLE con la altitud, etc.
  
  delay(101);
}
