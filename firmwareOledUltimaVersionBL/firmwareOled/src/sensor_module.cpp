#include "sensor_module.h"       // Declaraciones públicas de este módulo
#include "config.h"              // BMP_ADDR, pines, etc.
#include "ui_module.h"           // (se mantiene por compatibilidad con UI)
#include <Arduino.h>
#include <Wire.h>
#include <math.h>
#include <Preferences.h>
#include "ble_module.h"
#include "power_lock.h"

// ------------------------------
// Simulación de altitud (OPCIÓN A/B/C)
// ------------------------------
// 1 = activar simulación fija, 2 = perfil por etapas, 3 = rampa triangular, 0 = desactivar
#ifndef ALT_SIM
#define ALT_SIM 0
#endif

// Altura fija en pies para pruebas (p. ej. 0, 80, 2500, 12050)
#ifndef ALT_SIM_FT
#define ALT_SIM_FT 12450.0f
#endif

#ifndef ALT_SIM_MAX_FT
  #define ALT_SIM_MAX_FT 13000.0f   // pico de la rampa
#endif

#ifndef ALT_SIM_PERIOD_MS
  #define ALT_SIM_PERIOD_MS 40000UL  // 40 s: 0→MAX→0 en 40 s
#endif

// ------------------------------
// Histéresis de modos (para evitar flapping)
// ------------------------------
#ifndef MODE_HYST_FT
#define MODE_HYST_FT 30.0f   // ±30 ft alrededor de 60 ft (cambio Ahorro/Ultra)
#endif

// ------------------------------
// Freefall por velocidad vertical (m/s)
// ------------------------------
// Convención: vz > 0 subiendo, vz < 0 bajando
static constexpr float    VZ_ENTER_MPS     = 18.0f;   // entrar a FF si vz <= -18 m/s
static constexpr float    VZ_EXIT_MPS      = 8.0f;    // salir de FF si vz >= -8 m/s
static constexpr uint32_t ENTER_HOLD_MS    = 200;     // sostener condición de entrada
static constexpr uint32_t EXIT_HOLD_MS     = 500;     // sostener condición de salida
static constexpr float    ALT_FILTER_ALPHA = 0.22f;   // filtro IIR de altitud (0.15..0.30)
static constexpr float    MIN_DT_S         = 1e-4f;   // anti-división por cero
// (Opcional) exigir altura mínima para permitir FF por VZ (0=deshabilitado)
static constexpr float    MIN_AGL_FT_FOR_FF = 0.0f;   // p. ej. 300.0f si quieres blindaje extra

// Estado interno vario/FF por VZ
static bool     s_freefallByVZ  = false;
static bool     s_altFiltInit   = false;
static float    s_altFilt       = 0.0f;
static float    s_prevAltFilt   = 0.0f;
static uint32_t s_lastEnterTick = 0;
static uint32_t s_lastExitTick  = 0;
static uint32_t s_lastVarioMs   = 0;

// ------------------------------
// Estado de modo (solo en este .cpp)
// ------------------------------
static SensorMode currentMode = SENSOR_MODE_AHORRO;   // inicia en Ahorro
static unsigned long lastForcedReadingTime = 0;       // para "lectura periódica" en Ahorro

// Exponer el modo actual
SensorMode getSensorMode() {
  return currentMode;
}

// ------------------------------
// Variables externas definidas en otros módulos
// ------------------------------
extern bool calibracionRealizada;
extern float alturaOffset;
extern void onSampleAccepted();   // definida en main.cpp

// ------------------------------
// Objeto para el sensor BMP390 y variables de altitud
// ------------------------------
Adafruit_BMP3XX bmp;
float altitudReferencia = 0.0f;
float altCalculada      = 0.0f;   // relativa (m)
float altitud           = 0.0f;   // absoluta (m)

// ------------------------------
// Variables para armado/salto y NVS
// ------------------------------
bool enSalto = false;         // compat: armado/actividad por altura
bool ultraPreciso = false;    // compat: true=contorno, false=relleno (map a inJump)
bool jumpArmed = false;       // armado por altura (>=60 ft), para UI
bool inJump   = false;        // freefall confirmado, para UI

static bool prevFreefall = false;
static bool freefallArming = false;          // flag de armado de confirmación
static uint32_t freefallSinceMs = 0;
static const uint32_t FF_CONFIRM_MS = 300;   // confirma freefall (0.3 s)

static Preferences prefsSaltos;
uint32_t jumpCount = 0;

// ====================================================
// Helpers: Vario y Freefall por VZ
// ====================================================
static void updateVarioAndFreefall(float altRel_m, float dt_s) {
  // 1) Filtro exponencial de altitud
  if (!s_altFiltInit) {
    s_altFilt     = altRel_m;
    s_prevAltFilt = altRel_m;
    s_altFiltInit = true;
  } else {
    s_prevAltFilt = s_altFilt;
    s_altFilt += ALT_FILTER_ALPHA * (altRel_m - s_altFilt);
  }

  // 2) Velocidad vertical (m/s)
  float vz = 0.0f;
  if (dt_s > MIN_DT_S) {
    vz = (s_altFilt - s_prevAltFilt) / dt_s;
  }

  // 3) Altura mínima opcional para habilitar FF por VZ
  const float agl_ft = altRel_m * 3.281f;
  const bool alturaPermiteFF = (agl_ft >= MIN_AGL_FT_FOR_FF);

  // 4) Debounce temporal para entrar/salir de FREEFALL por vario (solo descenso)
  uint32_t now = millis();

  if (!s_freefallByVZ) {
    // Candidato a entrar: bajando rápido (vz <= -VZ_ENTER_MPS) y altura válida
    if (alturaPermiteFF && (vz <= -VZ_ENTER_MPS)) {
      if (s_lastEnterTick == 0) s_lastEnterTick = now;
      if (now - s_lastEnterTick >= ENTER_HOLD_MS) {
        s_freefallByVZ = true;
        s_lastExitTick = 0;
      }
    } else {
      s_lastEnterTick = 0; // reset si no cumple
    }
  } else {
    // Candidato a salir: ya no bajando tan rápido (vz >= -VZ_EXIT_MPS)
    if (vz >= -VZ_EXIT_MPS) {
      if (s_lastExitTick == 0) s_lastExitTick = now;
      if (now - s_lastExitTick >= EXIT_HOLD_MS) {
        s_freefallByVZ = false;
        s_lastEnterTick = 0;
      }
    } else {
      s_lastExitTick = 0;
    }
  }

  // (Opcional) Telemetría:
  // Serial.printf("vz=%.1f m/s, FF=%d, alt=%.0f ft\n", vz, s_freefallByVZ?1:0, agl_ft);
}

// ====================================================
// Inicialización del sensor
// ====================================================
void initSensor() {
  if (!bmp.begin_I2C(BMP_ADDR)) {
    Serial.println("¡Sensor BMP390L no encontrado!");
    while (1) { delay(10); }
  }

  // Configuración inicial por defecto (arranque en Ahorro)
  bmp.setTemperatureOversampling(BMP3_OVERSAMPLING_8X);
  bmp.setPressureOversampling(BMP3_OVERSAMPLING_32X);
  bmp.setIIRFilterCoeff(BMP3_IIR_FILTER_COEFF_15);
  bmp.setOutputDataRate(BMP3_ODR_25_HZ);

  // Lectura inicial para fijar la altitud de referencia
  if (bmp.performReading()) {
    altitudReferencia = bmp.readAltitude(1013.25);
  }

  // Inicializar el contador de saltos desde NVS (si no existe, se usa 0)
  if (prefsSaltos.begin("saltos", false)) {   // false = RW
    jumpCount = prefsSaltos.getUInt("jumpCount", 0);
    prefsSaltos.end();
  } else {
    Serial.println("NVS: no se pudo abrir 'saltos' en init (RW).");
  }

  s_lastVarioMs = millis();
}

// ====================================================
// Actualización de datos del sensor
// ====================================================
void updateSensorData() {
  // --------------------------------------------------
  // 0) dt para vario (usamos el tiempo entre muestras aceptadas)
  // --------------------------------------------------
  uint32_t nowMs = millis();
  float dt_s = (nowMs - s_lastVarioMs) / 1000.0f;
  if (dt_s < MIN_DT_S) dt_s = MIN_DT_S;

  // --------------------------------------------------
  // 1) Lectura del sensor
  //    - En Ahorro: lectura espaciada (aquí 500 ms para demo, ajusta a 60 s si quieres)
  //    - En otros modos: lectura frecuente
  // --------------------------------------------------
  static bool firstReadingDone = false;
  static uint8_t readFails = 0;   // fallos consecutivos de lectura

  const bool debeLeer =
      !firstReadingDone ||
      (currentMode == SENSOR_MODE_AHORRO && (millis() - lastForcedReadingTime >= 500UL)) ||
      (currentMode != SENSOR_MODE_AHORRO);

  bool sampleCounted = false;  // para onSampleAccepted()

  if (debeLeer) {
    const bool sensorOk = bmp.performReading(); // FORCED/one-shot (bloqueante)
    if (sensorOk) {
      readFails = 0;  // reset de fallos
      const float altActual = bmp.readAltitude(1013.25);
      altitud       = altActual;                                       // absoluto (m)
      altCalculada  = altActual - altitudReferencia + alturaOffset;    // relativa (m)

      // Contador de Hz (instrumentación en main.cpp)
      onSampleAccepted();
      sampleCounted = true;
      s_lastVarioMs = nowMs;  // dt medido entre muestras reales
    } else {
      // Si el sensor falla repetidamente, cancela armado de freefall
      if (++readFails >= 5) {
        freefallArming = false;    // evita confirmar freefall con datos obsoletos
        readFails = 5;             // satura
      }
    }

    if (!firstReadingDone) firstReadingDone = true;

    if (currentMode == SENSOR_MODE_AHORRO) {
      lastForcedReadingTime = millis();
    }
  }

  // --------------------------------------------------
  // 1.b) Simulación de altitud (puede sobrescribir lo anterior)
  // --------------------------------------------------
#if (ALT_SIM == 1)
  {
    // Opción A: altitud fija
    const float simAltM = ALT_SIM_FT / 3.281f;
    altitud      = simAltM;      // absoluto (m)
    altCalculada = simAltM;      // DEMO: sin restar referencia
    if (!sampleCounted) { onSampleAccepted(); sampleCounted = true; }
    s_lastVarioMs = nowMs;
  }
#elif (ALT_SIM == 2)
  {
    // Opción B: perfil por etapas
    struct DemoStage { float feet; uint32_t ms; };
    // Ahorro (<60ft) → Ultra (~80ft) → "alto" → Ahorro (~40ft)
    static const DemoStage kDemo[] = {
      {   0.0f,  4000 },   // 4 s en tierra (Ahorro)
      {  80.0f,  5000 },   // 5 s armado (Ultra)
      {12050.0f, 6000 },   // 6 s alto (antiguo freefall por altura)
      {  40.0f, 6000 },    // 6 s cerca de tierra (Ahorro)
    };
    static const size_t N = sizeof(kDemo)/sizeof(kDemo[0]);
    static size_t stage = 0;
    static uint32_t t0 = 0;

    if (t0 == 0) t0 = nowMs;
    if (nowMs - t0 >= kDemo[stage].ms) {         // avanza de etapa
      stage = (stage + 1) % N;
      t0 = nowMs;
      Serial.printf("[DEMO] etapa=%u, alt=%.0f ft\n", (unsigned)stage, kDemo[stage].feet);
    }

    const float simAltM = kDemo[stage].feet / 3.281f;
    altitud      = simAltM;      // absoluto (m)
    altCalculada = simAltM;      // DEMO
    if (!sampleCounted) { onSampleAccepted(); sampleCounted = true; }
    s_lastVarioMs = nowMs;
  }
#elif (ALT_SIM == 3)
  {
    // Opción C: rampa triangular 0 → MAX → 0 (en pies)
    static uint32_t t0 = 0;
    if (t0 == 0) t0 = nowMs;

    float phase = fmodf((float)(nowMs - t0), (float)ALT_SIM_PERIOD_MS) / (float)ALT_SIM_PERIOD_MS;
    float ft;
    if (phase < 0.5f) {
      ft = (phase * 2.0f) * ALT_SIM_MAX_FT;               // sube
    } else {
      ft = (1.0f - (phase - 0.5f) * 2.0f) * ALT_SIM_MAX_FT; // baja
    }
    const float simAltM = ft / 3.281f;

    altitud      = simAltM;
    altCalculada = simAltM;

    onSampleAccepted();
    sampleCounted = true;
    s_lastVarioMs = nowMs;
  }
#endif

  // --------------------------------------------------
  // 2) Vario + estado FF por VZ
  // --------------------------------------------------
  updateVarioAndFreefall(altCalculada, dt_s);

  // Convertir altitud relativa a pies (para cambio Ahorro/Ultra)
  const float altEnPies = altCalculada * 3.281f;

  // --------------------------------------------------
  // 3) Configuración de modos con HISTÉRESIS
  //     - Ahorro ↔ Ultra: por altitud (60 ft con histéresis)
  //     - Freefall: SOLO por velocidad vertical (s_freefallByVZ)
  // --------------------------------------------------
  const float LOW_ENTER  =  60.0f + MODE_HYST_FT;   // subir desde Ahorro a Ultra
  const float LOW_EXIT   =  60.0f - MODE_HYST_FT;   // bajar desde Ultra a Ahorro

  if (currentMode == SENSOR_MODE_AHORRO) {
    if (altEnPies >= LOW_ENTER) {
      // ULTRA PRECISO
      currentMode = SENSOR_MODE_ULTRA_PRECISO;
      bmp.setTemperatureOversampling(BMP3_OVERSAMPLING_16X);
      bmp.setPressureOversampling(BMP3_OVERSAMPLING_16X);
      bmp.setIIRFilterCoeff(BMP3_IIR_FILTER_COEFF_7);
      bmp.setOutputDataRate(BMP3_ODR_50_HZ);
      Serial.println("Modo Ultra Preciso activado (↑ desde Ahorro)");

      powerLockClear();   // libera lock al cruzar 60 ft
    }

    // Señales de UI/estado en Ahorro
    jumpArmed    = false;
    inJump       = false;
    enSalto      = false;
    ultraPreciso = false;

  } else if (currentMode == SENSOR_MODE_ULTRA_PRECISO) {

    if (s_freefallByVZ) {
      // FREEFALL por velocidad vertical (descenso rápido)
      currentMode = SENSOR_MODE_FREEFALL;
      bmp.setTemperatureOversampling(BMP3_NO_OVERSAMPLING);
      bmp.setPressureOversampling(BMP3_OVERSAMPLING_2X);
      bmp.setIIRFilterCoeff(BMP3_IIR_FILTER_DISABLE);
      // ODR se mantiene en 50 Hz
      Serial.println("Modo Freefall activado (por velocidad vertical)");
      // Mantener señales (se confirman abajo)
      jumpArmed = true;
      enSalto   = true;

    } else if (altEnPies < LOW_EXIT) {
      // AHORRO
      currentMode = SENSOR_MODE_AHORRO;
      bmp.setTemperatureOversampling(BMP3_OVERSAMPLING_8X);
      bmp.setPressureOversampling(BMP3_OVERSAMPLING_32X);
      bmp.setIIRFilterCoeff(BMP3_IIR_FILTER_COEFF_15);
      bmp.setOutputDataRate(BMP3_ODR_25_HZ);
      Serial.println("Modo Ahorro activado (↓ desde Ultra)");
      lastForcedReadingTime = millis();

      // UI/estado al bajar a Ahorro
      jumpArmed    = false;
      inJump       = false;
      enSalto      = false;
      ultraPreciso = false;

    } else {
      // Mantener señales en Ultra
      jumpArmed    = true;       // armado por altura
      enSalto      = true;
      ultraPreciso = true;       // contorno
    }

  } else { // SENSOR_MODE_FREEFALL
    if (!s_freefallByVZ) {
      // ULTRA PRECISO (se pierde condición de FF por VZ)
      currentMode = SENSOR_MODE_ULTRA_PRECISO;
      bmp.setTemperatureOversampling(BMP3_OVERSAMPLING_16X);
      bmp.setPressureOversampling(BMP3_OVERSAMPLING_16X);
      bmp.setIIRFilterCoeff(BMP3_IIR_FILTER_COEFF_7);
      bmp.setOutputDataRate(BMP3_ODR_50_HZ);
      Serial.println("Modo Ultra Preciso activado (salida de Freefall por VZ)");

      // reset parcial de UI (inJump se maneja abajo)
      jumpArmed = true;
      enSalto   = true;
    } else {
      // Mantener señales en Freefall
      jumpArmed = true;
      enSalto   = true;
    }
  }

  // ------------------------------
  // 4) Señales para UI + transición de salto (flanco + confirmación)
  // ------------------------------
  const bool nowFreefall = (currentMode == SENSOR_MODE_FREEFALL);

  // Flanco de subida → armar confirmación
  if (nowFreefall && !prevFreefall) {
    freefallSinceMs = millis();
    freefallArming  = true;
  }

  // Confirmación tras ventana FF_CONFIRM_MS (una sola vez por entrada)
  if (nowFreefall && freefallArming && !inJump &&
      (millis() - freefallSinceMs) >= FF_CONFIRM_MS) {

    inJump = true;  // UI: relleno

    // Persistir en NVS (evento)
    jumpCount++;
    if (prefsSaltos.begin("saltos", false)) {   // false = RW
      prefsSaltos.putUInt("jumpCount", jumpCount);
      prefsSaltos.end();
      Serial.printf("NVS: jumpCount actualizado a %u\n", jumpCount);
    } else {
      Serial.println("NVS: no se pudo abrir 'saltos' (RW) para guardar jumpCount.");
    }

    freefallArming = false; // ya confirmamos este ingreso a freefall
  }

  // Salida de freefall → reset de flags
  if (!nowFreefall) {
    inJump         = false;   // UI: círculo contorno
    freefallArming = false;   // cancelar armado si estaba pendiente
  }

  // Compatibilidad con flags previos
  ultraPreciso = jumpArmed && !inJump;  // true=contorno (armado), false=relleno (en salto)
  prevFreefall = nowFreefall;

  // --------------------------------------------------
  // 5) Notificación vía BLE (si pCharacteristic está inicializado)
  // --------------------------------------------------
  if (pCharacteristic != nullptr) {
    static uint32_t t_last_ble = 0;
    const uint32_t ble_interval = (currentMode == SENSOR_MODE_FREEFALL) ? 100 : 250; // 10 Hz / 4 Hz
    if (millis() - t_last_ble >= ble_interval) {
      long altInt = (long)(altCalculada * 3.281f); // enviar pies como entero
      String altStr = String(altInt);
      pCharacteristic->setValue(altStr.c_str());
      pCharacteristic->notify();
      t_last_ble = millis();
    }
  }
}
