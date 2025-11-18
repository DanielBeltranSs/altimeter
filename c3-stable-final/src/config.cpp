#include "config.h"
#include <Arduino.h>
#include <math.h>      // fabsf

Preferences prefs;

// -------------------------
// Variables globales (persistentes vía NVS)
// -------------------------
bool  unidadMetros      = false;
int   brilloPantalla    = 255;
int   altFormat         = 0;

// Tabla de timeouts y su cantidad
const long TIMEOUT_OPTIONS[] = { 0, 60000, 1200000, 1500000 };
const int  NUM_TIMEOUT_OPTIONS = sizeof(TIMEOUT_OPTIONS) / sizeof(TIMEOUT_OPTIONS[0]);

int  ahorroTimeoutOption = 0;                 // índice en TIMEOUT_OPTIONS
unsigned long ahorroTimeoutMs = TIMEOUT_OPTIONS[0];

bool   inversionActiva   = true;
String usuarioActual     = "elDani";          // Nombre de usuario por defecto

float  alturaOffset      = 0.0f;              // SIEMPRE en METROS

int idioma = LANG_ES;  // 0 = ES (default), 1 = EN

// ===== Auto Ground Zero (AGZ) =====
float agzBias = 0.0f;                         // Sesgo en METROS (persistente)

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

  // Cargar y sanear offset (rango y casi-cero)
  alturaOffset        = prefs.getFloat("alturaOffset", 0.0f);
  if (alturaOffset < ALTURA_OFFSET_MIN_M) alturaOffset = ALTURA_OFFSET_MIN_M;
  if (alturaOffset > ALTURA_OFFSET_MAX_M) alturaOffset = ALTURA_OFFSET_MAX_M;
  if (fabsf(alturaOffset) < OFFSET_ZERO_EPS_M) alturaOffset = 0.0f;

  idioma              = prefs.getInt("lang", LANG_ES);

  // ===== Cargar y sanear AGZ =====
  agzBias             = prefs.getFloat("agzBias", 0.0f);
  if (agzBias >  AGZ_BIAS_CLAMP_M) agzBias =  AGZ_BIAS_CLAMP_M;
  if (agzBias < -AGZ_BIAS_CLAMP_M) agzBias = -AGZ_BIAS_CLAMP_M;

  prefs.end();
}

void saveConfig() {
  prefs.begin("config", false);

  // Clamp antes de guardar para mantener coherencia
  ahorroTimeoutOption = clampTimeoutIndex(ahorroTimeoutOption);
  ahorroTimeoutMs     = TIMEOUT_OPTIONS[ahorroTimeoutOption];

  // Saneamos y normalizamos el offset previo al guardado
  if (alturaOffset < ALTURA_OFFSET_MIN_M) alturaOffset = ALTURA_OFFSET_MIN_M;
  if (alturaOffset > ALTURA_OFFSET_MAX_M) alturaOffset = ALTURA_OFFSET_MAX_M;
  if (fabsf(alturaOffset) < OFFSET_ZERO_EPS_M) alturaOffset = 0.0f;

  // Saneamos AGZ antes de persistir
  if (agzBias >  AGZ_BIAS_CLAMP_M) agzBias =  AGZ_BIAS_CLAMP_M;
  if (agzBias < -AGZ_BIAS_CLAMP_M) agzBias = -AGZ_BIAS_CLAMP_M;

  // Persistir parámetros
  prefs.putBool("unit", unidadMetros);
  prefs.putInt ("brillo", brilloPantalla);
  prefs.putInt ("altFormat", altFormat);
  prefs.putInt ("ahorro", ahorroTimeoutOption);
  prefs.putBool("invert", inversionActiva);
  prefs.putFloat("alturaOffset", alturaOffset);   // <-- en METROS
  prefs.putInt ("lang", idioma);

  // Persistir AGZ (también se puede guardar con saveAgzBias() cuando cambie)
  prefs.putFloat("agzBias", agzBias);

  prefs.end();
}

void loadUserConfig() {
  prefs.begin("config", false);
  usuarioActual = prefs.getString("user", "elDani");
  prefs.end();
}

// ===== Guardado ligero SOLO de AGZ (minimiza desgaste de NVS) =====
void saveAgzBias() {
  // Clampea por seguridad y guarda sólo el sesgo del auto-cero
  if (agzBias >  AGZ_BIAS_CLAMP_M) agzBias =  AGZ_BIAS_CLAMP_M;
  if (agzBias < -AGZ_BIAS_CLAMP_M) agzBias = -AGZ_BIAS_CLAMP_M;

  prefs.begin("config", false);
  prefs.putFloat("agzBias", agzBias);
  prefs.end();
}
