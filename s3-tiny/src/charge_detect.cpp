#include "charge_detect.h"
#include <Arduino.h>
#include <driver/gpio.h>   // para deshabilitar pulls internos en el pin

// ---------------------------------------------------------------------------------
// Compatibilidad de constantes de atenuación entre cores (alias seguro):
#if !defined(ADC_11db) && defined(ADC_ATTEN_DB_11)
  #define ADC_11db ADC_ATTEN_DB_11
#endif
// ---------------------------------------------------------------------------------

// ================== Parámetros ADC (solo NG / Arduino) ==================
#ifndef CHARGE_ADC_BITS
  #define CHARGE_ADC_BITS 12     // 0..4095
#endif

// ================== Divisor de VBUS ==================
// Superior (a VBUS) = 510k, inferior (a GND) = 330k por defecto.
// Puedes sobreescribir con defines CHARGE_R_TOP_KOHM / CHARGE_R_BOT_KOHM.
#ifndef CHARGE_R_TOP_KOHM
  #define CHARGE_R_TOP_KOHM 510.0f
#endif
#ifndef CHARGE_R_BOT_KOHM
  #define CHARGE_R_BOT_KOHM 330.0f
#endif

// Razón de división: Vpin = Vbus * DIV_RATIO
static constexpr float DIV_RATIO = (CHARGE_R_BOT_KOHM / (CHARGE_R_TOP_KOHM + CHARGE_R_BOT_KOHM)); // ≈ 0.3929f

// ================== Umbrales con histéresis (en VBUS, Volts) ==================
#ifndef CHARGE_VBUS_TH_ON
  #define CHARGE_VBUS_TH_ON  3.8f  // entra a “USB presente”
#endif
#ifndef CHARGE_VBUS_TH_OFF
  #define CHARGE_VBUS_TH_OFF 3.2f  // sale de “USB presente”
#endif

// ================== Filtro / debounce ==================
#ifndef CHARGE_CNT_ON_REQ
  #define CHARGE_CNT_ON_REQ  3
#endif
#ifndef CHARGE_CNT_OFF_REQ
  #define CHARGE_CNT_OFF_REQ 3
#endif

// Multimuestreo para cada lectura. Override con define.
#ifndef CHARGE_MSAMPLES
  #define CHARGE_MSAMPLES 8      // 8 lecturas; se descartan min y max
#endif

// =============== Estado interno ===============
static uint8_t s_cntOn  = 0;
static uint8_t s_cntOff = 0;
static bool    s_present = false;
static bool    s_warmed  = false;  // para descartar la primera lectura “fría”

// =============== Helpers (solo driver NG) ===============
static inline int readRawOnce() {
  // Útil para debug; usa igualmente driver NG.
  return analogRead(CHARGE_ADC_PIN);
}

static inline uint32_t readMilliVoltsOnce() {
  // Devuelve mV en el PIN (tras el divisor).
  return (uint32_t)analogReadMilliVolts(CHARGE_ADC_PIN);
}

// Lectura multimuestra con rechazo de extremos (descarta min y max) en mV
static uint32_t multisampleMilliVolts(int n = CHARGE_MSAMPLES) {
  if (n < 3) n = 3; // asegura extremos
  uint32_t minv = 0xFFFFFFFFu;
  uint32_t maxv = 0;
  uint64_t acc  = 0;
  for (int i = 0; i < n; ++i) {
    const uint32_t v = readMilliVoltsOnce();
    acc  += v;
    if (v < minv) minv = v;
    if (v > maxv) maxv = v;
  }
  // descarta extremos
  acc -= minv;
  acc -= maxv;
  const int denom = n - 2;
  return (uint32_t)((denom > 0) ? (acc / (uint64_t)denom) : (acc / (uint64_t)n));
}

// Convierte mV en pin a V en VBUS, con clamps razonables
static inline float pinmV_to_vbusV(uint32_t mV_pin) {
  float v_pin = (float)mV_pin * 0.001f;  // mV -> V
  float vbus  = (DIV_RATIO > 0.0f) ? (v_pin / DIV_RATIO) : 0.0f;
  if (vbus < 0.0f) vbus = 0.0f;
  if (vbus > 6.0f) vbus = 6.0f;          // techo por seguridad
  return vbus;
}

// =============== API ===============
void chargeDetectBegin() {
  // Config ADC NG (Arduino):
  analogReadResolution(CHARGE_ADC_BITS);
  #ifdef analogSetPinAttenuation
    analogSetPinAttenuation(CHARGE_ADC_PIN, ADC_11db);  // abarcar ~3.3V en el pin
  #endif

  // Config GPIO: ¡sin pulls internos! (usamos divisor externo)
  pinMode(CHARGE_ADC_PIN, INPUT);
  gpio_pullup_dis((gpio_num_t)CHARGE_ADC_PIN);
  gpio_pulldown_dis((gpio_num_t)CHARGE_ADC_PIN);

  // “Warm-up”: descarta una lectura inicial que suele ser inestable
  (void)readMilliVoltsOnce();
  s_warmed = true;

  // Resetea estado
  s_cntOn = s_cntOff = 0;
  // No forzamos s_present aquí: que Update lo decida
}

void chargeDetectUpdate() {
  if (!s_warmed) {
    (void)readMilliVoltsOnce();
    s_warmed = true;
  }

  const uint32_t mVpin = multisampleMilliVolts();
  const float    vbus  = pinmV_to_vbusV(mVpin);

  if (!s_present) {
    if (vbus >= CHARGE_VBUS_TH_ON) {
      if (++s_cntOn >= CHARGE_CNT_ON_REQ) {
        s_present = true;
        s_cntOff  = 0;
      }
    } else {
      s_cntOn = 0;
    }
  } else {
    if (vbus <= CHARGE_VBUS_TH_OFF) {
      if (++s_cntOff >= CHARGE_CNT_OFF_REQ) {
        s_present = false;
        s_cntOn   = 0;
      }
    } else {
      s_cntOff = 0;
    }
  }
}

bool isUsbPresent() {
  return s_present;
}

// =============== Debug opcional ===============
// Nota: estos helpers se mantienen por compatibilidad con tus menús/logs.
int   chargeDebugRaw()  { return readRawOnce(); }
float chargeDebugVadc() { return (float)readMilliVoltsOnce() * 0.001f; }  // V en el PIN
float chargeDebugVbus() { return pinmV_to_vbusV(readMilliVoltsOnce()); }  // V en VBUS
