#include "config.h"
#include <Arduino.h>

Preferences prefs;

// -------------------------
// Variables globales (persistentes vía NVS)
// -------------------------
bool unidadMetros = false;
int  brilloPantalla = 255;
int  altFormat = 0;

// Tabla de timeouts y su cantidad
const long TIMEOUT_OPTIONS[] = { 0, 60000, 1200000, 1500000 };
const int  NUM_TIMEOUT_OPTIONS = sizeof(TIMEOUT_OPTIONS) / sizeof(TIMEOUT_OPTIONS[0]);

int  ahorroTimeoutOption = 0;                 // índice en TIMEOUT_OPTIONS
unsigned long ahorroTimeoutMs = TIMEOUT_OPTIONS[0];

bool   inversionActiva = true;
// String usuarioActual = "default_user";
String usuarioActual = "elDani";              // Nombre de usuario por defecto
bool   bleActivo = true;

float  alturaOffset = 0.0;

int idioma = LANG_ES;  // 0 = ES (default), 1 = EN

// -------------------------
// Helpers locales
// -------------------------
static inline int clampTimeoutIndex(int idx) {
  if (idx < 0) return 0;
  if (idx >= NUM_TIMEOUT_OPTIONS) return 0;   // fallback seguro si hay datos corruptos en NVS
  return idx;
}

// -------------------------
// Carga/guarda de configuración
// -------------------------
void loadConfig() {
  prefs.begin("config", false);

  unidadMetros        = prefs.getBool("unit", false);
  brilloPantalla      = prefs.getInt("brillo", 255);
  altFormat           = prefs.getInt("altFormat", 0);

  // Asegurar que el índice leído es válido antes de indexar el arreglo
  ahorroTimeoutOption = clampTimeoutIndex(prefs.getInt("ahorro", 0));
  ahorroTimeoutMs     = TIMEOUT_OPTIONS[ahorroTimeoutOption];

  inversionActiva     = prefs.getBool("invert", true);
  alturaOffset        = prefs.getFloat("alturaOffset", 0.0);

  idioma              = prefs.getInt("lang", LANG_ES);

  prefs.end();
}

void saveConfig() {
  prefs.begin("config", false);

  // Clamp antes de guardar para mantener coherencia
  ahorroTimeoutOption = clampTimeoutIndex(ahorroTimeoutOption);
  ahorroTimeoutMs     = TIMEOUT_OPTIONS[ahorroTimeoutOption];

  prefs.putBool("unit", unidadMetros);
  prefs.putInt("brillo", brilloPantalla);
  prefs.putInt("altFormat", altFormat);
  prefs.putInt("ahorro", ahorroTimeoutOption);
  prefs.putBool("invert", inversionActiva);
  prefs.putFloat("alturaOffset", alturaOffset);
  prefs.putInt("lang", idioma);
  prefs.end();
}

void loadUserConfig() {
  prefs.begin("config", false);
  usuarioActual = prefs.getString("user", "elDani");
  prefs.end();
}

// Marcar firmware como válido tras un arranque correcto (evita rollback OTA)
void markFirmwareAsValid() {
  esp_err_t err = esp_ota_mark_app_valid_cancel_rollback();
  if (err != ESP_OK) {
    Serial.printf("Error al marcar la app como válida: %s\n", esp_err_to_name(err));
  } else {
    Serial.println("Firmware actual marcado como válido.");
  }
}
