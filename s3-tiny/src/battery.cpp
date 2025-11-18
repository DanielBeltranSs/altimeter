// battery.cpp  (ESP32-S3: ADC "NG" via Arduino)
// Usa solo analogRead*/analogSetPinAttenuation (sin adc1_*/esp_adc_cal_*)

#include <Arduino.h>     // millis(), delayMicroseconds(), analog*
#include <math.h>        // floorf()
#include "battery.h"
#include "config.h"
#include "charge_detect.h"  // isUsbPresent()

// ---------------------------------------------------------------------------------
// Compatibilidad de constantes de atenuación entre cores (alias seguro):
#if !defined(ADC_11db) && defined(ADC_ATTEN_DB_11)
  #define ADC_11db ADC_ATTEN_DB_11
#endif
// ---------------------------------------------------------------------------------

// ========================= Config =========================
#ifndef BATTERY_ADC_ATTEN
#define BATTERY_ADC_ATTEN ADC_11db     // ~3.3V full-scale en el pin
#endif

#ifndef BATTERY_ADC_BITS
#define BATTERY_ADC_BITS 12            // resolución del ADC
#endif

#ifndef BATTERY_DIVIDER_RATIO
#define BATTERY_DIVIDER_RATIO 2.0f     // (Rtop+Rbot)/Rbot  -> ej: 1M/1M => 2.0
#endif

#ifndef BATTERY_UPDATE_INTERVAL_MS
#define BATTERY_UPDATE_INTERVAL_MS 1000UL
#endif

// Umbrales pedidos
static constexpr float VBAT_FULL_V     = 4.15f;
static constexpr float VBAT_EMPTY_V    = 3.40f;
static constexpr float VBAT_DEEPSLEEP  = 3.36f;
static constexpr int   LOW_PERCENT_THR = 5;

// ========================= Estado interno =========================
static float         s_vbat      = 0.0f;  // Volts
static int           s_percent   = 0;     // % crudo (lineal por tensión)
static unsigned long s_tLast     = 0;

static int s_pctDisplay          = -1;    // % mostrado (monótono)

// ========================= Utilidades =========================
static inline uint32_t multisampleMilliVolts(uint8_t n = 8) {
  uint32_t acc = 0;
  for (uint8_t i = 0; i < n; ++i) {
    acc += (uint32_t)analogReadMilliVolts(BATTERY_PIN);
    delayMicroseconds(200);   // filtro simple por multisampling
  }
  return acc / (uint32_t)n;   // mV en el PIN (antes del divisor)
}

static inline float pinmV_to_vbatV(uint32_t mV_pin) {
  // pin ve Vbat / BATTERY_DIVIDER_RATIO
  const float v_pin = (float)mV_pin * 0.001f;      // mV -> V
  return v_pin * BATTERY_DIVIDER_RATIO;            // Vbat estimada
}

// Mapeo lineal solicitado: 3.40 V -> 0%, 4.15 V -> 100%
static int voltageToPercent(float vbat) {
  if (vbat <= VBAT_EMPTY_V) return 0;
  if (vbat >= VBAT_FULL_V)  return 100;
  const float span = VBAT_FULL_V - VBAT_EMPTY_V;
  float p = (vbat - VBAT_EMPTY_V) * (100.0f / span);
  int pct = (int)floorf(p + 1e-6f);
  return constrain(pct, 0, 100);
}

// ========================= API =========================
void batteryInit() {
  // Configura ADC "NG" de Arduino (sin legacy):
  pinMode(BATTERY_PIN, INPUT);
  analogReadResolution(BATTERY_ADC_BITS);
  analogSetPinAttenuation(BATTERY_PIN, BATTERY_ADC_ATTEN);

  const uint32_t mV  = multisampleMilliVolts(8);
  s_vbat             = pinmV_to_vbatV(mV);
  s_percent          = voltageToPercent(s_vbat);
  s_pctDisplay       = s_percent;     // inicializa el % mostrado
  s_tLast            = millis();
}

void batteryUpdate() {
  const unsigned long now = millis();
  if ((now - s_tLast) < BATTERY_UPDATE_INTERVAL_MS) return;
  s_tLast = now;

  const uint32_t mV  = multisampleMilliVolts(8);
  s_vbat             = pinmV_to_vbatV(mV);
  s_percent          = voltageToPercent(s_vbat);
}

float batteryGetVoltage() {
  return s_vbat;
}

int batteryGetPercent() {
  // % crudo según tu mapeo lineal
  int pct = s_percent;
  if (pct < 0)   pct = 0;
  if (pct > 100) pct = 100;

  // Inicializa filtro monótono la primera vez (por seguridad extra)
  if (s_pctDisplay < 0) {
    s_pctDisplay = pct;
    return s_pctDisplay;
  }

  // Monotonía según estado de carga
  if (isUsbPresent()) {
    // Cargando: solo puede SUBIR
    if (pct > s_pctDisplay) s_pctDisplay = pct;
  } else {
    // Sin cargador: solo puede BAJAR
    if (pct < s_pctDisplay) s_pctDisplay = pct;
  }

  return s_pctDisplay;
}

bool batteryIsLowPercent() {
  // Recomendado: no alertar “baja” si está cargando
  if (isUsbPresent()) return false;
  return (s_pctDisplay <= LOW_PERCENT_THR);
}

// --- Deep sleep por batería (bloqueado si hay USB) ---
bool batteryShouldDeepSleep() {
  if (isUsbPresent()) return false;
  return (s_vbat <= VBAT_DEEPSLEEP);
}
