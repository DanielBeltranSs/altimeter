#include "config.h"
#include <Arduino.h>

Preferences prefs;

// Inicialización de variables globales
bool unidadMetros = false;
int brilloPantalla = 255;
int altFormat = 0;
int ahorroTimeoutOption = 0;
const long TIMEOUT_OPTIONS[] = {0, 60000, 120000, 300000};

unsigned long ahorroTimeoutMs = TIMEOUT_OPTIONS[0];
bool inversionActiva = true;
String usuarioActual = "default_user";
bool bleActivo = true;

float alturaOffset = 0.0;


const int NUM_TIMEOUT_OPTIONS = 4;


void loadConfig() {
  prefs.begin("config", false);
  unidadMetros   = prefs.getBool("unit", false);
  brilloPantalla = prefs.getInt("brillo", 255);
  altFormat      = prefs.getInt("altFormat", 0);
  ahorroTimeoutOption = prefs.getInt("ahorro", 0);
  ahorroTimeoutMs = TIMEOUT_OPTIONS[ahorroTimeoutOption];
  inversionActiva = prefs.getBool("invert", true);
  alturaOffset = prefs.getFloat("alturaOffset", 0.0);
  prefs.end();
}

void saveConfig() {
  prefs.begin("config", false);
  prefs.putBool("unit", unidadMetros);
  prefs.putInt("brillo", brilloPantalla);
  prefs.putInt("altFormat", altFormat);
  prefs.putInt("ahorro", ahorroTimeoutOption);
  prefs.putBool("invert", inversionActiva);
  prefs.putFloat("alturaOffset", alturaOffset);
  prefs.end();
}

void loadUserConfig() {
  prefs.begin("config", false);
  usuarioActual = prefs.getString("user", "default_user");
  prefs.end();
}

void markFirmwareAsValid() {
  esp_err_t err = esp_ota_mark_app_valid_cancel_rollback();
  if (err != ESP_OK) {
    Serial.printf("Error al marcar la app como válida: %s\n", esp_err_to_name(err));
  } else {
    Serial.println("Firmware actual marcado como válido.");
  }
}
