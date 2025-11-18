#include <Arduino.h>
#include <Wire.h>
#include <math.h>
#include "board_pins.h"

#include "drivers/bmp390_bosch.h"
#include "services/altitude_estimator.h"
#include "services/auto_ground_zero.h"
#include "drivers/button.h"
#include "services/display_mgr.h"
#include "services/ble_manager.h"
#include "app/flight_mode.h"
#include "services/sensor_profile.h"
#include "drivers/buzzer.h"

// === Offset persistente ===
#include "config/device_config.h"
#include "config/nvs_store.h"
#include "services/altitude_frame.h"

#include "esp_sleep.h"
#if __has_include(<esp_wifi.h>)
  #include <esp_wifi.h>
#endif
#if __has_include(<esp_bt.h>)
  #include <esp_bt.h>
#endif

// ==== Unidades / Config ====
static constexpr float M2FT = 3.280839895f;
static constexpr uint32_t OLED_BUMP_MINUTES = 2;
static constexpr uint32_t OLED_BUMP_MAX_MIN = 10;
static constexpr uint16_t FORCED_WAIT_MS = 30;
static constexpr uint32_t BLE_WINDOW_MS = 120000;
static constexpr uint32_t BLE_DISC_GRACE_MS = 10000;

// ==== Objetos globales ====
static BMP390Bosch        gBmp;
static AltitudeEstimator  gAlt;
static AutoGroundZero     gAgz;
static FlightModeDetector gFsm;
static FlightMode         gMode = FlightMode::GROUND;
static Button             gBtn;
#ifdef ENABLE_DISPLAY
static DisplayMgr         gDisp;
#endif
static BleManager         gBle;
static SensorProfile      gProf;
static Buzzer             gBuzz;

// === Config + marco de presentación con offset ===
static DeviceConfig       gCfg;
static AltitudeFrame      gFrame;

// Loop timing
static uint32_t gLoopPeriodMs = 2000;   // GROUND por defecto
static bool     gNormalStreaming = false;

// ===== Helpers =====

// Light-sleep temporizado sin usar delay().
// Si force==false, respeta la política de "no dormir en FF/CANOPY o pantalla ON".
// Si force==true, duerme igual (útil para esperas cortas de conversión del sensor).
static inline void sleep_ms_lp(uint32_t ms, bool force = false) {
  if (ms == 0) return;
#ifdef ENABLE_DISPLAY
  if (!force && gDisp.isOn()) return; // no dormir si pantalla encendida (salvo force)
#endif
  if (!force) {
    if (gMode == FlightMode::FREEFALL || gMode == FlightMode::CANOPY) return;
  }
  esp_sleep_enable_timer_wakeup((uint64_t)ms * 1000ULL);
  esp_light_sleep_start();
}

// Calibración de p0 promediando en modo forced (sin delay())
static float calibrateP0() {
  Serial.printf("Calibrando p0...\n");
  double acc = 0; int ok = 0;
  float p, t;
  gBmp.setForcedMode(BMP3_OVERSAMPLING_2X, BMP3_NO_OVERSAMPLING, BMP3_IIR_FILTER_COEFF_1);
  for (int i = 0; i < 50; ++i) {
    gBmp.triggerForcedMeasurement();
    sleep_ms_lp(FORCED_WAIT_MS, /*force=*/true);
    if (gBmp.read(p, t)) { acc += p; ++ok; }
  }
  float p0 = ok ? static_cast<float>(acc / ok) : 101325.0f;
  Serial.printf("p0=%.2f Pa (%d)\n", p0, ok);
  return p0;
}

static void handleButton(BtnEvent ev, uint32_t now) {
  switch (ev) {
    case BtnEvent::Short:
#ifdef ENABLE_DISPLAY
      gDisp.on();
      gDisp.bumpMinutes(OLED_BUMP_MINUTES, OLED_BUMP_MAX_MIN);
      gDisp.showStatus("DISPLAY", "ACTIVE");
#endif
      break;

    case BtnEvent::Long3:
      if (gMode == FlightMode::GROUND) {
        // Zero rápido a presión actual (promedio corto)
        double acc=0; int ok=0; float p,t;
        for (int i=0;i<10;++i){
          gBmp.triggerForcedMeasurement();
          sleep_ms_lp(FORCED_WAIT_MS, /*force=*/true);
          if (gBmp.read(p,t)) { acc+=p; ++ok; }
        }
        if (ok){
          float p0 = static_cast<float>(acc/ok);
          gAlt.setSeaLevelPressure(p0);
          gAgz.begin(p0);
          gAgz.setFsm(&gFsm);
          // Re-sembrar FSM en la altitud actual para que AGL = 0 tras ZERO
          float p_now, t_now;
          if (gBmp.read(p_now, t_now)) {
            float alt_now = gAlt.filter(gAlt.toAltitudeMeters(p_now));
            gFsm.begin(alt_now);
          }
          Serial.printf("ZERO OK p0=%.2f Pa\n", p0);
#ifdef ENABLE_DISPLAY
          if (gDisp.isOn()) gDisp.showStatus("0", "OK");
#endif
          gBuzz.playCalibrationOk();
        } else {
          Serial.println("ZERO FAIL");
#ifdef ENABLE_DISPLAY
          if (gDisp.isOn()) gDisp.showStatus("ZERO", "FAIL");
#endif
        }
      } else {
        Serial.println("ZERO DENIED: not GROUND");
#ifdef ENABLE_DISPLAY
        if (gDisp.isOn()) gDisp.showStatus("DENIED", "NOT GROUND");
#endif
      }
      break;

    case BtnEvent::Long8:
      if (gMode == FlightMode::GROUND) {
        gBle.enable(now, BLE_WINDOW_MS);
#ifdef ENABLE_DISPLAY
        if (gDisp.isOn()) gDisp.showStatus("BLE", "ENABLED 2m");
        gDisp.setBleIndicator(true);
#endif
        gBuzz.playBleEnabled();
      } else {
        Serial.println("BLE DENIED: not GROUND");
#ifdef ENABLE_DISPLAY
        if (gDisp.isOn()) gDisp.showStatus("DENIED", "NOT GROUND");
#endif
      }
      break;

    case BtnEvent::None:
    default: break;
  }
}

// Decide si usar light sleep (para el *sleep de bucle*, no para esperas cortas del sensor)
static inline bool canSleepLight() {
#ifdef ENABLE_DISPLAY
  if (gDisp.isOn()) return false; // pantalla encendida ⇒ no dormir
#endif
  // No dormir en FREEFALL ni CANOPY
  if (gMode == FlightMode::FREEFALL || gMode == FlightMode::CANOPY) return false;
  // GROUND y CLIMB sí pueden dormir
  return true;
}

void setup() {
  Serial.begin(115200);
  delay(300);

  Wire.begin(PIN_I2C_SDA, PIN_I2C_SCL, I2C_HZ);

#if defined(ARDUINO_ARCH_ESP32)
  setCpuFrequencyMhz(40);
#endif
#if __has_include(<esp_wifi.h>)
  esp_wifi_stop();
#endif
#if __has_include(<esp_bt.h>)
  esp_bt_controller_disable();
#endif

  // Botón
  gBtn.begin(PIN_BUTTON, true);
  gBtn.setTimings(30, 250, 2500, 8000);
  gBtn.enableGpioWakeForLightSleep();

  // Buzzer
  gBuzz.begin(PIN_BUZZER, /*active_high=*/true);

  // Pantalla
#ifdef ENABLE_DISPLAY
  gDisp.begin(OLED_I2C_ADDR, I2C_HZ);
  gDisp.on(); gDisp.showStatus("BOOT","OLED TEST");
  sleep_ms_lp(1000, /*force=*/true);
  gDisp.off();
#endif

  // Sensor
  if (!gBmp.begin(Wire, BMP3_ADDR_DEFAULT, I2C_HZ)) {
    Serial.println("ERROR: BMP390 init");
#ifdef ENABLE_DISPLAY
    gDisp.on(); gDisp.showStatus("BMP ERROR","Check I2C");
#endif
    while(true) {
      sleep_ms_lp(1000, /*force=*/true);
    }
  }

  // Estimador y p0
  gAlt.setEmaAlpha(0.15f);
  float p0 = calibrateP0();
  gAlt.setSeaLevelPressure(p0);
  gAgz.begin(p0);
  gAgz.setFsm(&gFsm);

  // === Cargar config (NVS) y enlazar a AltitudeFrame ===
  cfgLoad(gCfg);                  // nvs_store ya inicializa con defaults si no existía
  gFrame.setConfig(&gCfg);
  Serial.printf("Offset NVS: %ld mm (%.1f ft) | Units=%s\n",
                (long)gCfg.offset_mm,
                (gCfg.offset_mm/1000.0f)*M2FT,
                (gCfg.units==Units::FEET?"ft":"m"));

  // Perfiles iniciales
  gProf.applyFor(FlightMode::GROUND, gBmp, gLoopPeriodMs, gNormalStreaming);
  gLoopPeriodMs = 2000; // GROUND: 2.0 s

  // FSM seed
  float p,t;
  if (gBmp.read(p,t)) gFsm.begin(gAlt.filter(gAlt.toAltitudeMeters(p)));
  else                gFsm.begin(0.0f);

  Serial.println("mode | disp_ft | raw_ft | P(Pa) | T(C)");
}

void loop() {
  const uint32_t now = millis();

  // 1) Botón en activo
  if (BtnEvent ev = gBtn.poll(now); ev != BtnEvent::None) handleButton(ev, now);

  // 2) Lectura baro
  float p=NAN, t=NAN;
  if (gNormalStreaming) {
    gBmp.read(p, t);
  } else {
    gBmp.triggerForcedMeasurement();
    sleep_ms_lp(FORCED_WAIT_MS, /*force=*/true);
    gBmp.read(p, t);
  }

  if (!isnan(p)) {
    const float alt_m  = gAlt.toAltitudeMeters(p);
    const float alt_sm = gAlt.filter(alt_m);   // AGL cruda/altitud filtrada

    // 2.1) AGZ (solo si estás quieto en tierra)
    bool still = (fabsf(gFsm.vz_mps()) < 0.05f) && (gMode == FlightMode::GROUND);
    if (still && gAgz.update(p, alt_sm, gMode, now)) {
      gAlt.setSeaLevelPressure(gAgz.p0());
      Serial.printf("AGZ p0=%.2f Pa\n", gAgz.p0());
    }

    // 2.2) FSM y perfiles
    FlightMode newMode = gFsm.update(alt_sm, now);
    if (newMode != gMode) {
      gMode = newMode;
      gProf.applyFor(gMode, gBmp, gLoopPeriodMs, gNormalStreaming);
      if (gMode != FlightMode::GROUND) gBle.disable();

      // Ajuste de periodo del bucle según modo
      switch (gMode) {
        case FlightMode::GROUND:  gLoopPeriodMs = 0; break; // 2.0 s
        case FlightMode::CLIMB:   gLoopPeriodMs = 300;  break; // 0.3 s
        case FlightMode::FREEFALL:
        case FlightMode::CANOPY:  /* sin sleep de bucle */      break;
      }
    }

    // === Offset aplicado en METROS para UI/Display ===
    const float agl_raw_m   = alt_sm;                        // física
    const float indicated_m = gFrame.aglIndicated_m(agl_raw_m); // indicada (offset)
    const float indicated_ft= indicated_m * M2FT;

#ifdef ENABLE_DISPLAY
    // Indicador BLE parpadeante según estado real
    gDisp.setBleIndicator(gBle.active());

    if (gDisp.isOn()) {
      // Display espera METROS; él convierte a ft internamente
      gDisp.showAltitude(indicated_m,
        (gMode==FlightMode::FREEFALL?"FREEFALL": gMode==FlightMode::CLIMB?"CLIMB":
         gMode==FlightMode::CANOPY?"CANOPY":"GROUND"));
    }
#endif

    // Serial: indicado + raw en ft para depurar
    Serial.printf("%d | %.1f | %.1f | %.2f | %.2f\n",
      (int)gMode,
      indicated_ft,
      agl_raw_m * M2FT,
      p, t);
  } else {
    Serial.printf("Read FAIL (err=%d)\n", gBmp.lastError());
#ifdef ENABLE_DISPLAY
    if (gDisp.isOn()) gDisp.showStatus("SENSOR","READ FAIL");
#endif
  }

  // 3) Servicios
#ifdef ENABLE_DISPLAY
  gDisp.tick(now);
#endif
  gBle.tick(now, /*must_off*/ gMode != FlightMode::GROUND);

  // 4) Espera del bucle (sin delay)
  Serial.flush();
  if (canSleepLight()) {
    // Espera de bucle + wake por botón (no bloqueante)
    BtnEvent wakeEv = gBtn.lightSleepWaitAndClassify((uint64_t)gLoopPeriodMs * 1000ULL);
    if (wakeEv != BtnEvent::None) handleButton(wakeEv, millis());
  } else {
    // Sin dormir: seguimos inmediatamente
  }
}
