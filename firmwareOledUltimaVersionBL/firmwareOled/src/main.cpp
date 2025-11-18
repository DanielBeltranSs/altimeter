#include <Arduino.h>
#include <Wire.h>
#include <esp_sleep.h>          // deep sleep (GPIO wakeup en C3)
#include "config.h"
#include "ble_module.h"
#include "sensor_module.h"
#include "ui_module.h"
#include "buzzer_module.h"
#include "power_lock.h"         // <<< Sleep-lock opción B (25 min fijos)
#include "battery.h"            // <<< Nuevo: módulo de batería

// ==========================
/* Instrumentación de Hz (Opción 1) */
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
// Si no quieres tocar config.h, estos defaults quedan aquí (puedes moverlos a config.h).
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
// Arma al detectar transición a modo Ahorro y duerme a los 5 min si procede
// -------------------------
static SensorMode    s_prevMode      = SENSOR_MODE_AHORRO;
static bool          s_landingArmed  = false;
static unsigned long s_landingT0     = 0;

// ---- Wake por GPIO ----
// Botón: pin -> pulsador -> GND (pull-up interno) => activo en LOW
static inline void setupWakeSourceGPIO() {
  pinMode(WAKE_BTN_PIN, INPUT_PULLUP);                 // WAKE_BTN_PIN definido en config.h
  const uint64_t mask = (1ULL << WAKE_BTN_PIN);
  // Despierta cuando el pin baje a LOW (pulsador a GND):
  esp_deep_sleep_enable_gpio_wakeup(mask, ESP_GPIO_WAKEUP_GPIO_LOW);
}

// Entrar a deep sleep (configura wake y apaga periféricos básicos)
static void enterDeepSleepNow(const char* reason = "inactividad") {
  Serial.printf("Entrando a deep sleep por %s...\n", reason);

  stopBuzzer();             // silenciar
  u8g2.setPowerSave(true);  // apagar OLED
  u8g2.clearBuffer();
  u8g2.sendBuffer();

  setupWakeSourceGPIO();    // botón wake

  delay(30);
  Serial.flush();
  esp_deep_sleep_start();   // ¡a dormir!
}

// Chequear si corresponde dormir (respeta menú, Snake, sleep-lock y modo del sensor)
// 1) Camino aterrizaje fijo (independiente del timeout del menú)
// 2) Camino inactividad (el configurable desde el menú)
static inline void maybeEnterDeepSleep() {
  // --- 1) Deep Sleep fijo post-aterrizaje ---
#if LANDING_DS_ENABLE
  if (s_landingArmed) {
    // No forzar sueño si hay contexto que lo bloquee
    if (!menuActivo && !gameSnakeRunning && !powerLockActive()) {
      // Debe seguir en Ahorro para considerar "en tierra"
      if (getSensorMode() == SENSOR_MODE_AHORRO) {
        // Evita dormir si hubo interacción muy reciente (10 s)
        if (millis() - lastActivityMs >= 10000UL) {
          if (millis() - s_landingT0 >= LANDING_DS_DELAY_MS) {
            enterDeepSleepNow("aterrizaje (5min)");
          }
        }
      } else {
        // Salió de Ahorro (despegó o cambió modo): cancela
        s_landingArmed = false;
      }
    }
  }
#endif

  // --- 2) Deep Sleep por inactividad (configurable en el menú) ---
  if (ahorroTimeoutMs == 0) return;            // OFF
  if (menuActivo) return;                      // no dormir dentro del menú
  if (gameSnakeRunning) return;                // no dormir durante Snake
  if (powerLockActive()) return;               // sleep-lock activo

  // No dormir en Ultra/Freefall
  SensorMode sm = getSensorMode();
  if (sm != SENSOR_MODE_AHORRO) return;

  unsigned long now = millis();
  if ((now - lastActivityMs) >= ahorroTimeoutMs) enterDeepSleepNow("inactividad");
}

void setup() {
  Serial.begin(115200);
  delay(300);
  Serial.println("Setup iniciado");

  markFirmwareAsValid();

  Wire.begin(SDA_PIN, SCL_PIN);
  Wire.setClock(400000);  // 400 kHz

  loadConfig();
  loadUserConfig();
  
  initBuzzer();
  setupBLE();
  initUI();
  initSensor();
  batteryInit();  // <<< Nuevo: inicializar batería
  
  // Botones de UI
  pinMode(BUTTON_ALTITUDE, INPUT_PULLUP);
  pinMode(BUTTON_OLED,     INPUT_PULLUP);
  pinMode(BUTTON_MENU,     INPUT_PULLUP);

  // Fuente de wake preparada desde ya
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
  powerLockUpdate();

  updateSensorData();
  updateUI();
  batteryUpdate();  // <<< Nuevo: actualizar lectura/porcentaje de batería
  
  // ----- Corte por batería baja, pero NUNCA durante el vuelo -----
  // ----- Corte por batería baja, con 40 s de gracia tras aterrizar -----
  SensorMode sm = getSensorMode();
  extern bool enSalto;
  extern bool inJump;

  // “En vuelo” = no Ahorro o con flags de vuelo activos
  bool inFlight = (sm != SENSOR_MODE_AHORRO) || enSalto || inJump;

  // Detectar transición vuelo -> tierra para iniciar la gracia
  static bool prevInFlight = false;
  static unsigned long landedAtMs = 0;

  if (inFlight) {
    prevInFlight = true;
    landedAtMs = 0; // si volvemos a volar, cancela gracia
  } else {
    if (prevInFlight) {           // acaba de aterrizar
      landedAtMs = millis();      // comienza el período de gracia
      prevInFlight = false;
    }
  }

  // Gracia de 40 s (40000 ms) después de aterrizar
  bool inGraceAfterLanding = (landedAtMs != 0) && ((millis() - landedAtMs) < 40000UL);

  // Dormir por batería solo si: NO en vuelo, NO en gracia y condición de batería baja
  if (!inFlight && !inGraceAfterLanding && batteryShouldDeepSleep()) {
    enterDeepSleepNow("batería baja");
    return; // seguridad (deep sleep no retorna)
  }

  // --- Armado/cancelación del temporizador de aterrizaje (transiciones de modo) ---
#if LANDING_DS_ENABLE
  {
    SensorMode cur = getSensorMode();
    // Aterrizaje = transición desde NO-AHORRO -> AHORRO
    if (s_prevMode != SENSOR_MODE_AHORRO && cur == SENSOR_MODE_AHORRO) {
      s_landingArmed = true;
      s_landingT0    = millis();
      // Serial.println("[LandingTimer] armado (5 min)");
    }
    // Salida de Ahorro cancela el timer (posible despegue/cambio de modo)
    if (s_prevMode == SENSOR_MODE_AHORRO && cur != SENSOR_MODE_AHORRO) {
      s_landingArmed = false;
      // Serial.println("[LandingTimer] cancelado (salió de Ahorro)");
    }
    s_prevMode = cur;
  }
#endif

  // Calibración automática al inicio (una sola vez)
  if (!calibracionRealizada) {
    if (bmp.performReading()) {
      altitudReferencia = bmp.readAltitude(1013.25);
      Serial.println("Calibración inicial: altitud reiniciada a cero.");
#if DEBUG_HZ
      onSampleAccepted();
#endif
    } else {
      Serial.println("Error al leer sensor en calibración inicial.");
    }
    calibracionRealizada = true;
    noteUserActivity();
  }
  
  // Menú
  if (menuActivo) {
    processMenu();
    noteUserActivity();  // hubo interacción
  } else {
    // Abrir menú
    if (digitalRead(BUTTON_MENU) == LOW) {
      noteUserActivity();
      menuActivo = true;
      menuOpcion = 0;
      lastMenuInteraction = millis();
      while (digitalRead(BUTTON_MENU) == LOW) { delay(10); }
      delay(50);
    }
    
    // Recalibración (mantener 1 s) -> ACTIVA SLEEP-LOCK (25 min fijos)
    if (digitalRead(BUTTON_ALTITUDE) == LOW) {
      noteUserActivity();
      unsigned long startTime = millis();
      while (digitalRead(BUTTON_ALTITUDE) == LOW) {
        if (millis() - startTime >= 1000) {
          if (bmp.performReading()) {
            altitudReferencia = bmp.readAltitude(1013.25);
            alturaOffset = 0.0;
            Serial.println("Altitud reiniciada a cero por botón (1s).");
#if DEBUG_HZ
            onSampleAccepted();
#endif
            buzzerBeep(2000, 240, 1000);

            // >>>> ACTIVAR SLEEP-LOCK (opción B: 25 min, independiente del timeout) <<<<
            powerLockActivate();  // usa SLEEP_LOCK_MS_FIXED (25 min) definido en power_lock.h

          } else {
            Serial.println("Error al leer sensor en recalibración manual.");
          }
          while (digitalRead(BUTTON_ALTITUDE) == LOW) { delay(10); }
          delay(50);
          noteUserActivity();
          break;
        }
      }
    }
    
    // Confirmar/seleccionar
    if (digitalRead(BUTTON_OLED) == LOW) {
      noteUserActivity();
      while (digitalRead(BUTTON_OLED) == LOW) { delay(10); }
      delay(50);
    }
  }

  // Reporte de Hz (1 Hz) usando el modo REAL del sensor
#if DEBUG_HZ
  SensorMode modoReal = getSensorMode();
  int modoIdx = (modoReal == SENSOR_MODE_AHORRO) ? 0 :
                (modoReal == SENSOR_MODE_ULTRA_PRECISO) ? 1 : 2;
  hzReportTick(modoIdx);
#endif
  
  // Ritmo de UI según modo real (suaviza CPU)
  SensorMode modo = getSensorMode();
  uint16_t baseDelay = 101;
  if (modo == SENSOR_MODE_ULTRA_PRECISO) baseDelay = 10;
  if (modo == SENSOR_MODE_FREEFALL)     baseDelay = 0;
  delay(baseDelay);

  // Evaluar sueño (aterrizaje fijo primero, luego inactividad)
  maybeEnterDeepSleep();
}
