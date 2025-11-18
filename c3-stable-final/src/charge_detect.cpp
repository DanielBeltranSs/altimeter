#include "charge_detect.h"
#include <Arduino.h>

// ================== Parámetros ADC (ESP32-C3) ==================
// Permiten override desde config.h o build flags.
#ifndef CHARGE_ADC_BITS
  #define CHARGE_ADC_BITS 12     // 0..4095
#endif
#ifndef CHARGE_VREF_VOLTS
  #define CHARGE_VREF_VOLTS 3.30f  // referencia efectiva ~3.3V
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

// Razón de división: Vadc = Vbus * DIV_RATIO
static constexpr float DIV_RATIO = (CHARGE_R_BOT_KOHM / (CHARGE_R_TOP_KOHM + CHARGE_R_BOT_KOHM)); // ≈ 0.3929f

// ================== Umbrales con histéresis ==================
// Puedes sobreescribir con CHARGE_VBUS_TH_ON / CHARGE_VBUS_TH_OFF.
#ifndef CHARGE_VBUS_TH_ON
  #define CHARGE_VBUS_TH_ON  3.8f  // entra a “USB presente”
#endif
#ifndef CHARGE_VBUS_TH_OFF
  #define CHARGE_VBUS_TH_OFF 3.2f  // sale de “USB presente”
#endif

// ================== Filtro / debounce ==================
// Número de muestras consecutivas requeridas. Override con defines.
#ifndef CHARGE_CNT_ON_REQ
  #define CHARGE_CNT_ON_REQ  3
#endif
#ifndef CHARGE_CNT_OFF_REQ
  #define CHARGE_CNT_OFF_REQ 3
#endif

// Multimuestreo para cada lectura ADC. Override con define.
#ifndef CHARGE_MSAMPLES
  #define CHARGE_MSAMPLES 8      // 8 lecturas; se descartan min y max
#endif

// Calibración opcional (offset/ganancia) sobre Vadc. Útil si el Vref
// efectivo no es 3.30 V. Dejar en 0/1 si no se usa.
#ifndef CHARGE_CAL_OFFSET_V
  #define CHARGE_CAL_OFFSET_V 0.0f
#endif
#ifndef CHARGE_CAL_GAIN
  #define CHARGE_CAL_GAIN 1.0f
#endif

// =============== Estado interno ===============
static uint8_t s_cntOn  = 0;
static uint8_t s_cntOff = 0;
static bool    s_present = false;
static bool    s_warmed  = false;  // para descartar la primera lectura “fría”

// =============== Helpers ===============
static inline int readRawOnce() {
  return analogRead(CHARGE_ADC_PIN);
}

// Lectura multimuestra con rechazo de extremos (descarta min y max)
static int multisampleRaw(int n = CHARGE_MSAMPLES) {
  if (n < 3) n = 3; // asegura que existan extremos a descartar
  int minv =  1 << CHARGE_ADC_BITS;
  int maxv = -1;
  long acc = 0;
  for (int i = 0; i < n; ++i) {
    const int v = readRawOnce();
    acc  += v;
    if (v < minv) minv = v;
    if (v > maxv) maxv = v;
  }
  // descarta extremos
  acc -= minv;
  acc -= maxv;
  const int denom = n - 2;
  return (denom > 0) ? int(acc / denom) : int(acc / n);
}

static inline float rawToVadc(int raw) {
  const float maxc = float((1 << CHARGE_ADC_BITS) - 1);
  float vadc = (raw * CHARGE_VREF_VOLTS) / maxc;
  // Calibración simple
  vadc = (vadc * CHARGE_CAL_GAIN) + CHARGE_CAL_OFFSET_V;
  // clamp a rango plausible
  if (vadc < 0.0f) vadc = 0.0f;
  if (vadc > CHARGE_VREF_VOLTS + 0.2f) vadc = CHARGE_VREF_VOLTS + 0.2f;
  return vadc;
}

static inline float vadcToVbus(float vadc) {
  // Vadc = Vbus * DIV_RATIO
  if (DIV_RATIO <= 0.0f) return 0.0f;
  float vbus = vadc / DIV_RATIO;
  // clamp a rango plausible de VBUS (0..6.0V por si hay ruido/pullups)
  if (vbus < 0.0f) vbus = 0.0f;
  if (vbus > 6.0f) vbus = 6.0f;
  return vbus;
}

// =============== API ===============
void chargeDetectBegin() {
  analogReadResolution(CHARGE_ADC_BITS);

  // Atenuación por pin a 11 dB para abarcar ~3.3 V en ADC (Arduino-ESP32)
  #ifdef analogSetPinAttenuation
    analogSetPinAttenuation(CHARGE_ADC_PIN, ADC_11db);
  #endif

  pinMode(CHARGE_ADC_PIN, INPUT);

  // “Warm-up”: descarta una lectura inicial que suele ser inestable
  (void)readRawOnce();
  s_warmed = true;

  // Resetea estado
  s_cntOn = s_cntOff = 0;
  // No forzamos s_present aquí: que Update decida tras primeras lecturas estables
}

void chargeDetectUpdate() {
  if (!s_warmed) {
    (void)readRawOnce();
    s_warmed = true;
  }

  const int   raw  = multisampleRaw();
  const float vadc = rawToVadc(raw);
  const float vbus = vadcToVbus(vadc);

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
int   chargeDebugRaw()  { return multisampleRaw(); }
float chargeDebugVadc() { return rawToVadc(multisampleRaw()); }
float chargeDebugVbus() { return vadcToVbus(chargeDebugVadc()); }
