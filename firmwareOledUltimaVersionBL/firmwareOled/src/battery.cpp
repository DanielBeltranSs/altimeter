#include "battery.h"
#include "config.h"
#include <driver/adc.h>

// ========================= Config =========================
#ifndef BATTERY_ADC_ATTEN
#define BATTERY_ADC_ATTEN ADC_ATTEN_DB_12
#endif

#ifndef BATTERY_ADC_WIDTH
#define BATTERY_ADC_WIDTH ADC_WIDTH_BIT_12
#endif

#ifndef BATTERY_DIVIDER_RATIO
#define BATTERY_DIVIDER_RATIO 2.0f
#endif

#ifndef BATTERY_UPDATE_INTERVAL_MS
#define BATTERY_UPDATE_INTERVAL_MS 1000UL
#endif

// Umbrales pedidos
static constexpr float VBAT_FULL_V     = 4.15f;
static constexpr float VBAT_EMPTY_V    = 3.40f;
static constexpr float VBAT_DEEPSLEEP  = 3.36f;
static constexpr int   LOW_PERCENT_THR = 5;

// Constantes ADC
static constexpr float ADC_MAX_COUNTS  = 4095.0f;
static constexpr float ADC_FS_PIN_VOLT = 3.1f;

// ========================= Estado interno =========================
static adc1_channel_t s_adcChan = ADC1_CHANNEL_1;  // usa el canal que ya tienes configurado

static float s_vbat    = 0.0f;
static int   s_percent = 0;
static unsigned long s_tLast = 0;

// ========================= Utilidades =========================
static int multisampleRaw(uint8_t n = 8) {
  uint32_t acc = 0;
  for (uint8_t i = 0; i < n; ++i) {
    acc += (uint32_t)adc1_get_raw(s_adcChan);
    delayMicroseconds(200);
  }
  return (int)(acc / (uint32_t)n);
}

static float rawToVadc(int raw) {
  return (raw / ADC_MAX_COUNTS) * ADC_FS_PIN_VOLT;
}

static float vadcToVbat(float v_adc) {
  return v_adc * BATTERY_DIVIDER_RATIO;
}

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
  adc1_config_width(BATTERY_ADC_WIDTH);
  adc1_config_channel_atten(s_adcChan, BATTERY_ADC_ATTEN);

  int raw   = multisampleRaw(8);
  float vadc = rawToVadc(raw);
  s_vbat    = vadcToVbat(vadc);
  s_percent = voltageToPercent(s_vbat);
  s_tLast   = millis();
}

void batteryUpdate() {
  unsigned long now = millis();
  if ((now - s_tLast) < BATTERY_UPDATE_INTERVAL_MS) return;
  s_tLast = now;

  int raw   = multisampleRaw(8);
  float vadc = rawToVadc(raw);
  s_vbat    = vadcToVbat(vadc);
  s_percent = voltageToPercent(s_vbat);
}

float batteryGetVoltage() { return s_vbat; }
int   batteryGetPercent() { return s_percent; }
bool  batteryIsLowPercent() { return (s_percent <= LOW_PERCENT_THR); }

// --- Wake por batería desactivado temporalmente ---
bool  batteryShouldDeepSleep() {
  return (s_vbat <= VBAT_DEEPSLEEP); 
}