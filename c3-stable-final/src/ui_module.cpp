#include "ui_module.h"
#include "config.h"
#include <Arduino.h>
#include <U8g2lib.h>
#include "sensor_module.h"
#include <math.h>        // fabsf(), powf
#include "snake.h"
#include "power_lock.h"
#include "battery.h"     // Voltaje/% batería desde un solo módulo
#include "datetime_module.h"  // Fecha/Hora (sin RTC externo por ahora)
#include "logbookUi.h"
#include "logbook.h"
#include "charge_detect.h"
#include "alarm.h"

// ===== Defaults seguros (si no están ya en config.h) =====
#ifndef ALTURA_OFFSET_MIN_M
#define ALTURA_OFFSET_MIN_M   (-300.0f)
#endif
#ifndef ALTURA_OFFSET_MAX_M
#define ALTURA_OFFSET_MAX_M   ( 300.0f)
#endif
#ifndef OFFSET_STEP_M
#define OFFSET_STEP_M       0.1f        // 10 cm por toque en METROS
#endif
#ifndef OFFSET_STEP_FT
#define OFFSET_STEP_FT      1.0f        // 1 pie por toque en FEET
#endif
#ifndef OFFSET_ACCEL
#define OFFSET_ACCEL        10.0f       // x10 en pulsación larga
#endif
#ifndef OFFSET_ZERO_EPS_M
#define OFFSET_ZERO_EPS_M   0.05f       // <5 cm se considera 0 al guardar
#endif

// ===== Idioma =====
extern int idioma; // LANG_ES / LANG_EN
static String T(const char* es, const char* en) {
  return (idioma == LANG_ES) ? String(es) : String(en);
}

// --- Helper: normalizar altFormat para que solo existan 0 (normal) o 4 (AUTO)
static inline int normalizeAltFormat(int v) {
  return (v == 4) ? 4 : 0;
}

// ==== Entrada no bloqueante (flancos, hold, auto-repeat) ====
// *ACTIVO-ALTO*: "down" cuando el pin está en HIGH.
struct Btn {
  uint8_t  pin;
  bool     down;
  bool     prev;
  uint32_t tDown;
  uint32_t tNextRpt;

  Btn(uint8_t p) : pin(p), down(false), prev(false), tDown(0), tNextRpt(0) {}
  Btn() : pin(0), down(false), prev(false), tDown(0), tNextRpt(0) {}
};

static Btn BTN_ALT (BUTTON_ALTITUDE);
static Btn BTN_OK  (BUTTON_OLED);
static Btn BTN_MENU(BUTTON_MENU);

static inline void btnTick(Btn& b) {
  b.prev = b.down;
  b.down = (digitalRead(b.pin) == HIGH);  // ACTIVO-ALTO
  if (b.down && !b.prev) {                // flanco de subida (pulsado)
    b.tDown    = millis();
    b.tNextRpt = 0;
  }
}
static inline bool btnRise(const Btn& b) {
  return b.down && !b.prev;
}
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
U8G2_SSD1306_128X64_NONAME_F_HW_I2C u8g2(U8G2_R0, U8X8_PIN_NONE);
bool startupDone = false;

// ==== Config/UI global ====
const int TOTAL_OPCIONES      = 11;  // incluye Idioma
const int OPCIONES_POR_PAGINA = 4;

bool gameSnakeRunning   = false;
bool editingOffset      = false;
float offsetTemp        = 0.0f;
int  menuOpcion         = 0;
bool menuActivo         = false;
bool batteryMenuActive  = false;

// ==== Externs de configuración/estado ====
extern bool  unidadMetros;
extern int   brilloPantalla;
extern int   altFormat;       // AHORA: solo 0=normal o 4=AUTO
extern unsigned long ahorroTimeoutMs;
extern int   ahorroTimeoutOption;
extern float alturaOffset;    // Offset de altitud en metros
extern String usuarioActual;

extern bool  pantallaEncendida;
extern float altCalculada;
extern bool  jumpArmed;
extern bool  inJump;

extern volatile uint32_t uiBlockMenuOpenUntilMs;
volatile uint32_t uiBlockMenuOpenUntilMs = 0;

long lastMenuInteraction = 0;

// Getter que añadimos en main.cpp
unsigned long getLastActivityMs();

// ==== Auto (según unidad visible) ====
static int s_autoFmt  = 0;  // 0/1/2 decimales
static int s_autoBand = 0;  // 0:<999, 1:999..9999, 2:>=10000


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

// ==== Turbo seguro para SSD1306 / clones ====
static void enableOledUltra() {
  u8g2.sendF("c", 0xAE);
  u8g2.sendF("c", 0x8D); u8g2.sendF("c", 0x14);
  u8g2.sendF("c", 0x81); u8g2.sendF("c", 0xFF);
  u8g2.sendF("c", 0xD9); u8g2.sendF("c", 0xFF);
  u8g2.sendF("c", 0xDB); u8g2.sendF("c", 0x40);
  u8g2.sendF("c", 0xAC); u8g2.sendF("c", 0x00);
  extern bool inversionActiva;
  u8g2.sendF("c", inversionActiva ? 0xA7 : 0xA6);
  u8g2.sendF("c", 0xAF);
}

// ---------------------------------------------------------------------------
void initUI() {
  altFormat = normalizeAltFormat(altFormat);

  u8g2.begin();
  enableOledUltra();

  u8g2.setPowerSave(false);
  u8g2.setContrast(brilloPantalla);
  extern bool inversionActiva;
  u8g2.sendF("c", inversionActiva ? 0xA7 : 0xA6);
  Serial.println(
    inversionActiva ? T("Display iniciado en modo invertido.", "Display started in inverted mode.")
                    : T("Display iniciado en modo normal.",   "Display started in normal mode.")
  );
}

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
  int xPos = (128 - u8g2.getStrWidth(cuenta.c_str())) / 2;
  if (xPos < 0) xPos = 0;
  u8g2.setCursor(xPos, 40);
  u8g2.print(cuenta);

  u8g2.setFont(u8g2_font_ncenB08_tr);
  String ini = T("Iniciando...", "Starting...");
  int w = u8g2.getStrWidth(ini.c_str());
  int x = (128 - w) / 2;
  if (x < 0) x = 0;
  u8g2.setCursor(x, 60);
  u8g2.print(ini);
  u8g2.sendBuffer();

  if (elapsed >= 3000) startupDone = true;
}

// ---------------------------------------------------------------------------
void dibujarMenu() {
  int paginaActual = menuOpcion / OPCIONES_POR_PAGINA;
  int totalPaginas = (TOTAL_OPCIONES + OPCIONES_POR_PAGINA - 1) / OPCIONES_POR_PAGINA;
  int inicio = paginaActual * OPCIONES_POR_PAGINA;
  int fin = inicio + OPCIONES_POR_PAGINA;
  if (fin > TOTAL_OPCIONES) fin = TOTAL_OPCIONES;

  u8g2.clearBuffer();
  u8g2.setFont(u8g2_font_ncenB08_tr);
  u8g2.setCursor(0, 12);
  u8g2.print(T("MENU:", "MENU:"));

  // Fecha (DD/MM/YY)
  {
    static uint32_t lastUpd = 0;
    static char dstr[9] = "--/--/--";
    const int X_DATE = 80;
    const int Y_DATE = 12;

    uint32_t now = millis();
    if (now - lastUpd >= 1000) {
      char ymd[11];
      datetimeFormatYMD(ymd, sizeof(ymd));
      if (ymd[0] != '-') {
        dstr[0] = ymd[8]; dstr[1] = ymd[9];
        dstr[2] = '/';
        dstr[3] = ymd[5]; dstr[4] = ymd[6];
        dstr[5] = '/';
        dstr[6] = ymd[2]; dstr[7] = ymd[3];
        dstr[8] = '\0';
      } else {
        memcpy(dstr, "--/--/--", 9);
      }
      lastUpd = now;
    }
    u8g2.setCursor(X_DATE, Y_DATE);
    u8g2.print(dstr);
  }

  // Voltaje
  {
    float vbat = batteryGetVoltage();
    u8g2.setCursor(95, 24);
    u8g2.print(vbat, 2);
    u8g2.print("V");
  }

  for (int i = inicio; i < fin; i++) {
    int y = 24 + (i - inicio) * 12;
    u8g2.setCursor(0, y);
    u8g2.print(i == menuOpcion ? "> " : "  ");
    switch(i) {
      case 0:
        u8g2.print(T("Unidad: ", "Units: "));
        u8g2.print(unidadMetros ? T("metros", "meters") : T("pies", "feet"));
        break;
      case 1:
        u8g2.print(T("Brillo: ", "Brightness: "));
        u8g2.print(brilloPantalla);
        break;
      case 2:
        u8g2.print(T("Altura: ", "Altitude fmt: "));
        u8g2.print(normalizeAltFormat(altFormat) == 4 ? "AUTO" : T("normal", "normal"));
        break;
      case 3:
        u8g2.print(T("Bitacora", "Logbook"));
        break;
      case 4:
        u8g2.print(T("Fecha/Hora", "Date/Time"));
        break;
      case 5: {
        extern bool inversionActiva;
        u8g2.print(T("Invertir: ", "Invert: "));
        u8g2.print(inversionActiva ? "ON" : "OFF");
        break;
      }
      case 6:
        u8g2.print(T("Ahorro: ", "Power save: "));
        if (ahorroTimeoutMs == 0) u8g2.print("OFF");
        else u8g2.print(String(ahorroTimeoutMs / 60000) + T(" min", " min"));
        break;
      case 7:
        u8g2.print("Offset: ");
        if (unidadMetros) { u8g2.print(alturaOffset, 2); u8g2.print(" m"); }
        else              { u8g2.print(alturaOffset * 3.281f, 0); u8g2.print(" ft"); }
        break;
      case 8:
        u8g2.print("Snake");
        break;
      case 9:
        u8g2.print(T("Idioma: ", "Language: "));
        u8g2.print((idioma == LANG_ES) ? "ES" : "EN");
        break;
      case 10:
        u8g2.print(T("Salir del menú", "Exit menu"));
        break;
    }
  }

  u8g2.setCursor(100, 63);
  u8g2.print(String(paginaActual + 1) + "/" + String(totalPaginas));
  u8g2.sendBuffer();
}

// ---------------------------------------------------------------------------
static void dibujarOffsetEdit() {
  u8g2.clearBuffer();

  // Título claro
  u8g2.setFont(u8g2_font_ncenB08_tr);
  u8g2.setCursor(5, 18);
  u8g2.print(T("Offset de altitud", "Altitude offset"));

  // Valor grande
  u8g2.setFont(u8g2_font_ncenB18_tr);
  u8g2.setCursor(5, 50);
  if (unidadMetros) { u8g2.print(offsetTemp, 2); u8g2.print(" m"); }
  else              { u8g2.print(offsetTemp * 3.281f, 0); u8g2.print(" ft"); }

  // Ayuda
  u8g2.setFont(u8g2_font_5x7_tf);
  u8g2.setCursor(5, 63);
  u8g2.print(T("OK + / ALT - | MENU Guarda | ALT+MENU Cancela | OK+ALT = 0",
               "OK + / ALT - | MENU Save   | ALT+MENU Cancel  | OK+ALT = 0"));

  u8g2.sendBuffer();
}

// ---------------------------------------------------------------------------
static void ejecutarOpcionMenu(int opcion) {
  switch(opcion) {
    case 0: // Unidad
      unidadMetros = !unidadMetros;
      break;

    case 1: // Brillo
      brilloPantalla += 50;
      if (brilloPantalla > 255) brilloPantalla = 50;
      u8g2.setContrast(brilloPantalla);
      break;

    case 2: { // Formato altitud: SOLO 0 <-> 4
      altFormat = normalizeAltFormat(altFormat);
      altFormat = (altFormat == 0) ? 4 : 0;
      } break;

    case 3: // Bitácora
      logbookUiOpen();
      break;

    case 4: // Fecha/hora
      datetimeMenuOpen();
      break;

    case 5: { // Invertir
      extern bool inversionActiva;
      inversionActiva = !inversionActiva;
      u8g2.sendF("c", inversionActiva ? 0xA7 : 0xA6);
      } break;

    case 6: // Ahorro
      ahorroTimeoutOption = (ahorroTimeoutOption + 1) % NUM_TIMEOUT_OPTIONS;
      ahorroTimeoutMs     = TIMEOUT_OPTIONS[ahorroTimeoutOption];
      break;

    case 7: // Editar offset
      if (!editingOffset) {
        editingOffset = true;
        offsetTemp    = alturaOffset;    // siempre en METROS
        dibujarOffsetEdit();
      }
      break;

    case 8: // Juego — lanzar modo “activo” y cerrar menú (NO llamar aquí la lógica)
      if (!gameSnakeRunning) {
        gameSnakeRunning = true;
        menuActivo = false;                              // cerramos el menú
        uiBlockMenuOpenUntilMs = millis() + 220;         // anti-rebote al entrar al juego
        // Reseteo de flags de menú
        s_firstFrameMenu = true;
        s_blockMenuSelectUntilMs = 0;
      }
      break;

    case 9: // Idioma
      idioma = (idioma == LANG_ES) ? LANG_EN : LANG_ES;
      break;

    case 10: { // Salir
      menuActivo = false;
      lastMenuInteraction = millis();
      uiBlockMenuOpenUntilMs = millis() + 300;
      // Reseteo de flags anti-flanco por cierre
      s_firstFrameMenu = true;
      s_blockMenuSelectUntilMs = 0;
      } break;
  }
  saveConfig();
}

// ---------------------------------------------------------------------------
void processMenu() {
  // Tick de botones
  btnTick(BTN_ALT);
  btnTick(BTN_OK);
  btnTick(BTN_MENU);

  // Primer frame tras abrir el menú: armar bloqueo corto e “ignorar flancos”
  if (s_firstFrameMenu) {
    s_blockMenuSelectUntilMs = millis() + 220;  // ~220 ms de gracia
    // "Primar" prevs para no ver un btnRise artificial al entrar
    BTN_ALT.prev  = BTN_ALT.down;
    BTN_OK.prev   = BTN_OK.down;
    BTN_MENU.prev = BTN_MENU.down;
    s_firstFrameMenu = false;
  }

  // Ignorar entradas los primeros ms al entrar al menú
  if (millis() < uiBlockMenuOpenUntilMs) {
    dibujarMenu();
    return;
  }

  // Submenús que toman control completo
  if (logbookUiIsActive()) { lastMenuInteraction = millis(); return; }
  if (datetimeMenuActive()) { lastMenuInteraction = millis(); return; }

  // ====== EDICIÓN DE OFFSET ======
  if (editingOffset) {
    bool changed = false;

    // Cancelar sin guardar: ALT + MENU simultáneos
    if (BTN_ALT.down && BTN_MENU.down) {
      editingOffset = false;
      lastMenuInteraction = millis();
      return;
    }

    // Reset rápido a 0: OK + ALT simultáneos
    if (BTN_OK.down && BTN_ALT.down) {
      offsetTemp = 0.0f;
      changed = true;
    }

    // Paso base por unidad (en METROS)
    float base_step_m = unidadMetros ? OFFSET_STEP_M : (OFFSET_STEP_FT * 0.3048f);
    // Paso efectivo con aceleración si hay pulsación larga en cualquiera de los dos
    bool isLong = btnLong(BTN_OK) || btnLong(BTN_ALT);
    float step_m = isLong ? (base_step_m * OFFSET_ACCEL) : base_step_m;

    // Incrementar/decrementar con su propio auto-repeat
    if (btnRise(BTN_OK) || btnRepeat(BTN_OK, 400, 120)) {
      offsetTemp += step_m;
      changed = true;
    }
    if (btnRise(BTN_ALT) || btnRepeat(BTN_ALT, 400, 120)) {
      offsetTemp -= step_m;
      changed = true;
    }

    if (changed) {
      // Límite duro
      if (offsetTemp < ALTURA_OFFSET_MIN_M) offsetTemp = ALTURA_OFFSET_MIN_M;
      if (offsetTemp > ALTURA_OFFSET_MAX_M) offsetTemp = ALTURA_OFFSET_MAX_M;

      // Snap a 0 usando medio paso BASE (no el acelerado) y una epsilon mínima
      float snap_eps = fmaxf(OFFSET_ZERO_EPS_M, base_step_m * 0.5f);
      if (fabsf(offsetTemp) < snap_eps) offsetTemp = 0.0f;

      lastMenuInteraction = millis();
      dibujarOffsetEdit();
    }

    // Guardar con MENU (comportamiento actual)
    if (btnRise(BTN_MENU)) {
      if (fabsf(offsetTemp) < OFFSET_ZERO_EPS_M) offsetTemp = 0.0f; // casi-cero -> 0
      alturaOffset = offsetTemp;     // SIEMPRE en METROS
      saveConfig();
      editingOffset = false;
      lastMenuInteraction = millis();
    }
    return;
  }

  // ====== NAVEGACIÓN DE MENÚ ======
  if (btnRise(BTN_MENU)) {
    // Si aún estamos en la ventana de gracia, no ejecutar la opción
    if ((int32_t)(millis() - s_blockMenuSelectUntilMs) < 0) {
      lastMenuInteraction = millis();  // evita auto-cierre por inactividad
      // No navegamos ni ejecutamos nada
      return;
    }
    ejecutarOpcionMenu(menuOpcion);
    lastMenuInteraction = millis();
    return;
  }

  if (btnRise(BTN_ALT) || btnRepeat(BTN_ALT, 500, 150)) {
    menuOpcion = (menuOpcion + 1) % TOTAL_OPCIONES;
    lastMenuInteraction = millis();
  }
  if (btnRise(BTN_OK) || btnRepeat(BTN_OK, 500, 150)) {
    menuOpcion = (menuOpcion - 1 + TOTAL_OPCIONES) % TOTAL_OPCIONES;
    lastMenuInteraction = millis();
  }

  if (millis() - lastMenuInteraction > 4000) {
    menuActivo = false;
    // Reseteo de flags anti-flanco por cierre automático
    s_firstFrameMenu = true;
    s_blockMenuSelectUntilMs = 0;
  }
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

  if (mustRestore) {
    if (ahorroDimmed) { u8g2.setContrast(brilloPantalla); }
    ahorroDimmed = false;
    return;
  }

  if (!ahorroDimmed && (now - lastAct >= INACTIVITY_DIM_MS)) {
    u8g2.setContrast(AHORRO_DIM_CONTRAST);
    ahorroDimmed = true;
    return;
  }

  if (ahorroDimmed && (now - lastAct) <= 1000UL) {
    u8g2.setContrast(brilloPantalla);
    ahorroDimmed = false;
  }
}

// ---------------------------------------------------------------------------
void updateUI() {
  if (!startupDone) { mostrarCuentaRegresiva(); return; }

  // === Snake activo: dibujar/avanzar el juego y salir ===
  if (gameSnakeRunning) {
    playSnakeGame();     // Llamada por frame (no bloqueante)
    return;
  }

  if (!pantallaEncendida) return;

  handleAhorroAutoDim();

  if (!menuActivo) {
    // Throttling de repintado por modo
    static uint32_t t_last_ui = 0;
    uint16_t ui_interval = 140;
    SensorMode m = getSensorMode();
    if (m == SENSOR_MODE_ULTRA_PRECISO) ui_interval = 100;
    if (m == SENSOR_MODE_FREEFALL)     ui_interval = 80;
    uint32_t now_ui = millis();
    if (now_ui - t_last_ui < ui_interval) return;
    t_last_ui = now_ui;

    // -------- Pantalla principal / HUD --------
    u8g2.clearBuffer();

    // Unidad (M o FT)
    u8g2.setFont(u8g2_font_ncenB08_tr);
    u8g2.setCursor(2, 12);
    u8g2.print(unidadMetros ? "M" : "FT");

    // Hora local
    {
      char hhmm[6];
      datetimeFormatHHMM(hhmm, sizeof(hhmm));
      int w = u8g2.getStrWidth(hhmm);
      int x = (128 - w)/2; if (x < 0) x = 0;
      u8g2.setCursor(x, 12);
      u8g2.print(hhmm);
    }

    // Luna parpadeante si DS inminente (oculta si hay USB)...

// --- Indicador de suspensión o, si no aplica, temperatura ---
    bool mostreSuspIcono = false;

    if (ahorroTimeoutMs > 0 && !powerLockActive() && getSensorMode() == SENSOR_MODE_AHORRO && !isUsbPresent()) {
      unsigned long now     = millis();
      unsigned long lastAct = getLastActivityMs();
      long msLeft           = (long)ahorroTimeoutMs - (long)(now - lastAct);
      if (msLeft > 0 && msLeft <= 120000L) {
        bool blinkOn = ((now / 500UL) % 2UL) == 0UL;
        if (blinkOn) {
          u8g2.setFont(u8g2_font_open_iconic_weather_1x_t);
          u8g2.drawGlyph(18, 12, 66);     // luna
          u8g2.setFont(u8g2_font_5x8_mf);
          u8g2.drawStr(27, 10, "zzz");    // zzz
          mostreSuspIcono = true;
        }
      }
    }

    if (!mostreSuspIcono) {
      char tbuf[8];
      float tC = bmp.temperature;
      snprintf(tbuf, sizeof(tbuf), "%.0f°C", tC);   // "24°C"

      u8g2.setFont(u8g2_font_6x10_tf);
      u8g2.drawUTF8(23, 12, tbuf);                  // mismo lugar que luna+zzz
    }


    // Batería %
    {
      int pct = batteryGetPercent();
      bool blink = batteryIsLowPercent();
      bool showNow = true;
      if (blink) showNow = ((millis() / 500UL) % 2UL) == 0UL;
      if (showNow) {
        u8g2.setFont(u8g2_font_ncenB08_tr);
        String batStr = String(pct) + "%";
        int batWidth = u8g2.getStrWidth(batStr.c_str());
        u8g2.setCursor(128 - batWidth - 2, 12);
        u8g2.print(batStr);
      }
    }

    // === Indicador de CARGA (espacio reservado) ===
    if (isUsbPresent()) {
      u8g2.setFont(u8g2_font_open_iconic_other_1x_t);
      u8g2.drawGlyph(90, 12, 64);
    }

    float altRel_m = altCalculada;                     // ya incluye offset + AGZ
    float rel_from_offset_m = altRel_m - alturaOffset; // distancia al offset (m)

    float altToShow; // valor final en la unidad visible

    if (unidadMetros) {
      // Si estamos dentro del umbral (±UI_DEADBAND_M) alrededor del offset,
      // mostrar EXACTAMENTE el offset (en m). Si no, mostrar la altitud real.
      if (fabsf(rel_from_offset_m) < UI_DEADBAND_M) {
        altToShow = alturaOffset;                     // pegado al offset (m)
      } else {
        altToShow = altRel_m;
      }
    } else {
      // Mismo criterio pero en pies
      float rel_from_offset_ft = rel_from_offset_m * 3.281f;
      if (fabsf(rel_from_offset_ft) < UI_DEADBAND_FT) {
        altToShow = alturaOffset * 3.281f;           // pegado al offset (ft)
      } else {
        altToShow = altRel_m * 3.281f;
      }
    }

    String altDisplay;
    int fmt = normalizeAltFormat(altFormat);
    if (fmt == 4) {
      float absAlt = fabsf(altToShow);
      if (absAlt < 999.0f) { long v = lroundf(altToShow); altDisplay = String(v); }
      else if (absAlt < 9999.0f) { float vDisp = roundf((altToShow/1000.0f) * 100.0f) / 100.0f; altDisplay = String(vDisp, 2); }
      else { float vDisp = roundf((altToShow/1000.0f) * 10.0f) / 10.0f; altDisplay = String(vDisp, 1); }
    } else {
      altDisplay = String((long)altToShow);
    }

    u8g2.setFont(u8g2_font_fub30_tr);
    int xPosAlt = (128 - u8g2.getStrWidth(altDisplay.c_str())) / 2;
    if (xPosAlt < 0) xPosAlt = 0;
    u8g2.setCursor(xPosAlt, 50);
    u8g2.print(altDisplay);

    // Bordes
    u8g2.drawHLine(0, 15, 128); 
    u8g2.drawHLine(0, 52, 128); 
    u8g2.drawHLine(0, 0, 128); 
    u8g2.drawHLine(0, 63, 128); 
    u8g2.drawVLine(0, 0, 64); 
    u8g2.drawVLine(127, 0, 64);

    // Usuario
    u8g2.setFont(u8g2_font_ncenB08_tr);
    String user = usuarioActual;
    int xPosUser = (128 - u8g2.getStrWidth(user.c_str())) / 2;
    if (xPosUser < 0) xPosUser = 0;
    u8g2.setCursor(xPosUser, 62);
    u8g2.print(user);

    // Total saltos
    uint32_t lifetime = 0; logbookGetTotal(lifetime);
    String jumpStr = String(lifetime);
    u8g2.setCursor(128 - u8g2.getStrWidth(jumpStr.c_str()) - 14, 62);
    u8g2.print(jumpStr);

    // Indicadores
    if (jumpArmed || inJump) {
      if (inJump) u8g2.drawDisc(14, 58, 3);
      else        u8g2.drawCircle(14, 58, 3);
    }
    if (powerLockActive()) {
      u8g2.setFont(u8g2_font_open_iconic_thing_1x_t);
      u8g2.drawGlyph(26, 63, 79);
      alarmOnLockAltitude();
    }

    u8g2.sendBuffer();

  } else {
    // ======= Menú / Submenús =======
    if (datetimeMenuActive()) { datetimeMenuDrawAndHandle(); return; }
    if (editingOffset) { dibujarOffsetEdit(); return; }

    if (batteryMenuActive) {
      if (pantallaEncendida) {
        float vbat = batteryGetVoltage();
        int   pct  = batteryGetPercent();

        u8g2.clearBuffer();
        u8g2.setFont(u8g2_font_ncenB08_tr);
        u8g2.setCursor(0,12);
        u8g2.print(T("BATERIA:", "BATTERY:"));

        u8g2.setCursor(0,28);
        u8g2.print("V_Bat: ");
        u8g2.setCursor(50,28);
        u8g2.print(vbat, 2);
        u8g2.print("V");

        u8g2.setCursor(0,44);
        u8g2.print(T("Carga: ", "Charge: "));
        u8g2.setCursor(50,44);
        u8g2.print(pct);
        u8g2.print("%");

        u8g2.sendBuffer();

        btnTick(BTN_OK);
        if (btnRise(BTN_OK)) {
          batteryMenuActive = false;
          lastMenuInteraction = millis();
        }
      }
      return;
    }

    if (logbookUiIsActive()) {
      logbookUiDrawAndHandle(u8g2);
      return;
    }

    dibujarMenu();
  }
}
