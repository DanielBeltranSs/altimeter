#include <Arduino.h>
#include <Wire.h>
#include <U8g2lib.h>
#include <esp_sleep.h>        // deep sleep (GPIO wakeup)
#include "driver/gpio.h"
#include "driver/rtc_io.h"

#include "config.h"
#include "sensor_module.h"
#include "ui_module.h"
#include "power_lock.h"       // Sleep-lock opción B (25 min fijos)
#include "battery.h"          // módulo de batería
#include "datetime_module.h"  // tiempo
#include "logbook.h"
#include "charge_detect.h"
#include "alarm.h"

// ==========================
// Externs provistos por otros módulos
// ==========================
extern volatile uint32_t uiBlockMenuOpenUntilMs;

extern float altCalculada;            // sensor_module.cpp (solo para debug Hz)
extern float alturaOffset;            // offset visible en UI
extern float altitudReferencia;       // referencia de altitud
extern Adafruit_BMP3XX bmp;           // sensor

// Sesgo AGZ (aprendizaje drift barométrico)
extern float agzBias;
extern void  saveAgzBias(void);

// Estado UI / menú
extern bool  menuActivo;
extern int   menuOpcion;
extern long  lastMenuInteraction;

// Juego Snake
extern bool gameSnakeRunning;

// Flags de “vuelo” (definidos en otro módulo)
extern bool enSalto;
extern bool inJump;

// Timeout de ahorro (ms) proveniente de configuración de usuario
extern unsigned long ahorroTimeoutMs;

// ==========================
// Instrumentación de Hz
// ==========================
#ifndef DEBUG_HZ
#define DEBUG_HZ 1
#endif

#if DEBUG_HZ
volatile uint32_t g_samples = 0;     // incrementa al aceptar una lectura válida
unsigned long g_t_last = 0;          // millis() del último reporte
void onSampleAccepted() { g_samples++; }

static void hzReportTick(int modo /* 0: Ahorro, 1: Ultra, 2: Freefall */) {
  unsigned long now = millis();
  if (now - g_t_last >= 1000UL) {
    Serial.printf("[HZ] modo=%d  Hz=%lu\n", modo, (unsigned long)g_samples);
    g_samples = 0;
    g_t_last = now;
  }
}
#endif

// === Deep Sleep por aterrizaje (fijo 5 min) ===
#ifndef LANDING_DS_ENABLE
#define LANDING_DS_ENABLE   1
#endif
#ifndef LANDING_DS_DELAY_MS
#define LANDING_DS_DELAY_MS 300000UL   // 5 minutos
#endif

// ==========================
// Estado general
// ==========================
bool pantallaEncendida = true;   // compat
bool calibracionRealizada = false;

static unsigned long lastActivityMs = 0;
static inline void noteUserActivity() { lastActivityMs = millis(); }
unsigned long getLastActivityMs() {  // expone a la UI
  return lastActivityMs;
}

// Temporizador fijo post-aterrizaje
static SensorMode    s_prevMode      = SENSOR_MODE_AHORRO;
static bool          s_landingArmed  = false;
static unsigned long s_landingT0     = 0;

// ==========================
// Wake por GPIO (nivel alto)
// ==========================
// Botón: ACTIVO-ALTO (pulldown). VBUS: HIGH con divisor 330k/510k (o similar).
static inline void setupWakeSourceGPIO() {
  // Config digital (no rige en deep-sleep, pero útil en runtime)
  pinMode(WAKE_BTN_PIN, INPUT_PULLDOWN);

  pinMode(CHARGE_ADC_PIN, INPUT);
  gpio_pullup_dis((gpio_num_t)CHARGE_ADC_PIN);
  gpio_pulldown_dis((gpio_num_t)CHARGE_ADC_PIN);

  // Fuentes de wake (EXT1: cualquiera en HIGH)
  const uint64_t mask = (1ULL << WAKE_BTN_PIN) | (1ULL << CHARGE_ADC_PIN);
  ESP_ERROR_CHECK(esp_sleep_enable_ext1_wakeup(mask, ESP_EXT1_WAKEUP_ANY_HIGH));
}

// Wake GPIO para light-sleep (nivel alto en botones/VBUS)
static inline void setupGpioWakeForLightSleep() {
  // Botones (activos en HIGH)
  gpio_wakeup_enable((gpio_num_t)BUTTON_MENU,     GPIO_INTR_HIGH_LEVEL);
  gpio_wakeup_enable((gpio_num_t)BUTTON_ALTITUDE, GPIO_INTR_HIGH_LEVEL);
  gpio_wakeup_enable((gpio_num_t)BUTTON_OLED,     GPIO_INTR_HIGH_LEVEL);
  // VBUS por divisor (CHARGE_ADC_PIN sube cuando hay USB)
  gpio_wakeup_enable((gpio_num_t)CHARGE_ADC_PIN,  GPIO_INTR_HIGH_LEVEL);

  // Habilita fuente de wake por GPIO en light sleep
  ESP_ERROR_CHECK(esp_sleep_enable_gpio_wakeup());
}

// Duerme entre lecturas FORCED en suelo/ahorro para ahorrar batería
static void lightSleepBetweenSensorReads() {
  // No dormir si estamos en UI interactiva o juegos, o con sleep-lock
  if (menuActivo) return;
  if (gameSnakeRunning) return;
  if (powerLockActive()) return;
  if (getSensorMode() != SENSOR_MODE_AHORRO) return;

  uint32_t rem_ms = sensor_ms_until_next_forced_read();
  // Umbral: evita dormir para descansos muy cortos
  if (rem_ms < 25) return;

  // Margen para despertar un poco antes de la lectura
  const uint32_t SAFETY_MS = 8;
  uint64_t sleep_us = (uint64_t)((rem_ms > SAFETY_MS) ? (rem_ms - SAFETY_MS) : 0) * 1000ULL;
  if (sleep_us < 1000) return;

  // Programa fuentes de wake para este ciclo de light sleep
  esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_ALL);
  setupGpioWakeForLightSleep();
  ESP_ERROR_CHECK(esp_sleep_enable_timer_wakeup(sleep_us));

  Serial.flush();                // evita que el UART despierte/consuma
  esp_light_sleep_start();       // vuelve aquí al despertar (timer o GPIO)
}


// ==========================
// Pull-downs RTC para deep-sleep
// ==========================
static void armRtcPullsForDeepSleep() {
  // WAKE_BTN_PIN: mantener LOW en deep-sleep (evita falsos HIGH por flotación)
  rtc_gpio_init((gpio_num_t)WAKE_BTN_PIN);
  rtc_gpio_set_direction((gpio_num_t)WAKE_BTN_PIN, RTC_GPIO_MODE_INPUT_ONLY);
  rtc_gpio_pullup_dis((gpio_num_t)WAKE_BTN_PIN);
  rtc_gpio_pulldown_en((gpio_num_t)WAKE_BTN_PIN);

  // CHARGE_ADC_PIN (VBUS divisor): mantener LOW si no hay USB
  rtc_gpio_init((gpio_num_t)CHARGE_ADC_PIN);
  rtc_gpio_set_direction((gpio_num_t)CHARGE_ADC_PIN, RTC_GPIO_MODE_INPUT_ONLY);
  rtc_gpio_pullup_dis((gpio_num_t)CHARGE_ADC_PIN);
  rtc_gpio_pulldown_en((gpio_num_t)CHARGE_ADC_PIN);
}

// ==========================
// Debug: causa de wake
// ==========================
static void printWakeDebug() {
  esp_sleep_wakeup_cause_t cause = esp_sleep_get_wakeup_cause();
  Serial.printf("[WAKE] cause=%d\n", (int)cause);
  if (cause == ESP_SLEEP_WAKEUP_EXT1) {
    uint64_t pins = esp_sleep_get_ext1_wakeup_status();
    Serial.printf("[WAKE] ext1 mask=0x%llX (pin(es) HIGH)\n", pins);
  }
}

// ======================================================================
// Proveedor de tiempo para Logbook
// ======================================================================
static uint32_t timeProviderThunk() {
  int64_t e = datetimeNowEpoch();        // -1 si no hay base válida
  return (e >= 0) ? (uint32_t)e : 0;     // 0 si no hay base
}

// ======================================================================
// *** BLINDAJE DE VUELO ***
// ======================================================================
static inline bool inFlightNow() {
  SensorMode sm = getSensorMode();     // AHORRO / ULTRA / FREEFALL
  if (sm != SENSOR_MODE_AHORRO) return true; // modo de vuelo activo
  if (enSalto || inJump)        return true; // flags UI/estado de salto
  return false;
}

static bool     s_flightGraceArmed = false;
static uint32_t s_flightGraceT0    = 0;
static const uint32_t FLIGHT_GRACE_MS = 120000UL; // 120 s de gracia global

static inline void updateFlightGraceWindow() {
  static bool prevInFlight_ctx = false;
  bool nowInFlight = inFlightNow();
  if (nowInFlight) {
    prevInFlight_ctx   = true;
    s_flightGraceArmed = false;
  } else {
    if (prevInFlight_ctx) {
      s_flightGraceArmed = true;
      s_flightGraceT0    = millis();
      prevInFlight_ctx   = false;
    }
  }
}

static inline bool inFlightGraceNow() {
  return s_flightGraceArmed && (millis() - s_flightGraceT0 < FLIGHT_GRACE_MS);
}

// ==========================
// Entrar a deep sleep
// ==========================
static void enterDeepSleepNow(const char* reason = "inactividad") {
  if (inFlightNow() || inFlightGraceNow()) {
    Serial.printf("Deep sleep BLOQUEADO por vuelo/gracia (%s).\n", reason);
    return;
  }

  // Evitar dormir si el botón está presionado (si duermes con HIGH, EXT1 despierta al toque)
  if (digitalRead(WAKE_BTN_PIN) == HIGH) {
    Serial.println("Deep sleep cancelado: botón en HIGH.");
    noteUserActivity();
    return;
  }
  // Evitar dormir si hay USB presente
  if (isUsbPresent()) {
    Serial.println("Deep sleep cancelado: USB presente.");
    noteUserActivity();
    return;
  }

  Serial.printf("Entrando a deep sleep por %s...\n", reason);

  // Cierre de logbook si hubiera salto abierto
  logbookFinalizeIfOpen();

  // Apagar OLED de forma segura
  u8g2.setPowerSave(true);
  u8g2.clearBuffer();
  u8g2.sendBuffer();

  // Asegurar niveles estables en dominio RTC durante deep-sleep
  armRtcPullsForDeepSleep();

  // Re-configurar fuentes de wake de forma limpia
  esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_ALL);
  const uint64_t mask = (1ULL << WAKE_BTN_PIN) | (1ULL << CHARGE_ADC_PIN);
  ESP_ERROR_CHECK(esp_sleep_enable_ext1_wakeup(mask, ESP_EXT1_WAKEUP_ANY_HIGH));

  // Hook de fecha/hora
  datetimeOnBeforeDeepSleep();

  delay(30);
  Serial.flush();
  esp_deep_sleep_start();   // ¡a dormir!
}

// ==========================
// Evaluar si corresponde dormir
// ==========================
static inline void maybeEnterDeepSleep() {
  if (inFlightNow() || inFlightGraceNow()) return;
  if (isUsbPresent()) { noteUserActivity(); return; }

#if LANDING_DS_ENABLE
  if (s_landingArmed) {
    if (!menuActivo && !gameSnakeRunning && !powerLockActive()) {
      if (getSensorMode() == SENSOR_MODE_AHORRO) {
        if (millis() - lastActivityMs >= 10000UL) {
          if (millis() - s_landingT0 >= LANDING_DS_DELAY_MS) {
            enterDeepSleepNow("aterrizaje (5min)");
          }
        }
      } else {
        s_landingArmed = false;
      }
    }
  }
#endif

  if (ahorroTimeoutMs == 0) return;            // OFF
  if (menuActivo) return;                      // no dormir dentro del menú
  if (gameSnakeRunning) return;                // no dormir durante Snake
  if (powerLockActive()) return;               // sleep-lock activo

  SensorMode sm = getSensorMode();
  if (sm != SENSOR_MODE_AHORRO) return;

  unsigned long now = millis();
  if ((now - lastActivityMs) >= ahorroTimeoutMs) enterDeepSleepNow("inactividad");
}

// ======================================================================
// Estrangulador de lecturas del BMP sin tocar sensor_module
// ======================================================================
#ifndef SENSOR_TICK_AHORRO_MS
#define SENSOR_TICK_AHORRO_MS       150  // ~6.7 Hz en tierra (ahorro real de I2C/energía)
#endif
#ifndef SENSOR_TICK_ULTRA_MS
#define SENSOR_TICK_ULTRA_MS         50  // ~10 Hz
#endif
#ifndef SENSOR_TICK_FREEFALL_MS
#define SENSOR_TICK_FREEFALL_MS      10  // ~12.5 Hz
#endif

static void tickSensor() {
  static uint32_t lastTick = 0;
  const uint32_t now = millis();

  uint16_t interval = SENSOR_TICK_AHORRO_MS;
  SensorMode m = getSensorMode();
  if (m == SENSOR_MODE_ULTRA_PRECISO) interval = SENSOR_TICK_ULTRA_MS;
  else if (m == SENSOR_MODE_FREEFALL) interval = SENSOR_TICK_FREEFALL_MS;

  if (now - lastTick < interval) return;
  lastTick = now;

  updateSensorData();
#if DEBUG_HZ
  onSampleAccepted();
#endif
}

// ==========================
// Setup / Loop
// ==========================
unsigned long tTest = 0;
bool testFired = false;

void setup() {
  Serial.begin(115200);
  delay(300);
  Serial.println("Setup iniciado");
  printWakeDebug();

  Wire.begin(SDA_PIN, SCL_PIN);
  Wire.setClock(400000);  // 400 kHz

  loadConfig();
  loadUserConfig();

  // Orden: tiempo -> logbook (init + proveedor) -> Sensor/UI/Batería
  datetimeInit();
  logbookInit();
  logbookSetTimeSource(timeProviderThunk);

  initSensor();
  initUI();
  //alarmInit();
  tTest = millis();

  batteryInit();          // inicializar batería
  chargeDetectBegin();    // inicializar medición de VBUS (ADC)

  // Botones de UI (ACTIVOS-ALTO)
  pinMode(BUTTON_ALTITUDE, INPUT_PULLDOWN);
  pinMode(BUTTON_OLED,     INPUT_PULLDOWN);
  pinMode(BUTTON_MENU,     INPUT_PULLDOWN);

  uiBlockMenuOpenUntilMs = millis() + 300;

  // Prepara EXT1 (no imprescindible, lo volvemos a armar antes de dormir)
  setupWakeSourceGPIO();

  // Iniciar temporizador de inactividad
  noteUserActivity();

  // Inicializa referencia de modo para el temporizador de aterrizaje
  s_prevMode = getSensorMode();
  s_landingArmed = false;

  Serial.println("Setup completado");
}

void loop() {
  // Servicio de alarmas y lock
  //alarmService();
  powerLockUpdate();
  //alarmOnLockAltitude();                      // debería vibrar una vez

  // === Sensores / Batería / Carga / UI ===
  tickSensor();
  batteryUpdate();
  chargeDetectUpdate();
  static uint32_t lastDbg = 0;
  uint32_t now = millis();
  if (now - lastDbg >= 1000) {
    lastDbg = now;
    Serial.printf("[CHG] raw=%d vadc=%.2fV vbus=%.2fV present=%d\n",
                  chargeDebugRaw(), chargeDebugVadc(),
                  chargeDebugVbus(), isUsbPresent());
  }

  updateUI();

  // === Gracia global por contexto de vuelo ===
  updateFlightGraceWindow();
  lightSleepBetweenSensorReads();
  // ----- Corte por batería baja, pero NUNCA durante el vuelo -----
  if (!inFlightNow() && !inFlightGraceNow() && batteryShouldDeepSleep()) {
    enterDeepSleepNow("batería baja");
    return; // seguridad (deep sleep no retorna)
  }

  // --- Armado/cancelación del temporizador de aterrizaje (transiciones de modo) ---
#if LANDING_DS_ENABLE
  {
    SensorMode cur = getSensorMode();
    if (s_prevMode != SENSOR_MODE_AHORRO && cur == SENSOR_MODE_AHORRO) {
      s_landingArmed = true;
      s_landingT0    = millis();
    }
    if (s_prevMode == SENSOR_MODE_AHORRO && cur != SENSOR_MODE_AHORRO) {
      s_landingArmed = false;
    }
    s_prevMode = cur;
  }
#endif

  // === Calibración automática al inicio (una sola vez) ===
  if (!calibracionRealizada) {
    if (bmp.performReading()) {
      altitudReferencia = bmp.readAltitude(1013.25);
      Serial.println("Calibración inicial: altitud reiniciada a cero.");
      // ====== RESET AGZ INCONDICIONAL AL ARRANCAR ======
      agzBias = 0.0f;
      Serial.println("AGZ: sesgo reseteado (boot).");
      // ================================================
    } else {
      Serial.println("Error al leer sensor en calibración inicial.");
    }
    calibracionRealizada = true;
    noteUserActivity();
  }

  // ======================
  // === Manejo botones ===
  // ======================
  if (menuActivo) {
    processMenu();
    noteUserActivity();
  } else {
    static bool menuPrev = false, altPrev = false, okPrev = false;
    static uint32_t altDownTs = 0;
    static bool altDidAction = false;

    // ACTIVO-ALTO
    bool menuNow = (digitalRead(BUTTON_MENU) == HIGH);
    bool altNow  = (digitalRead(BUTTON_ALTITUDE) == HIGH);
    bool okNow   = (digitalRead(BUTTON_OLED) == HIGH);

    bool menuRise =  menuNow && !menuPrev;
    bool menuFall = !menuNow &&  menuPrev;
    bool altRise  =  altNow  && !altPrev;
    bool altFall  = !altNow  &&  altPrev;
    bool okRise   =  okNow   && !okPrev;

    // bloquear apertura si Snake está activo
    if (!gameSnakeRunning && !menuActivo && menuRise && (millis() >= uiBlockMenuOpenUntilMs)) {
      noteUserActivity();
      menuActivo = true;
      menuOpcion = 0;
      lastMenuInteraction = millis();
    }

    // Recalibración (mantener ALTITUDE 1s) con sleep-lock, preservando offset visible
    if (altRise) {
      altDownTs = millis();
      altDidAction = false;
    }
    if (altNow && !altDidAction && (millis() - altDownTs >= 1000UL)) {
      if (bmp.performReading()) {
        altitudReferencia = bmp.readAltitude(1013.25);

        // **Importante**: NO borrar el offset (alturaOffset se mantiene).
        // Tras el lock, la UI muestra el offset configurado.

        // AGZ: resetear sesgo para que la lectura quede exacta tras el lock
        agzBias = 0.0f;
        saveAgzBias();

        Serial.printf("Lock aplicado: ref=%.2fm, offset=%.2fm (AGZ=0)\n",
                      altitudReferencia, alturaOffset);
#if DEBUG_HZ
        onSampleAccepted();
#endif
        powerLockActivate();
      } else {
        Serial.println("Error al leer sensor en recalibración manual.");
      }
      altDidAction = true;
      noteUserActivity();
    }
    if (altFall) {
      altDownTs = 0;
      altDidAction = false;
    }

    if (okRise) {
      noteUserActivity();
      // Botón OLED: alternar backlight (ON=brillo menú, OFF=duty 250)
      lcdBacklightToggle();
    }

    // Actualiza prevs
    menuPrev = menuNow;
    altPrev  = altNow;
    okPrev   = okNow;
  }

  // Reporte de Hz (1 Hz) usando el modo REAL del sensor
#if DEBUG_HZ
  {
    SensorMode modoReal = getSensorMode();
    int modoIdx = (modoReal == SENSOR_MODE_AHORRO) ? 0 :
                  (modoReal == SENSOR_MODE_ULTRA_PRECISO) ? 1 : 2;
    hzReportTick(modoIdx);
  }
#endif

  // === Evaluar sueño (aterrizaje fijo primero, luego inactividad) ===
  maybeEnterDeepSleep();

  // Sin delay bloqueante: la UI regula su ritmo de repintado por modo
}
