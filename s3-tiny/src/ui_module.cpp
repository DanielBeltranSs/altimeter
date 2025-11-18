#include "ui_module.h"
#include "config.h"
#include <Arduino.h>
#include <driver/ledc.h>
#include <U8g2lib.h>
#include "sensor_module.h"
#include <math.h>
#include "snake.h"
#include "power_lock.h"
#include "battery.h"
#include "datetime_module.h"
#include "logbookUi.h"
#include "logbook.h"
#include "charge_detect.h"
#include "alarm.h"

// ===== Radios/BT (Arduino-ESP32) =====
#if defined(ARDUINO_ARCH_ESP32)
  #include <WiFi.h>
  extern "C" {
    #include "esp_wifi.h"
    #include "esp_bt.h"
  }
#endif

// ===== Defaults seguros (si no están ya en config.h) =====
#ifndef ALTURA_OFFSET_MIN_M
#define ALTURA_OFFSET_MIN_M   (-300.0f)
#endif
#ifndef ALTURA_OFFSET_MAX_M
#define ALTURA_OFFSET_MAX_M   ( 300.0f)
#endif
#ifndef OFFSET_STEP_M
#define OFFSET_STEP_M         0.1f
#endif
#ifndef OFFSET_STEP_FT
#define OFFSET_STEP_FT        1.0f
#endif
#ifndef OFFSET_ACCEL
#define OFFSET_ACCEL          10.0f
#endif
#ifndef OFFSET_ZERO_EPS_M
#define OFFSET_ZERO_EPS_M     0.05f
#endif

// ===== Power/UI ahorro =====
#ifndef UI_DEADBAND_FT
#define UI_DEADBAND_FT        10.0f
#endif
#ifndef UI_DEADBAND_M
#define UI_DEADBAND_M         3.05f
#endif
#ifndef UI_AHORRO_ALT_STEP_M
#define UI_AHORRO_ALT_STEP_M  1.0f
#endif
#ifndef UI_AHORRO_ALT_STEP_FT
#define UI_AHORRO_ALT_STEP_FT 1.0f
#endif
#ifndef UI_BLINK_MS
#define UI_BLINK_MS           500UL
#endif
#ifndef UI_AHORRO_TICK_MS
#define UI_AHORRO_TICK_MS     800UL       // Tick lento HUD en Ahorro
#endif
#ifndef UI_SLOW_BLINK_MS
#define UI_SLOW_BLINK_MS      2000UL      // Blink lento en Ahorro
#endif

// ===== Escalado de CPU =====
#ifndef CPU_FREQ_AHORRO_MHZ
#define CPU_FREQ_AHORRO_MHZ   40
#endif
#ifndef CPU_FREQ_ACTIVO_MHZ
#define CPU_FREQ_ACTIVO_MHZ   160
#endif
#ifndef CPU_FREQ_RAPIDO_MHZ
#define CPU_FREQ_RAPIDO_MHZ   160
#endif

// ===== Placa: WS2812 DIN (pinout) =====
#ifndef PIN_RGB_DIN
#define PIN_RGB_DIN           38  // GP38
#endif

// ===== Idioma =====
extern int idioma; // LANG_ES / LANG_EN
static String T(const char* es, const char* en) {
  return (idioma == LANG_ES) ? String(es) : String(en);
}

// --- Helper: normalizar altFormat para que solo existan 0 (normal) o 4 (AUTO)
static inline int normalizeAltFormat(int v) { return (v == 4) ? 4 : 0; }

// ==== Entrada no bloqueante ====
struct Btn {
  uint8_t  pin;
  bool     down;
  bool     prev;
  uint32_t tDown;
  uint32_t tNextRpt;
  Btn(uint8_t p): pin(p), down(false), prev(false), tDown(0), tNextRpt(0) {}
  Btn(): pin(0), down(false), prev(false), tDown(0), tNextRpt(0) {}
};

static Btn BTN_ALT (BUTTON_ALTITUDE);
static Btn BTN_OK  (BUTTON_OLED);
static Btn BTN_MENU(BUTTON_MENU);

// --- Contador de repintados ---
static uint32_t g_uiRepaintCounter = 0;
static inline void uiStampRepaintCounter() {
  char cbuf[16];
  snprintf(cbuf, sizeof(cbuf), "R:%lu", (unsigned long)g_uiRepaintCounter);
  u8g2.setFont(u8g2_font_micro_tr);
  u8g2.setCursor(1, 8);
  u8g2.print(cbuf);
}

// Fast-path: fuerza repintado inmediato si hay interacción
static volatile bool uiForceRefresh = false;

// Gating por tick en Ahorro (HUD)
static uint32_t s_ui_next_allowed_ms = 0;

void uiRequestRefresh() {
  uiForceRefresh = true;
  s_ui_next_allowed_ms = 0;  // permite pintar ya, sin esperar al próximo tick
}

static inline void btnTick(Btn& b) {
  b.prev = b.down;
  b.down = (digitalRead(b.pin) == HIGH);
  if (b.down && !b.prev) {
    b.tDown    = millis();
    b.tNextRpt = 0;
    uiForceRefresh = true; // fast-path
  }
}
static inline bool btnRise(const Btn& b) { return b.down && !b.prev; }
static inline bool btnRepeat(Btn& b, uint32_t firstDelay, uint32_t period) {
  if (!b.down) return false;
  uint32_t now = millis();
  if (now - b.tDown < firstDelay) return false;
  if (b.tNextRpt == 0) { b.tNextRpt = now; return true; }
  if (now >= b.tNextRpt) { b.tNextRpt = now + period; return true; }
  return false;
}
static inline bool btnLong(const Btn& b, uint32_t longMs=600) {
  return b.down && (millis() - b.tDown >= longMs);
}

// ==== Display ====
U8G2_ST7567_JLX12864_F_4W_SW_SPI u8g2(
  U8G2_R2, LCD_SCK, LCD_MOSI, LCD_CS, LCD_DC, LCD_RST
);
static inline void backlightDuty(uint8_t duty) {
  ledc_set_duty(LEDC_LOW_SPEED_MODE, (ledc_channel_t)LCD_LEDC_CH, duty);
  ledc_update_duty(LEDC_LOW_SPEED_MODE, (ledc_channel_t)LCD_LEDC_CH);
}

// Backlight activo-bajo:
//  - duty = 255  -> pin alto todo el periodo  -> BL APAGADO
//  - duty pequeño -> más tiempo en bajo      -> BL más brillante
static bool s_backlightEnabled = false;
static inline uint8_t blDutyFromUser() {
  int b = brilloPantalla; if (b < 0) b = 0; if (b > 255) b = 255;
  return (uint8_t)(255 - b);   // invertir por activo-bajo
}
static inline void backlightOff() { backlightDuty(0); s_backlightEnabled = false; }
static inline void backlightOnUser() { backlightDuty(blDutyFromUser()); s_backlightEnabled = true; }


static void backlightInit() {
   pinMode(LCD_LED, OUTPUT);
   digitalWrite(LCD_LED, HIGH);

  ledc_timer_config_t t = {
    .speed_mode      = LEDC_LOW_SPEED_MODE,
    .duty_resolution = (ledc_timer_bit_t)LCD_LEDC_RES_BITS,
    .timer_num       = LEDC_TIMER_0,
    .freq_hz         = LCD_LEDC_FREQ,
    .clk_cfg         = LEDC_AUTO_CLK
  };
  ledc_timer_config(&t);
  ledc_channel_config_t ch = {
    .gpio_num   = (int)LCD_LED,
    .speed_mode = LEDC_LOW_SPEED_MODE,
    .channel    = (ledc_channel_t)LCD_LEDC_CH,
    .intr_type  = LEDC_INTR_DISABLE,
    .timer_sel  = LEDC_TIMER_0,
    .duty       = 0,
    .hpoint     = 0
  };
  ledc_channel_config(&ch);
  backlightOff(); // asegurar BL apagado al iniciar (por activo-bajo)
}


bool startupDone = false;

// ==== Config/UI global ====
const int TOTAL_OPCIONES      = 11;
const int OPCIONES_POR_PAGINA = 4;

bool gameSnakeRunning   = false;
bool editingOffset      = false;
float offsetTemp        = 0.0f;
int  menuOpcion         = 0;
bool menuActivo         = false;
bool batteryMenuActive  = false;

// ==== Externs ====
extern bool  unidadMetros;
extern int   brilloPantalla;
extern int   altFormat;
extern unsigned long ahorroTimeoutMs;
extern int   ahorroTimeoutOption;
extern float alturaOffset;
extern String usuarioActual;

extern bool  pantallaEncendida;
extern float altCalculada;
extern bool  jumpArmed;
extern bool  inJump;

extern volatile uint32_t uiBlockMenuOpenUntilMs;
volatile uint32_t uiBlockMenuOpenUntilMs = 0;

long lastMenuInteraction = 0;
unsigned long getLastActivityMs();

// ==== Auto (según unidad visible) ====
static int s_autoFmt  = 0;
static int s_autoBand = 0;

static uint32_t s_blockMenuSelectUntilMs = 0;
static bool     s_firstFrameMenu = true;

static void autoUpdateFormat(float altRel_m) {
  float alt = unidadMetros ? fabsf(altRel_m) : fabsf(altRel_m * 3.281f);
  int newBand = (alt < 999.0f) ? 0 : (alt < 9999.0f) ? 1 : 2;
  s_autoBand = newBand;
  switch (s_autoBand) {
    case 0: s_autoFmt = 0; break;
    case 1: s_autoFmt = 2; break;
    case 2: s_autoFmt = 1; break;
    default: s_autoFmt = 0; break;
  }
}

// ====== Gating HUD (Ahorro) ======
static struct {
  bool     first = true;
  bool     inDeadband = false;
  float    lastAltShown = NAN;
  int      lastPct = -1;
  bool     lastUsb = false;
  int      lastTempInt = INT32_MIN;
  uint16_t lastHHMM = 0xFFFF;
  bool     lastBatBlinkOn = false;
  bool     lastShowMoon = false;
} s_uiA;

static inline uint32_t currentBlinkPeriod() {
  return (getSensorMode()==SENSOR_MODE_AHORRO) ? UI_SLOW_BLINK_MS : UI_BLINK_MS;
}
static inline bool bat_blink_now() {
  if (!batteryIsLowPercent()) return true;
  return ((millis() / currentBlinkPeriod()) % 2UL) == 0UL;
}
static inline bool moon_blink_now(bool usb) {
  if (ahorroTimeoutMs == 0) return false;
  if (powerLockActive())     return false;
  if (getSensorMode()!=SENSOR_MODE_AHORRO) return false;
  if (usb) return false;
  unsigned long now = millis();
  long left = (long)ahorroTimeoutMs - (long)(now - getLastActivityMs());
  if (!(left > 0 && left <= 120000L)) return false;
  return ((now / UI_SLOW_BLINK_MS) % 2UL) == 0UL;
}
static inline uint16_t hhmm_to_minutes() {
  char hhmm[6]; datetimeFormatHHMM(hhmm, sizeof(hhmm));
  if (hhmm[0]=='-' || hhmm[1]=='-') return s_uiA.lastHHMM;
  int hh = (hhmm[0]-'0')*10 + (hhmm[1]-'0');
  int mm = (hhmm[3]-'0')*10 + (hhmm[4]-'0');
  return (uint16_t)(hh*60 + mm);
}
static bool ui_should_repaint_ahorro() {
  bool dirty = false;

  uint16_t nowHHMM = hhmm_to_minutes();
  if (s_uiA.first || nowHHMM != s_uiA.lastHHMM) { dirty = true; s_uiA.lastHHMM = nowHHMM; }

  bool usb = isUsbPresent();
  if (s_uiA.first || usb != s_uiA.lastUsb) { dirty = true; s_uiA.lastUsb = usb; }

  bool moonNow  = moon_blink_now(usb);
  int  tInt     = (int)lroundf(bmp.temperature);
  if (s_uiA.first || moonNow != s_uiA.lastShowMoon) { dirty = true; }
  if (!moonNow && (s_uiA.first || tInt != s_uiA.lastTempInt)) { dirty = true; }
  s_uiA.lastShowMoon = moonNow;
  s_uiA.lastTempInt  = tInt;

  int pct = batteryGetPercent();
  bool batBlinkOn = bat_blink_now();
  if (s_uiA.first || pct != s_uiA.lastPct || batBlinkOn != s_uiA.lastBatBlinkOn) {
    dirty = true; s_uiA.lastPct = pct; s_uiA.lastBatBlinkOn = batBlinkOn;
  }

  float altRel_m = altCalculada;
  float rel_from_offset_m = altRel_m - alturaOffset;
  bool inDB = unidadMetros
              ? (fabsf(rel_from_offset_m) < UI_DEADBAND_M)
              : (fabsf(rel_from_offset_m * 3.281f) < UI_DEADBAND_FT);
  float altShow = unidadMetros ? (inDB ? alturaOffset : altRel_m)
                               : (inDB ? alturaOffset*3.281f : altRel_m*3.281f);
  if (s_uiA.first || inDB != s_uiA.inDeadband) { dirty = true; s_uiA.inDeadband = inDB; }

  float step = unidadMetros ? UI_AHORRO_ALT_STEP_M : UI_AHORRO_ALT_STEP_FT;
  if (!inDB) {
    if (s_uiA.first || !isfinite(s_uiA.lastAltShown) || fabsf(altShow - s_uiA.lastAltShown) >= step) {
      dirty = true; s_uiA.lastAltShown = altShow;
    }
  } else {
    if (s_uiA.first || !isfinite(s_uiA.lastAltShown) || fabsf(altShow - s_uiA.lastAltShown) >= 0.5f) {
      dirty = true; s_uiA.lastAltShown = altShow;
    }
  }
  s_uiA.first = false;
  return dirty;
}

// ===== Radios y RGB OFF =====
static void boardLowPowerInit() {
#if defined(ARDUINO_ARCH_ESP32)
  WiFi.mode(WIFI_OFF);
  WiFi.disconnect(true, true);
  esp_wifi_stop();
  #if defined(CONFIG_BT_ENABLED) || defined(CONFIG_BT_BLUEDROID_ENABLED) || defined(CONFIG_BT_NIMBLE_ENABLED)
    btStop();
    esp_bt_controller_disable();
  #endif
#endif
  pinMode(PIN_RGB_DIN, OUTPUT);
  digitalWrite(PIN_RGB_DIN, LOW);
  delayMicroseconds(100);
  pinMode(PIN_RGB_DIN, INPUT);
}

// ===== Escalado de CPU según modo + interacción =====
static SensorMode s_lastCpuPolicy = (SensorMode)255;
static void powerPolicyTick() {
  SensorMode m = getSensorMode();
  // “Interacción”: menú, edición, datetime, logbook o juego
  bool interactive = menuActivo || editingOffset || logbookUiIsActive() || datetimeMenuActive() || gameSnakeRunning;

  uint32_t targetMHz;
  if (m == SENSOR_MODE_FREEFALL) {
    targetMHz = CPU_FREQ_RAPIDO_MHZ;           // prioridad a FF
  } else if (interactive) {
    targetMHz = CPU_FREQ_ACTIVO_MHZ;           // UI fluida en tierra
  } else if (m == SENSOR_MODE_AHORRO) {
    targetMHz = CPU_FREQ_AHORRO_MHZ;
  } else {
    targetMHz = CPU_FREQ_ACTIVO_MHZ;
  }

  // Cambia solo si varía el modo (usamos m como “política” vigente)
  static uint32_t s_curMHz = 0;
  if (s_curMHz != targetMHz || m != s_lastCpuPolicy) {
    setCpuFrequencyMhz(targetMHz);
    s_curMHz = targetMHz;
    s_lastCpuPolicy = m;
  }
}

// ---------------------------------------------------------------------------
void initUI() {
  altFormat = normalizeAltFormat(altFormat);
  u8g2.begin();
  u8g2.setPowerSave(false);
  u8g2.setContrast(150);
  backlightInit();
  backlightOff();

  boardLowPowerInit();
  powerPolicyTick();

  Serial.println(T("Display iniciado.", "Display started."));
}

// API pública para backlight
void lcdBacklightOnUser() { backlightOnUser(); }
void lcdBacklightOff()    { backlightOff(); }
void lcdBacklightToggle() { if (s_backlightEnabled) backlightOff(); else backlightOnUser(); }
bool lcdBacklightIsOn()   { return s_backlightEnabled; }


// ---------------------------------------------------------------------------
void mostrarCuentaRegresiva() {
  static unsigned long startupStartTime = 0;
  static uint32_t nextFrame = 0;
  uint32_t now = millis();
  if (startupStartTime == 0) startupStartTime = now;
  if (now < nextFrame) return;
  nextFrame = now + 100;

  unsigned long elapsed = now - startupStartTime;
  int secondsLeft = 3 - int(elapsed / 1000UL);
  if (secondsLeft < 0) secondsLeft = 0;

  u8g2.clearBuffer();
  u8g2.setFont(u8g2_font_fub30_tr);
  String cuenta = String(secondsLeft);
  int xPos = (128 - u8g2.getStrWidth(cuenta.c_str())) / 2; if (xPos < 0) xPos = 0;
  u8g2.setCursor(xPos, 40); u8g2.print(cuenta);

  u8g2.setFont(u8g2_font_ncenB08_tr);
  String ini = T("Iniciando...", "Starting...");
  int w = u8g2.getStrWidth(ini.c_str()); int x = (128 - w) / 2; if (x < 0) x = 0;
  u8g2.setCursor(x, 60); u8g2.print(ini);

  g_uiRepaintCounter++; uiStampRepaintCounter(); u8g2.sendBuffer();
  if (elapsed >= 3000) startupDone = true;
}

// ---------------------------------------------------------------------------
void dibujarMenu() {
  int paginaActual = menuOpcion / OPCIONES_POR_PAGINA;
  int totalPaginas = (TOTAL_OPCIONES + OPCIONES_POR_PAGINA - 1) / OPCIONES_POR_PAGINA;
  int inicio = paginaActual * OPCIONES_POR_PAGINA;
  int fin = inicio + OPCIONES_POR_PAGINA; if (fin > TOTAL_OPCIONES) fin = TOTAL_OPCIONES;

  u8g2.clearBuffer();
  u8g2.setFont(u8g2_font_ncenB08_tr);
  u8g2.setCursor(0, 12); u8g2.print(T("MENU:", "MENU:"));

  // Fecha (DD/MM/YY) — ya se actualiza 1s en bloque
  {
    static uint32_t lastUpd = 0;
    static char dstr[9] = "--/--/--";
    const int X_DATE = 80, Y_DATE = 12;
    uint32_t now = millis();
    if (now - lastUpd >= 1000) {
      char ymd[11]; datetimeFormatYMD(ymd, sizeof(ymd));
      if (ymd[0] != '-') {
        dstr[0]=ymd[8]; dstr[1]=ymd[9]; dstr[2]='/';
        dstr[3]=ymd[5]; dstr[4]=ymd[6]; dstr[5]='/';
        dstr[6]=ymd[2]; dstr[7]=ymd[3]; dstr[8]='\0';
      } else memcpy(dstr, "--/--/--", 9);
      lastUpd = now;
    }
    u8g2.setCursor(X_DATE, Y_DATE); u8g2.print(dstr);
  }

  // Voltaje (muestra, pero el muestreo real lo hace batteryUpdate)
  {
    float vbat = batteryGetVoltage();
    u8g2.setCursor(95, 24); u8g2.print(vbat, 2); u8g2.print("V");
  }

  for (int i = inicio; i < fin; i++) {
    int y = 24 + (i - inicio) * 12;
    u8g2.setCursor(0, y);
    u8g2.print(i == menuOpcion ? "> " : "  ");
    switch(i) {
      case 0: u8g2.print(T("Unidad: ", "Units: ")); u8g2.print(unidadMetros ? T("metros", "meters") : T("pies", "feet")); break;
      case 1: u8g2.print(T("Brillo: ", "Brightness: ")); u8g2.print(brilloPantalla); break;
      case 2: u8g2.print(T("Altura: ", "Altitude fmt: ")); u8g2.print(normalizeAltFormat(altFormat) == 4 ? "AUTO" : T("normal", "normal")); break;
      case 3: u8g2.print(T("Bitacora", "Logbook")); break;
      case 4: u8g2.print(T("Fecha/Hora", "Date/Time")); break;
      case 5: u8g2.print(T("Empty: ", "Empty: ")); break;
      case 6: u8g2.print(T("Ahorro: ", "Power save: ")); if (ahorroTimeoutMs == 0) u8g2.print("OFF"); else u8g2.print(String(ahorroTimeoutMs / 60000) + T(" min", " min")); break;
      case 7:
        u8g2.print("Offset: ");
        if (unidadMetros) { u8g2.print(alturaOffset, 2); u8g2.print(" m"); }
        else              { u8g2.print(alturaOffset * 3.281f, 0); u8g2.print(" ft"); }
        break;
      case 8: u8g2.print("Snake"); break;
      case 9: u8g2.print(T("Idioma: ", "Language: ")); u8g2.print((idioma == LANG_ES) ? "ES" : "EN"); break;
      case 10: u8g2.print(T("Salir del menú", "Exit menu")); break;
    }
  }

  u8g2.setCursor(100, 63);
  u8g2.print(String(paginaActual + 1) + "/" + String(totalPaginas));
  g_uiRepaintCounter++; uiStampRepaintCounter(); u8g2.sendBuffer();
}

// ---------------------------------------------------------------------------
static void dibujarOffsetEdit() {
  u8g2.clearBuffer();
  u8g2.setFont(u8g2_font_ncenB08_tr);
  u8g2.setCursor(5, 18); u8g2.print(T("Offset de altitud", "Altitude offset"));

  u8g2.setFont(u8g2_font_ncenB18_tr);
  u8g2.setCursor(5, 50);
  if (unidadMetros) { u8g2.print(offsetTemp, 2); u8g2.print(" m"); }
  else              { u8g2.print(offsetTemp * 3.281f, 0); u8g2.print(" ft"); }

  u8g2.setFont(u8g2_font_5x7_tf);
  u8g2.setCursor(5, 63);
  u8g2.print(T("OK + / ALT - | MENU Guarda | ALT+MENU Cancela | OK+ALT = 0",
               "OK + / ALT - | MENU Save   | ALT+MENU Cancel  | OK+ALT = 0"));

  g_uiRepaintCounter++; uiStampRepaintCounter(); u8g2.sendBuffer();
}

// ====== Gating de dibujo del MENÚ ======
static uint32_t s_menu_next_draw_ms = 0;
static inline void maybeDrawMenu() {
  uint32_t now = millis();
  // periodo base de menú (rápido fuera de Ahorro)
  uint32_t period = (getSensorMode()==SENSOR_MODE_AHORRO) ? 120UL : 80UL;

  if (uiForceRefresh || (int32_t)(now - s_menu_next_draw_ms) >= 0) {
    dibujarMenu();
    uiForceRefresh = false;
    s_menu_next_draw_ms = now + period;
  }
}

// ---------------------------------------------------------------------------
static void ejecutarOpcionMenu(int opcion) {
  switch(opcion) {
    case 0: unidadMetros = !unidadMetros; break;
    case 1:
      brilloPantalla += 50; if (brilloPantalla > 255) brilloPantalla = 0;
      // Aplica inmediatamente el nivel de backlight seleccionado
      if (s_backlightEnabled) backlightOnUser();
      break;
    case 2: { altFormat = normalizeAltFormat(altFormat); altFormat = (altFormat == 0) ? 4 : 0; } break;
    case 3: logbookUiOpen(); break;
    case 4: datetimeMenuOpen(); break;
    case 5: break;
    case 6:
      ahorroTimeoutOption = (ahorroTimeoutOption + 1) % NUM_TIMEOUT_OPTIONS;
      ahorroTimeoutMs     = TIMEOUT_OPTIONS[ahorroTimeoutOption];
      break;
    case 7:
      if (!editingOffset) { editingOffset = true; offsetTemp = alturaOffset; dibujarOffsetEdit(); }
      break;
    case 8:
      if (!gameSnakeRunning) {
        gameSnakeRunning = true; menuActivo = false; uiBlockMenuOpenUntilMs = millis() + 220;
        s_firstFrameMenu = true; s_blockMenuSelectUntilMs = 0;
      }
      break;
    case 9: idioma = (idioma == LANG_ES) ? LANG_EN : LANG_ES; break;
    case 10: { // Salir
      menuActivo = false;
      lastMenuInteraction = millis();
      uiBlockMenuOpenUntilMs = millis() + 300;
      // Reseteo de flags anti-flanco por cierre
      s_firstFrameMenu = true;
      s_blockMenuSelectUntilMs = 0;

      uiRequestRefresh();     // <<< fuerza redibujado inmediato de la UI
      return;                 // evitá seguir y re-dibujar menú por accidente
    }
  }
  saveConfig();
  uiForceRefresh = true; // feedback inmediato en menú
}

// ---------------------------------------------------------------------------
void processMenu() {
  // CPU en modo interactivo (lo refuerza powerPolicyTick en updateUI)
  btnTick(BTN_ALT); btnTick(BTN_OK); btnTick(BTN_MENU);

  if (s_firstFrameMenu) {
    s_blockMenuSelectUntilMs = millis() + 220;
    BTN_ALT.prev  = BTN_ALT.down; BTN_OK.prev   = BTN_OK.down; BTN_MENU.prev = BTN_MENU.down;
    s_firstFrameMenu = false;
    uiForceRefresh = true; // dibuja al entrar
  }

  // Submenús que toman control completo
  if (logbookUiIsActive())  { lastMenuInteraction = millis(); return; }
  if (datetimeMenuActive()) { lastMenuInteraction = millis(); return; }

  // Ignorar entradas los primeros ms al entrar al menú pero sigue dibujando suave
  if (millis() < uiBlockMenuOpenUntilMs) { maybeDrawMenu(); return; }

  // ====== EDICIÓN DE OFFSET ======
  if (editingOffset) {
    bool changed = false;
    if (BTN_ALT.down && BTN_MENU.down) { editingOffset = false; lastMenuInteraction = millis(); uiForceRefresh = true; return; }
    if (BTN_OK.down && BTN_ALT.down) { offsetTemp = 0.0f; changed = true; }

    float base_step_m = unidadMetros ? OFFSET_STEP_M : (OFFSET_STEP_FT * 0.3048f);
    bool isLong = btnLong(BTN_OK) || btnLong(BTN_ALT);
    float step_m = isLong ? (base_step_m * OFFSET_ACCEL) : base_step_m;

    if (btnRise(BTN_OK)  || btnRepeat(BTN_OK,  400, 120)) { offsetTemp += step_m; changed = true; }
    if (btnRise(BTN_ALT) || btnRepeat(BTN_ALT, 400, 120)) { offsetTemp -= step_m; changed = true; }

    if (changed) {
      if (offsetTemp < ALTURA_OFFSET_MIN_M) offsetTemp = ALTURA_OFFSET_MIN_M;
      if (offsetTemp > ALTURA_OFFSET_MAX_M) offsetTemp = ALTURA_OFFSET_MAX_M;
      float snap_eps = fmaxf(OFFSET_ZERO_EPS_M, base_step_m * 0.5f);
      if (fabsf(offsetTemp) < snap_eps) offsetTemp = 0.0f;
      lastMenuInteraction = millis();
      dibujarOffsetEdit(); // edición es modal: dibuja directo
    }

    if (btnRise(BTN_MENU)) {
      if (fabsf(offsetTemp) < OFFSET_ZERO_EPS_M) offsetTemp = 0.0f;
      alturaOffset = offsetTemp; saveConfig();
      editingOffset = false; lastMenuInteraction = millis(); uiForceRefresh = true;
    }
    return;
  }

  // ====== NAVEGACIÓN DE MENÚ ======
  if (btnRise(BTN_MENU)) {
    if ((int32_t)(millis() - s_blockMenuSelectUntilMs) < 0) { lastMenuInteraction = millis(); maybeDrawMenu(); return; }
    ejecutarOpcionMenu(menuOpcion); lastMenuInteraction = millis(); maybeDrawMenu(); return;
  }

  if (btnRise(BTN_ALT) || btnRepeat(BTN_ALT, 500, 150)) {
    menuOpcion = (menuOpcion + 1) % TOTAL_OPCIONES; lastMenuInteraction = millis(); uiForceRefresh = true;
  }
  if (btnRise(BTN_OK)  || btnRepeat(BTN_OK,  500, 150)) {
    menuOpcion = (menuOpcion - 1 + TOTAL_OPCIONES) % TOTAL_OPCIONES; lastMenuInteraction = millis(); uiForceRefresh = true;
  }

  if (millis() - lastMenuInteraction > 4000) {
    menuActivo = false;
    // Reseteo de flags anti-flanco por cierre automático
    s_firstFrameMenu = true;
    s_blockMenuSelectUntilMs = 0;

    uiRequestRefresh();     // <<< igual que al salir con la opción del menú
    return;                 // salí del processMenu este ciclo
  }


  // Dibujo con gating de menú
  maybeDrawMenu();
}

// --- Atenuación automática en modo Ahorro ---
static bool        ahorroDimmed        = false;
static const uint8_t  AHORRO_DIM_CONTRAST = 5;
static const uint32_t INACTIVITY_DIM_MS   = 120000UL;

static inline void handleAhorroAutoDim() {
  const unsigned long now     = millis();
  const unsigned long lastAct = getLastActivityMs();
  const SensorMode m          = getSensorMode();
  const bool mustRestore = (m != SENSOR_MODE_AHORRO) || powerLockActive() || inJump;

  if (mustRestore) { if (ahorroDimmed) { u8g2.setContrast(150); } ahorroDimmed = false; return; }
  if (!ahorroDimmed && (now - lastAct >= INACTIVITY_DIM_MS)) { u8g2.setContrast(150); ahorroDimmed = true; return; }
  if (ahorroDimmed && (now - lastAct) <= 1000UL) { u8g2.setContrast(150); ahorroDimmed = false; }
}

// ---------------------------------------------------------------------------
void updateUI() {
  if (!startupDone) { mostrarCuentaRegresiva(); return; }

  // Re-aplica política de CPU (sube a ACTIVO si hay interacción)
  powerPolicyTick();

  // Redibujo inmediato al cambiar lock (candado)
  static bool s_prevLock = false;
  bool lockNow = powerLockActive();
  if (lockNow != s_prevLock) { uiForceRefresh = true; s_prevLock = lockNow; }

  if (gameSnakeRunning) { playSnakeGame(); return; }
  if (!pantallaEncendida) return;

  handleAhorroAutoDim();

  if (!menuActivo) {
    // Throttling HUD por modo
    static uint32_t t_last_ui = 0;
    uint16_t ui_interval = 140;
    SensorMode m = getSensorMode();
    if (m == SENSOR_MODE_ULTRA_PRECISO) ui_interval = 100;
    if (m == SENSOR_MODE_FREEFALL)     ui_interval = 80;

    uint32_t now_ui = millis();
    if (now_ui - t_last_ui < ui_interval) return;
    t_last_ui = now_ui;

    // Tick lento + gating solo en Ahorro
    if (m == SENSOR_MODE_AHORRO) {
      if (!uiForceRefresh && (int32_t)(now_ui - s_ui_next_allowed_ms) < 0) return;
      s_ui_next_allowed_ms = now_ui + UI_AHORRO_TICK_MS;
      if (!uiForceRefresh && !ui_should_repaint_ahorro()) return;
      uiForceRefresh = false;
    }

    // -------- HUD --------
    u8g2.clearBuffer();

    u8g2.setFont(u8g2_font_ncenB08_tr);
    u8g2.setCursor(2, 12); u8g2.print(unidadMetros ? "M" : "FT");

    { char hhmm[6]; datetimeFormatHHMM(hhmm, sizeof(hhmm));
      int w = u8g2.getStrWidth(hhmm); int x = (128 - w)/2; if (x < 0) x = 0;
      u8g2.setCursor(x, 12); u8g2.print(hhmm); }

    bool mostreSuspIcono = false;
    if (ahorroTimeoutMs > 0 && !powerLockActive() && getSensorMode() == SENSOR_MODE_AHORRO && !isUsbPresent()) {
      bool blinkOn = moon_blink_now(false);
      if (blinkOn) {
        u8g2.setFont(u8g2_font_open_iconic_weather_1x_t);
        u8g2.drawGlyph(18, 12, 66);
        u8g2.setFont(u8g2_font_5x8_mf);
        u8g2.drawStr(27, 10, "zzz");
        mostreSuspIcono = true;
      }
    }
    if (!mostreSuspIcono) {
      char tbuf[8]; float tC = bmp.temperature;
      snprintf(tbuf, sizeof(tbuf), "%.0f°C", tC);
      u8g2.setFont(u8g2_font_6x10_tf); u8g2.drawUTF8(23, 12, tbuf);
    }

    { int pct = batteryGetPercent(); if (bat_blink_now()) {
        u8g2.setFont(u8g2_font_ncenB08_tr);
        String batStr = String(pct) + "%";
        int batWidth = u8g2.getStrWidth(batStr.c_str());
        u8g2.setCursor(128 - batWidth - 2, 12); u8g2.print(batStr);
      }
    }

    if (isUsbPresent()) { u8g2.setFont(u8g2_font_open_iconic_other_1x_t); u8g2.drawGlyph(90, 12, 64); }

    float altRel_m = altCalculada;
    float rel_from_offset_m = altRel_m - alturaOffset;
    float altToShow;
    if (unidadMetros) {
      altToShow = (fabsf(rel_from_offset_m) < UI_DEADBAND_M) ? alturaOffset : altRel_m;
    } else {
      float rel_from_offset_ft = rel_from_offset_m * 3.281f;
      altToShow = (fabsf(rel_from_offset_ft) < UI_DEADBAND_FT) ? (alturaOffset*3.281f) : (altRel_m*3.281f);
    }

    String altDisplay;
    int fmt = normalizeAltFormat(altFormat);
    if (fmt == 4) {
      float absAlt = fabsf(altToShow);
      if      (absAlt <  999.0f) { long v = lroundf(altToShow); altDisplay = String(v); }
      else if (absAlt < 9999.0f) { float vDisp = roundf((altToShow/1000.0f)*100.0f)/100.0f; altDisplay = String(vDisp, 2); }
      else                       { float vDisp = roundf((altToShow/1000.0f)*10.0f)/10.0f;  altDisplay = String(vDisp, 1); }
    } else altDisplay = String((long)altToShow);

    u8g2.setFont(u8g2_font_fub30_tr);
    int xPosAlt = (128 - u8g2.getStrWidth(altDisplay.c_str())) / 2; if (xPosAlt < 0) xPosAlt = 0;
    u8g2.setCursor(xPosAlt, 50); u8g2.print(altDisplay);

    u8g2.drawHLine(0, 15, 128); u8g2.drawHLine(0, 52, 128);
    u8g2.drawHLine(0,  0, 128); u8g2.drawHLine(0, 63, 128);
    u8g2.drawVLine(0,  0, 64);  u8g2.drawVLine(127,0, 64);

    u8g2.setFont(u8g2_font_ncenB08_tr);
    String user = usuarioActual;
    int xPosUser = (128 - u8g2.getStrWidth(user.c_str())) / 2; if (xPosUser < 0) xPosUser = 0;
    u8g2.setCursor(xPosUser, 62); u8g2.print(user);

    uint32_t lifetime = 0; logbookGetTotal(lifetime);
    String jumpStr = String(lifetime);
    u8g2.setCursor(128 - u8g2.getStrWidth(jumpStr.c_str()) - 14, 62); u8g2.print(jumpStr);

    if (jumpArmed || inJump) { if (inJump) u8g2.drawDisc(14, 58, 3); else u8g2.drawCircle(14, 58, 3); }
    if (powerLockActive()) { u8g2.setFont(u8g2_font_open_iconic_thing_1x_t); u8g2.drawGlyph(26, 63, 79); alarmOnLockAltitude(); }

    g_uiRepaintCounter++; uiStampRepaintCounter(); u8g2.sendBuffer();

  } else {
    if (datetimeMenuActive()) { datetimeMenuDrawAndHandle(); return; }
    if (editingOffset)       { dibujarOffsetEdit(); return; }

    if (batteryMenuActive) {
      if (pantallaEncendida) {
        float vbat = batteryGetVoltage(); int pct  = batteryGetPercent();
        u8g2.clearBuffer();
        u8g2.setFont(u8g2_font_ncenB08_tr);
        u8g2.setCursor(0,12); u8g2.print(T("BATERIA:", "BATTERY:"));
        u8g2.setCursor(0,28); u8g2.print("V_Bat: "); u8g2.setCursor(50,28); u8g2.print(vbat, 2); u8g2.print("V");
        u8g2.setCursor(0,44); u8g2.print(T("Carga: ", "Charge: ")); u8g2.setCursor(50,44); u8g2.print(pct); u8g2.print("%");
        g_uiRepaintCounter++; uiStampRepaintCounter(); u8g2.sendBuffer();

        btnTick(BTN_OK);
        if (btnRise(BTN_OK)) { batteryMenuActive = false; lastMenuInteraction = millis(); uiForceRefresh = true; }
      }
      return;
    }

    if (logbookUiIsActive()) { logbookUiDrawAndHandle(u8g2); return; }

    // Dibujo del menú con gating
    maybeDrawMenu();
  }
}
