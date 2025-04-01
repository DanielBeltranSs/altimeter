#include <Arduino.h>
#include <Wire.h>
#include "config.h"
#include "ble_module.h"
#include "sensor_module.h"
#include "ui_module.h"

// Variables globales para la UI y menú (pueden definirse en ui_module.cpp también)

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
  updateSensorData();
  updateUI();

  // Si el menú está activo, procesarlo
  if (menuActivo) {
    processMenu();
  } else {
    // Si no está activo el menú, manejar otros botones
    if (digitalRead(BUTTON_MENU) == LOW) {
      menuActivo = true;
      menuOpcion = 0;
      lastMenuInteraction = millis();
      delay(50);
    }
    if (digitalRead(BUTTON_ALTITUDE) == LOW) {
      // En modo normal, por ejemplo, reiniciar altitud
      if (bmp.performReading()) {
        altitudReferencia = bmp.readAltitude(1013.25);
        Serial.println("Altitud reiniciada a cero.");
      }
      delay(50);
    }
    if (digitalRead(BUTTON_OLED) == LOW) {
      // Aquí se alterna el encendido/apagado de la pantalla si no hay menú activo
      pantallaEncendida = !pantallaEncendida;
      lastAltChangeTime = millis();
      if (pantallaEncendida)
        u8g2.setPowerSave(false);
      else
        u8g2.setPowerSave(true);
      delay(50);
    }
  }

  updateBatteryReading();
  delay(101);
}
