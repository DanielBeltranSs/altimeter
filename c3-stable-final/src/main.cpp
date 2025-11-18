#include <Arduino.h>
#include <Wire.h>
#include <U8g2lib.h>
#include <esp_sleep.h>        // deep sleep (GPIO wakeup en C3)
#include "config.h"
#include "sensor_module.h"
#include "ui_module.h"
#include "power_lock.h"       // Sleep-lock opción B (25 min fijos)
#include "battery.h"          // módulo de batería
#include "datetime_module.h"  // tiempo
#include "logbook.h"
#include "charge_detect.h"
#include <Arduino.h>
#include "esp_sleep.h"
#include "driver/gpio.h"
#include "alarm.h"


// OLED global definida en ui_module.cpp
extern U8G2_SSD1306_128X64_NONAME_F_HW_I2C u8g2;

// Bloqueo temporal para evitar re-entrada del menú tras "Exit menu"
extern volatile uint32_t uiBlockMenuOpenUntilMs;

// ==========================
// Instrumentación de Hz (Opción 1)
#ifndef DEBUG_HZ
#define DEBUG_HZ 1
#endif

#if DEBUG_HZ
volatile uint32_t g_samples = 0;     // incrementa al aceptar una lectura válida
unsigned long g_t_last = 0;          // millis() del último reporte
extern float altCalculada;           // del sensor_module.cpp
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
// ==========================

// === Deep Sleep por aterrizaje (fijo 5 min) ===
#ifndef LANDING_DS_ENABLE
#define LANDING_DS_ENABLE   1
#endif
#ifndef LANDING_DS_DELAY_MS
#define LANDING_DS_DELAY_MS 300000UL   // 5 minutos
#endif

// Variables globales para la UI y menú
bool pantallaEncendida = true;   // compat

// Calibración automática al inicio
bool calibracionRealizada = false;

extern float alturaOffset;

// Referencias del sensor
extern Adafruit_BMP3XX bmp;
extern float altitudReferencia;
// <<< añadimos el sesgo del AGZ como extern para resetearlo en setup >>>
extern float agzBias;

// (Declaradas en ui_module.h)
extern bool menuActivo;
extern int  menuOpcion;
extern long lastMenuInteraction;

// Bloqueo de sleep durante Snake
extern bool gameSnakeRunning;

// -------------------------
// Temporizador de inactividad para deep sleep
// -------------------------
static unsigned long lastActivityMs = 0;
static inline void noteUserActivity() { lastActivityMs = millis(); }
unsigned long getLastActivityMs() {  // expone el último timestamp de actividad para la UI
  return lastActivityMs;
}

// -------------------------
// Temporizador fijo post-aterrizaje (sin archivos nuevos)
// -------------------------
static SensorMode    s_prevMode      = SENSOR_MODE_AHORRO;
static bool          s_landingArmed  = false;
static unsigned long s_landingT0     = 0;

// ---- Wake por GPIO (mismo nivel: HIGH) ----
// Botón: ACTIVO-ALTO (pulldown). VBUS: HIGH con divisor 330k/510k.
static inline void setupWakeSourceGPIO() {
  // --- Config de pines ---
  // Botón OLED: activo-alto. Si no tienes resistor externo, deja el pulldown interno.
  pinMode(WAKE_BTN_PIN, INPUT_PULLDOWN);

  // Detección de carga (divisor desde VBUS): ¡SIN pulls internos!
  pinMode(CHARGE_ADC_PIN, INPUT);
  gpio_pullup_dis((gpio_num_t)CHARGE_ADC_PIN);
  gpio_pulldown_dis((gpio_num_t)CHARGE_ADC_PIN);

  // (Opcional) si tu botón SÍ tiene resistencia externa, puedes quitar el pulldown:
  // gpio_pulldown_dis((gpio_num_t)WAKE_BTN_PIN);

  // --- Wake por GPIO (cualquiera en ALTO) ---
  const uint64_t mask = (1ULL << WAKE_BTN_PIN) | (1ULL << CHARGE_ADC_PIN);
  esp_deep_sleep_enable_gpio_wakeup(mask, ESP_GPIO_WAKEUP_GPIO_HIGH);
}



// ======================================================================
// Proveedor de tiempo para Logbook (inyecta datetimeNowEpoch como uint32_t)
// ======================================================================
static uint32_t timeProviderThunk() {
  int64_t e = datetimeNowEpoch();        // -1 si no hay base válida
  return (e >= 0) ? (uint32_t)e : 0;     // 0 si no hay base
}

// ======================================================================
// *** BLINDAJE DE VUELO ***
// - Detector de “contexto de vuelo” (sin usar logbookIsActive)
// - Ventana de gracia global post-vuelo
// ======================================================================
extern bool enSalto;   // definidos en otra unidad
extern bool inJump;

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

// Entrar a deep sleep (configura wake y apaga periféricos básicos)
static void enterDeepSleepNow(const char* reason = "inactividad") {
  if (inFlightNow() || inFlightGraceNow()) {
    Serial.printf("Deep sleep BLOQUEADO por vuelo/gracia (%s).\n", reason);
    return;
  }

  Serial.printf("Entrando a deep sleep por %s...\n", reason);

  // Cierre de logbook si hubiera salto abierto
  logbookFinalizeIfOpen();

  // Apagar OLED de forma segura
  u8g2.setPowerSave(true);
  u8g2.clearBuffer();
  u8g2.sendBuffer();

  setupWakeSourceGPIO();    // wake por botón (HIGH) + VBUS (HIGH)
  datetimeOnBeforeDeepSleep();

  delay(30);
  Serial.flush();
  esp_deep_sleep_start();   // ¡a dormir!
}

// Chequear si corresponde dormir (respeta menú, Snake, sleep-lock y modo del sensor)
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
#define SENSOR_TICK_ULTRA_MS        50  // ~10 Hz
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
  // (cualquier otro modo cae en “Ahorro” por defecto)

  if (now - lastTick < interval) return;
  lastTick = now;

  updateSensorData();   // << única llamada; fuera del throttle no se invoca
}
unsigned long tTest = 0;
bool testFired = false;

void setup() {
  Serial.begin(115200);
  delay(300);
  Serial.println("Setup iniciado");

  Wire.begin(SDA_PIN, SCL_PIN);
  Wire.setClock(400000);  // 400 kHz

  loadConfig();
  loadUserConfig();

  // Orden: tiempo -> logbook (init + proveedor) -> UI/Sensor/Batería
  datetimeInit();
  logbookInit();
  logbookSetTimeSource(timeProviderThunk);
  initSensor();
  initUI();
  alarmInit();
  tTest = millis();
  batteryInit();          // inicializar batería
  chargeDetectBegin();    // inicializar medición de VBUS (ADC)
  
  // Botones de UI (ACTIVOS-ALTO)
  pinMode(BUTTON_ALTITUDE, INPUT_PULLDOWN);
  pinMode(BUTTON_OLED,     INPUT_PULLDOWN);
  pinMode(BUTTON_MENU,     INPUT_PULLDOWN);

  uiBlockMenuOpenUntilMs = millis() + 300;

  // Fuente de wake preparada desde ya (botón HIGH + VBUS HIGH)
  setupWakeSourceGPIO();

  // Iniciar temporizador de inactividad
  noteUserActivity();

  // Inicializa referencia de modo para el temporizador de aterrizaje
  s_prevMode = getSensorMode();
  s_landingArmed = false;

  Serial.println("Setup completado");
}

void loop() {
  // Mantener el estado del lock (expiración automática)
  alarmService();
  if (!testFired && millis() - tTest > 1500) { // vibra 1.5 s después de arrancar
    alarmOnLockAltitude();                      // debería vibrar una vez
    testFired = true;
  }
  powerLockUpdate();

  // === Sensores / UI / Batería ===
  tickSensor();        // << en vez de updateSensorData() continuo
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

  // === Actualiza ventana de gracia global por contexto de vuelo ===
  updateFlightGraceWindow();
  
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
#if DEBUG_HZ
      onSampleAccepted();
#endif
      // ====== RESET AGZ INCONDICIONAL AL ARRANCAR ======
      // Arranque exacto al offset (no persistimos aquí para evitar desgaste).
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

    // Recalibración (mantener ALTITUDE 1s) con sleep-lock
    if (altRise) {
      altDownTs = millis();
      altDidAction = false;
    }
    if (altNow && !altDidAction && (millis() - altDownTs >= 1000UL)) {
      if (bmp.performReading()) {
        altitudReferencia = bmp.readAltitude(1013.25);

        // **Importante**: NO borrar el offset.
        // Así, tras el lock, la UI muestra 'alturaOffset' (p. ej. +200 m),
        // y al aterrizar en una zona más baja por ese offset, llegará a 0.

        // ======== AGZ: resetear sesgo para que la lectura quede exacta tras el lock ========
        agzBias = 0.0f;
        saveAgzBias();
        // ====================================================================================

        Serial.printf("Lock aplicado: ref=%.2fm, offset=%.2fm (preservado, AGZ=0)\n",
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

  // Sin delay bloqueante: la UI ya regula su ritmo de repintado por modo.
}
