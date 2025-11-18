#include "sensor_module.h"       // Declaraciones públicas de este módulo
#include "config.h"              // BMP_ADDR, pines, etc.
#include "ui_module.h"           // (compatibilidad con UI)
#include <Arduino.h>
#include <Wire.h>
#include <math.h>
#include "power_lock.h"
#include "logbook.h"             // Integración de bitácora
#include "nvs.h"
#include "nvs_flash.h"


// ------------------------------
// Simulación de altitud (OPCIÓN A/B/C)
// ------------------------------
#ifndef ALT_SIM
#define ALT_SIM 0
#endif

#ifndef ALT_SIM_FT
#define ALT_SIM_FT 12450.0f
#endif

#ifndef ALT_SIM_MAX_FT
  #define ALT_SIM_MAX_FT 13000.0f
#endif

#ifndef ALT_SIM_PERIOD_MS
  #define ALT_SIM_PERIOD_MS 40000UL
#endif

// ------------------------------
// Histéresis de modos (para evitar flapping)
// ------------------------------
#ifndef MODE_HYST_FT
#define MODE_HYST_FT 30.0f   // ±30 ft alrededor de 60 ft
#endif

// ------------------------------
// Freefall por velocidad vertical (m/s)
// ------------------------------
// Convención: vz > 0 subiendo, vz < 0 bajando
static constexpr float    VZ_ENTER_MPS     = 18.0f;   // entrar a FF si vz <= -18 m/s
static constexpr float    VZ_EXIT_MPS      = 8.0f;    // salir de FF si vz >= -8 m/s
static constexpr uint32_t ENTER_HOLD_MS    = 200;     // sostener condición de entrada
static constexpr uint32_t EXIT_HOLD_MS     = 500;     // sostener condición de salida
static constexpr float    MIN_DT_S         = 1e-4f;   // anti-división por cero

// (1) Altura mínima para habilitar FF por VZ (blindaje contra falsos positivos)
static constexpr float    MIN_AGL_FT_FOR_FF = 300.0f; // **Aplicado**

// ---- Umbrales de cierre robusto (parametrizables si quieres moverlos a config.h)
static constexpr float    GROUND_ALT_M          = 3.0f;                     // ±3 m
static constexpr float    GROUND_VZ_MPS         = 0.3f;                     // |VZ| < 0.3 m/s
static constexpr uint32_t GROUND_STABLE_MS      = 1000UL;                   // 1 s
static constexpr float    POSTDEPLOY_LOWALT_FT  = 50.0f;                    // < 50 ft
static constexpr uint32_t POSTDEPLOY_WATCHDOG_MS= (5UL * 60UL * 1000UL);    // 5 min

// ------------------------------
// Estado interno vario/FF por VZ
// ------------------------------
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
// Variables para armado/salto (UI/estado)
// ------------------------------
bool enSalto = false;         // compat: armado/actividad por altura
bool ultraPreciso = false;    // compat: true=contorno, false=relleno (map a inJump)
bool jumpArmed = false;       // armado por altura (>=60 ft), para UI
bool inJump   = false;        // freefall confirmado, para UI

static bool prevFreefall = false;
static bool freefallArming = false;          // flag de armado de confirmación
static uint32_t freefallSinceMs = 0;
static const uint32_t FF_CONFIRM_MS = 300;   // confirma freefall (0.3 s)

// ====================================================
// Filtro adaptativo por modo (α)
// ====================================================
static inline float alphaFor(SensorMode m) {
  switch (m) {
    case SENSOR_MODE_FREEFALL:      return 0.35f;  // muy reactivo (vz ágil)
    case SENSOR_MODE_ULTRA_PRECISO: return 0.12f;  // suave pero estable (avión/campana)
    case SENSOR_MODE_AHORRO:
    default:                        return 0.08f;  // muy suave (tierra)
  }
}

// Velocidad de I2C según modo (ahorro=100 kHz, vuelo=400 kHz)
static inline void setI2cForMode(SensorMode m) {
  Wire.setClock(m == SENSOR_MODE_AHORRO ? 100000 : 400000);
}


// ====================================================
// Helpers: Vario y Freefall por VZ
// ====================================================
static void updateVarioAndFreefall(float altRel_m, float dt_s) {
  // 1) Filtro exponencial de altitud (α depende del modo actual)
  if (!s_altFiltInit) {
    s_altFilt     = altRel_m;
    s_prevAltFilt = altRel_m;
    s_altFiltInit = true;
  } else {
    s_prevAltFilt = s_altFilt;
    s_altFilt += alphaFor(currentMode) * (altRel_m - s_altFilt);
  }

  // 2) Velocidad vertical (m/s)
  float vz = 0.0f;
  if (dt_s > MIN_DT_S) vz = (s_altFilt - s_prevAltFilt) / dt_s;

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
      s_lastEnterTick = 0;
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
  setI2cForMode(SENSOR_MODE_AHORRO);

  // Lectura inicial para fijar la altitud de referencia
  if (bmp.performReading()) {
    altitudReferencia = bmp.readAltitude(1013.25);
  }

  // (Opcional) Log informativo: total de saltos (lifetime) desde logbook
  uint32_t total = 0;
  if (logbookGetTotal(total)) {
    Serial.printf("Logbook total (lifetime) = %lu\n", (unsigned long)total);
  }

  s_lastVarioMs = millis();
}

uint32_t sensor_ms_until_next_forced_read() {
  if (currentMode != SENSOR_MODE_AHORRO) return 0;
  // Si aún no hemos hecho la primera lectura, no dormimos
  if (lastForcedReadingTime == 0) return 0;

  const uint32_t now = millis();
  const uint32_t elapsed = now - lastForcedReadingTime;
  if (elapsed >= FORCED_AHORRO_MS) return 0;           // ya toca leer
  return FORCED_AHORRO_MS - elapsed;                    // ms restantes
}


// ====================================================
// Actualización de datos del sensor
// ====================================================
void updateSensorData() {
  // 0) dt para vario
  uint32_t nowMs = millis();
  float dt_s = (nowMs - s_lastVarioMs) / 1000.0f;
  if (dt_s < MIN_DT_S) dt_s = MIN_DT_S;

  // 1) Lectura del sensor (FORCED/one-shot)
  static bool firstReadingDone = false;
  static uint8_t readFails = 0;

  const bool debeLeer =
      !firstReadingDone ||
      (currentMode == SENSOR_MODE_AHORRO && (millis() - lastForcedReadingTime >= FORCED_AHORRO_MS)) ||
      (currentMode != SENSOR_MODE_AHORRO);

  bool sampleCounted = false;

  if (debeLeer) {
    const bool sensorOk = bmp.performReading();
    if (sensorOk) {
      readFails = 0;
      const float altActual = bmp.readAltitude(1013.25);
      altitud       = altActual;                                       // absoluto (m)
      // ===== Integración AGZ: sumar sesgo al cálculo relativo =====
      altCalculada  = altActual - altitudReferencia + alturaOffset + agzBias;    // relativa (m)

      onSampleAccepted();
      sampleCounted = true;
      s_lastVarioMs = nowMs;
    } else {
      if (++readFails >= 5) {
        freefallArming = false;
        readFails = 5;
      }
    }

    if (!firstReadingDone) firstReadingDone = true;
    if (currentMode == SENSOR_MODE_AHORRO) lastForcedReadingTime = millis();
  }

  // 1.b) Simulación
#if (ALT_SIM == 1)
  {
    const float simAltM = ALT_SIM_FT / 3.281f;
    altitud      = simAltM;
    altCalculada = simAltM;
    if (!sampleCounted) { onSampleAccepted(); sampleCounted = true; }
    s_lastVarioMs = nowMs;
  }
#elif (ALT_SIM == 2)
  {
    struct DemoStage { float feet; uint32_t ms; };
    static const DemoStage kDemo[] = {
      {   0.0f,  4000 },
      {  80.0f,  5000 },
      {12050.0f, 6000 },
      {  40.0f,  6000 },
    };
    static const size_t N = sizeof(kDemo)/sizeof(kDemo[0]);
    static size_t stage = 0;
    static uint32_t t0 = 0;

    if (t0 == 0) t0 = nowMs;
    if (nowMs - t0 >= kDemo[stage].ms) {
      stage = (stage + 1) % N;
      t0 = nowMs;
      Serial.printf("[DEMO] etapa=%u, alt=%.0f ft\n", (unsigned)stage, kDemo[stage].feet);
    }

    const float simAltM = kDemo[stage].feet / 3.281f;
    altitud      = simAltM;
    altCalculada = simAltM;
    if (!sampleCounted) { onSampleAccepted(); sampleCounted = true; }
    s_lastVarioMs = nowMs;
  }
#elif (ALT_SIM == 3)
{
  static uint32_t t0 = 0;
  if (t0 == 0) t0 = nowMs;

  // ===== parámetros de la simulación =====
  const uint32_t DWELL_MS        = 5000UL;            // 5 s tierra al inicio y final
  const uint32_t PERIOD          = ALT_SIM_PERIOD_MS; // p.ej. 40000 ms total
  const float    CANOPY_ALT_FT   = 4000.0f;           // altura de "apertura"
  const uint32_t CANOPY_HOLD_MS  = 2000UL;            // ↑ aumenta aquí los "segundos en apertura"
  const uint32_t CANOPY_SLOW_MS  = 2000UL;            // tramo opcional de canopy lento (después del hold)
  const float    CANOPY_V_FTPS   = 6.6f;              // ~2.0 m/s en ft/s (lento, no re-entra a FF)

  // Asegura tiempos válidos
  uint32_t dwellEach = (2*DWELL_MS < PERIOD) ? DWELL_MS : (PERIOD / 4);
  uint32_t rem       = (PERIOD > 2*dwellEach) ? (PERIOD - 2*dwellEach) : 0;

  // Subida + bajada (restaremos el hold y la rampa lenta de la bajada)
  uint32_t up_ms   = rem / 2;
  uint32_t down_ms = rem - up_ms;

  // Trozos de la bajada:
  // 1) fast descent: MAX -> 4000 ft
  // 2) HOLD en 4000
  // 3) CANOPY lento: 4000 -> (4000 - CANOPY_V_FTPS * (CANOPY_SLOW_MS/1000))
  // 4) salto a 0 ft en dwell final (o puedes añadir otra rampa si quieres)
  uint32_t need_down = CANOPY_HOLD_MS + CANOPY_SLOW_MS + 10UL; // margen
  if (down_ms <= need_down) {
    // si no alcanza, roba tiempo del dwell
    uint32_t extra = need_down - down_ms;
    if (dwellEach > extra) dwellEach -= extra;
    rem     = (PERIOD > 2*dwellEach) ? (PERIOD - 2*dwellEach) : need_down + up_ms;
    up_ms   = rem / 2;
    down_ms = rem - up_ms;
  }

  // Tiempos absolutos por fase
  const uint32_t t_up_start   = dwellEach;
  const uint32_t t_up_end     = t_up_start + up_ms;
  const uint32_t t_down_start = t_up_end;

  uint32_t fast_ms = down_ms - (CANOPY_HOLD_MS + CANOPY_SLOW_MS);
  const uint32_t t_hold_start = t_down_start + fast_ms;
  const uint32_t t_hold_end   = t_hold_start + CANOPY_HOLD_MS;
  const uint32_t t_slow_end   = t_hold_end + CANOPY_SLOW_MS;

  const uint32_t t_down_end   = t_down_start + down_ms;
  const uint32_t t_cycle_end  = t_down_end + dwellEach;

  // Tiempo dentro del ciclo
  uint32_t tm = (nowMs - t0) % PERIOD;

  float ft; // altitud simulada en pies

  if (tm < dwellEach) {
    // Tierra (dwell inicial)
    ft = 0.0f;

  } else if (tm < t_up_end && up_ms > 0) {
    // Subida lineal: 0 -> ALT_SIM_MAX_FT
    float p = float(tm - t_up_start) / float(up_ms);
    if (p < 0.f) p = 0.f; if (p > 1.f) p = 1.f;
    ft = p * ALT_SIM_MAX_FT;

  } else if (tm < t_down_end && down_ms > 0) {
    // Bajada con apertura a 4000 ft y canopy lento
    if (tm < t_hold_start) {
      // (1) Freefall rápido: MAX -> 4000 ft
      float p = float(tm - t_down_start) / float(fast_ms); // 0..1
      if (p < 0.f) p = 0.f; if (p > 1.f) p = 1.f;
      ft = ALT_SIM_MAX_FT - p * (ALT_SIM_MAX_FT - CANOPY_ALT_FT);
    } else if (tm < t_hold_end) {
      // (2) HOLD en 4000 ft (vz ≈ 0) > EXIT_HOLD_MS → salida de FF segura
      ft = CANOPY_ALT_FT;
    } else if (tm < t_slow_end) {
      // (3) CANOPY lento: descenso suave desde 4000 ft
      float sec = float(tm - t_hold_end) / 1000.0f;
      float drop = CANOPY_V_FTPS * sec;   // ft descendidos
      float ft2 = CANOPY_ALT_FT - drop;
      if (ft2 < 0.f) ft2 = 0.f;           // no bajar de 0
      ft = ft2;
    } else {
      // (4) Resto de la bajada (si quedó tiempo): mantén 0 (o añade rampa si prefieres)
      ft = 0.0f;
    }

  } else {
    // Tierra (dwell final)
    ft = 0.0f;
  }

  const float simAltM = ft / 3.281f;
  altitud      = simAltM;   // absoluto (m)
  altCalculada = simAltM;   // relativo (m)

  onSampleAccepted();
  sampleCounted = true;
  s_lastVarioMs = nowMs;
}
#endif

  // 2) Vario + estado FF por VZ
  updateVarioAndFreefall(altCalculada, dt_s);

  // Tick de bitácora (mide tiempos y estados internos del salto)
  logbookTick(altCalculada, currentMode);

  // Convertir altitud relativa a pies (para cambio Ahorro/Ultra)
  const float altEnPies = altCalculada * 3.281f;

  // 3) Configuración de modos con HISTÉRESIS
  const float LOW_ENTER  =  60.0f + MODE_HYST_FT;   // subir desde Ahorro a Ultra
  const float LOW_EXIT   =  60.0f - MODE_HYST_FT;   // bajar desde Ultra a Ahorro

  if (currentMode == SENSOR_MODE_AHORRO) {
    if (altEnPies >= LOW_ENTER) {
      // ULTRA PRECISO
      currentMode = SENSOR_MODE_ULTRA_PRECISO;
      bmp.setTemperatureOversampling(BMP3_OVERSAMPLING_16X);
      bmp.setPressureOversampling(BMP3_OVERSAMPLING_16X); // (ajusta si tu lib usa otro literal)
      bmp.setIIRFilterCoeff(BMP3_IIR_FILTER_COEFF_7);
      bmp.setOutputDataRate(BMP3_ODR_50_HZ);
      setI2cForMode(SENSOR_MODE_ULTRA_PRECISO);
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
      // FREEFALL por velocidad vertical
      currentMode = SENSOR_MODE_FREEFALL;
      bmp.setTemperatureOversampling(BMP3_NO_OVERSAMPLING);
      bmp.setPressureOversampling(BMP3_OVERSAMPLING_2X);
      bmp.setIIRFilterCoeff(BMP3_IIR_FILTER_DISABLE);
      setI2cForMode(SENSOR_MODE_FREEFALL);
      // ODR se mantiene en 50 Hz (Adafruit). Si migras a Bosch API: 200 Hz en FF.
      Serial.println("Modo Freefall activado (por velocidad vertical)");
      jumpArmed = true;
      enSalto   = true;

    } else if (altEnPies < LOW_EXIT) {
      // AHORRO
      currentMode = SENSOR_MODE_AHORRO;
      bmp.setTemperatureOversampling(BMP3_OVERSAMPLING_8X);
      bmp.setPressureOversampling(BMP3_OVERSAMPLING_32X);
      bmp.setIIRFilterCoeff(BMP3_IIR_FILTER_COEFF_15);
      bmp.setOutputDataRate(BMP3_ODR_25_HZ);
      setI2cForMode(SENSOR_MODE_AHORRO);
      Serial.println("Modo Ahorro activado (↓ desde Ultra)");
      lastForcedReadingTime = millis();

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
      setI2cForMode(SENSOR_MODE_ULTRA_PRECISO);
      Serial.println("Modo Ultra Preciso activado (salida de Freefall por VZ)");

      jumpArmed = true;
      enSalto   = true;
    } else {
      // Mantener señales en Freefall
      jumpArmed = true;
      enSalto   = true;
    }
  }

  // 4) Señales para UI + transición de salto (flanco + confirmación)
  const bool nowFreefall = (currentMode == SENSOR_MODE_FREEFALL);

  // Flanco de subida → **solo** armar confirmación
  if (nowFreefall && !prevFreefall) {
    freefallSinceMs = millis();
    freefallArming  = true;
    // (No iniciamos logbook aquí; se inicia al confirmar)
  }

  // Confirmación tras ventana FF_CONFIRM_MS → aquí iniciamos bitácora
  if (nowFreefall && freefallArming && !inJump &&
      (millis() - freefallSinceMs) >= FF_CONFIRM_MS) {

    inJump = true;  // UI: relleno

    // Failsafe: si quedó algo abierto, ciérralo antes de iniciar uno nuevo
    if (logbookIsActive()) {
      logbookFinalizeIfOpen();
    }

    // Iniciar log al confirmar FF (no en el primer toque)
    // --- INSTRUMENTACIÓN NVS (OPEN) ---
    nvs_stats_t st_open_before;
    nvs_get_stats(NULL, &st_open_before);

    logbookBeginFreefall(altCalculada);  // << se abre el registro de salto

    nvs_stats_t st_open_after;
    nvs_get_stats(NULL, &st_open_after);
    Serial.printf("[NVS] OPEN used_entries %u -> %u (delta=%+d)\n",
                  st_open_before.used_entries,
                  st_open_after.used_entries,
                  int(st_open_after.used_entries) - int(st_open_before.used_entries));

    freefallArming = false; // ya confirmamos este ingreso a freefall
  }

  // Flanco de bajada de freefall → marcar apertura y reset flags
  if (!nowFreefall && prevFreefall) {
    logbookMarkDeploy(altCalculada);
    inJump         = false;   // UI: círculo contorno
    freefallArming = false;   // cancelar armado si estaba pendiente
  } else if (!nowFreefall) {
    inJump         = false;
    freefallArming = false;
  }

  // Compatibilidad con flags previos
  ultraPreciso = jumpArmed && !inJump;  // true=contorno (armado), false=relleno (en salto)
  prevFreefall = nowFreefall;

  // ===== Cierre por AHORRO mantenido =====
  static uint32_t groundSinceMs = 0;
  if (currentMode == SENSOR_MODE_AHORRO) {
    if (groundSinceMs == 0) groundSinceMs = millis();
    if (logbookIsActive()) {
      if (millis() - groundSinceMs >= 100UL) {   // antes: 1000UL
        logbookFinalizeIfOpen();
        groundSinceMs = 0;
      }
    }
  } else {
    groundSinceMs = 0;
  }

  // ===== Cierre FAILSAFE en suelo estable =====
  {
    static uint32_t s_groundStableMs = 0;
    // Reusar filtro para vz: s_altFilt / s_prevAltFilt
    float vz_mps = (s_altFilt - s_prevAltFilt) / max(dt_s, MIN_DT_S);
    const bool modeAhorro = (currentMode == SENSOR_MODE_AHORRO);
    const bool nearGround = fabsf(altCalculada) < GROUND_ALT_M;
    const bool vzQuiet    = fabsf(vz_mps)       < GROUND_VZ_MPS;
    const bool groundLike = modeAhorro || (nearGround && vzQuiet);

    if (logbookIsActive() && groundLike) {
      if (s_groundStableMs == 0) s_groundStableMs = millis();
      if (millis() - s_groundStableMs >= GROUND_STABLE_MS) {
        logbookFinalizeIfOpen();          // cierre robusto adicional
        s_groundStableMs = 0;
      }
    } else {
      s_groundStableMs = 0;
    }
  }

  // ===== Watchdog post-apertura (canopy/suelo por mucho tiempo y bajo) =====
  {
    static uint32_t s_postDeployMs = 0;
    const bool lowAltFt = (altCalculada * 3.281f) < POSTDEPLOY_LOWALT_FT;

    if (logbookIsActive()) {
      // Si NO hay FF por VZ (canopy/suelo) y estamos bajos, correr reloj
      if (!s_freefallByVZ) {
        if (s_postDeployMs == 0) s_postDeployMs = millis();
        if (lowAltFt && (millis() - s_postDeployMs >= POSTDEPLOY_WATCHDOG_MS)) {
          logbookFinalizeIfOpen();
          s_postDeployMs = 0;
        }
      } else {
        s_postDeployMs = 0; // Reset si volvemos a FF
      }
    } else {
      s_postDeployMs = 0;
    }
  }

  // ===== Auto Ground Zero (AGZ) — corrección lenta de drift en tierra =====
  {
    // Recalcular Vz con el filtro ya calculado, para este ámbito
    float vz_mps = (s_altFilt - s_prevAltFilt) / max(dt_s, MIN_DT_S);

    // Altura relativa SIN offset de usuario (solo base + sesgo):
    // rel_sin_offset = altitud - altitudReferencia + agzBias
    const float rel_sin_offset = (altitud - altitudReferencia) + agzBias;

    // Condición de elegibilidad: en tierra, cerca de "0 real" y tranquilo
    const bool nearZero = fabsf(rel_sin_offset) < AGZ_WINDOW_M;
    const bool quietVz  = fabsf(vz_mps)        < AGZ_VZ_QUIET_MPS;
    const bool eligible = (!inJump) && nearZero && quietVz;

    static uint32_t s_agzStableStartMs = 0;
    static uint32_t s_agzLastSaveMs    = 0;
    static float    s_lastSavedBias    = 0.0f;

    if (eligible) {
      if (s_agzStableStartMs == 0) s_agzStableStartMs = millis();

      if (millis() - s_agzStableStartMs >= AGZ_STABLE_MS) {
        // Error a cancelar (no tocamos alturaOffset del usuario)
        const float err = -rel_sin_offset;

        // Filtro lento con límite de velocidad por hora
        const float dt = fmaxf(dt_s, 0.05f);
        const float alpha = dt / AGZ_TAU_SECONDS;

        float step = err * alpha;

        const float vlim_mps = AGZ_RATE_LIMIT_MPH / 3600.0f; // m/s
        if (step >  vlim_mps * dt) step =  vlim_mps * dt;
        if (step < -vlim_mps * dt) step = -vlim_mps * dt;

        agzBias += step;

        // Clamp de seguridad
        if (agzBias >  AGZ_BIAS_CLAMP_M) agzBias =  AGZ_BIAS_CLAMP_M;
        if (agzBias < -AGZ_BIAS_CLAMP_M) agzBias = -AGZ_BIAS_CLAMP_M;

        // Guardado poco frecuente (delta grande o periodo)
        if (fabsf(agzBias - s_lastSavedBias) >= AGZ_SAVE_DELTA_M ||
            (millis() - s_agzLastSaveMs) >= AGZ_SAVE_PERIOD_MS) {
          saveAgzBias();
          s_lastSavedBias = agzBias;
          s_agzLastSaveMs = millis();
          // (Opcional) Serial.printf("[AGZ] saved bias=%.2f m\n", agzBias);
        }
      }
    } else {
      s_agzStableStartMs = 0; // se perdió condición de elegibilidad
    }
  }
}

