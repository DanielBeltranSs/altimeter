#include <Arduino.h>
#include <U8g2lib.h>
#include "config.h"
#include "logbook.h"
#include "logbookUi.h"
#include "datetime_module.h"   // para formatear ts_local

//Aceleracion para logbook (variables globales originales, se mantienen aunque no se usen aquí)
static uint16_t s_step = 1;           // tamaño de paso actual
static uint32_t s_holdStartMs = 0;
static const uint16_t STEP_MAX = 512; // tope de aceleración
static const uint16_t STEP_MIN = 1;
static const uint16_t STEP_HOLD_MS = 250; // cada 250 ms duplica

// --- Estado interno del submenú ---
static bool     s_active   = false;
static uint16_t s_count    = 0;   // Nº de saltos guardados (ring)
static int      s_idx      = 0;   // 0 = más reciente

// --- Confirmación de borrado (ALT+OLED 2s) ---
static bool     s_erasePrompt   = false;
static uint32_t s_comboStartMs  = 0;
static uint32_t s_comboStartMs2 = 0;
static constexpr uint32_t ERASE_HOLD_MS = 2000;

// --- Estado de "toast" no bloqueante ---
static bool     s_toastActive   = false;
static uint32_t s_toastUntilMs  = 0;
static char     s_toastMsg[48]  = {0};
static constexpr uint32_t TOAST_MS = 900;

// --- Anti-flanco al entrar (NUEVO) ---
static uint32_t s_blockInputsUntilMs = 0;   // ventana para ignorar entradas
static bool     s_primePrevOnFirst   = false; // primar prevs en el primer frame

// --- Dependencias externas ---
extern bool   unidadMetros;          // config.cpp
extern int    idioma;                // LANG_ES / LANG_EN (config.cpp)
extern long   lastMenuInteraction;   // ui_module.cpp

// Helper simple de idioma
static inline const char* T(const char* es, const char* en) {
  return (idioma == LANG_ES) ? es : en;
}

static inline uint8_t decAlt() { return unidadMetros ? 1 : 0; }
// Activo-alto: pulsado = HIGH (pulldown)
static inline bool btnHigh(int pin) { return digitalRead(pin) == HIGH; }

// ------- Dibujo de una entrada -------
static void drawEntry(U8G2 &u8g2, const JumpLog& jl, int idx, int total) {
  (void)total;
  u8g2.clearBuffer();

  // Marco
  u8g2.drawHLine(0, 0, 128);
  u8g2.drawHLine(0, 13, 128);
  u8g2.drawHLine(0, 63, 128);
  u8g2.drawVLine(0, 0, 64);
  u8g2.drawVLine(127, 0, 64);

  // ===========================
  // Encabezado: "Jump: x HH:MM DD/MM/YY"
  // ===========================
  u8g2.setFont(u8g2_font_5x8_mf);

  char hhmm[6];      // "HH:MM"
  char dmy[9];       // "DD/MM/YY"
  datetimeFormatEpoch_HHMM((uint32_t)jl.ts_local, hhmm, sizeof(hhmm));
  datetimeFormatEpoch_DDMMYY((uint32_t)jl.ts_local, dmy, sizeof(dmy));

  char hdr[48];
  snprintf(hdr, sizeof(hdr), "Jump: %lu %s %s",
           (unsigned long)jl.jump_id, hhmm, dmy);
  u8g2.setCursor(2, 10);
  u8g2.print(hdr);

  // Campos (alturas y tiempos)
  String sExit   = logbookFormatAltCm(jl.exit_alt_cm,   unidadMetros, decAlt());
  String sDeploy = logbookFormatAltCm(jl.deploy_alt_cm, unidadMetros, decAlt());
  String sFF     = logbookFormatFF(jl.freefall_time_ds);

  // >>> Velocidades en km/h
  String sVff  = logbookFormatVelKmh(jl.vmax_ff_cmps,  1);
  String sVcan = logbookFormatVelKmh(jl.vmax_can_cmps, 1);

  // Exit / Open
  u8g2.setCursor(2, 22);  u8g2.print(T("Exit:", "Exit:"));
  u8g2.setCursor(30, 22); u8g2.print(sExit);

  u8g2.setCursor(2, 32);  u8g2.print(T("Open:", "Open:"));
  u8g2.setCursor(30, 32); u8g2.print(sDeploy);

  // Freefall (tiempo) y V (freefall en km/h)
  u8g2.setCursor(2, 42);  u8g2.print("FF:");
  u8g2.setCursor(30, 42); u8g2.print(sFF);

  u8g2.setCursor(2, 52);  u8g2.print("V:");
  u8g2.setCursor(30, 52); u8g2.print(sVff);

  // ===========================
  // Fila inferior: "Vc:" y el id del salto
  // ===========================
  u8g2.setCursor(2, 62);   u8g2.print("Vc:");
  u8g2.setCursor(30, 62);  u8g2.print(sVcan);

  // Footer: solo número de salto <jump_id>
  u8g2.setFont(u8g2_font_5x7_mf);
  char idbuf[20];
  if (jl.jump_id) snprintf(idbuf, sizeof(idbuf), "<%lu>", (unsigned long)jl.jump_id);
  else            snprintf(idbuf, sizeof(idbuf), "<%d>", idx + 1);
  int idW = u8g2.getStrWidth(idbuf);
  int idX = 128 - idW - 2; if (idX < 0) idX = 0;
  u8g2.setCursor(idX, 62);
  u8g2.print(idbuf);

  u8g2.sendBuffer();
}

static void drawEmpty(U8G2 &u8g2) {
  u8g2.clearBuffer();
  u8g2.setFont(u8g2_font_ncenB08_tr);
  u8g2.setCursor(10, 28); u8g2.print(T("Sin registros", "No entries"));
  u8g2.setCursor(10, 46); u8g2.print(T("MENU para salir", "MENU to exit"));
  u8g2.sendBuffer();
}

static void drawErasePrompt(U8G2 &u8g2, bool confirmStage) {
  (void)confirmStage;
  u8g2.clearBuffer();
  u8g2.setFont(u8g2_font_profont12_mf);
  u8g2.setCursor(0, 16); u8g2.print(T("Borrar Bitacora", "Erase Logbook"));
  u8g2.setCursor(0, 32); u8g2.print(T("Mantener ALT+OLED", "Hold ALT+OLED"));
  u8g2.setCursor(0, 44); u8g2.print(T("2s para CONFIRMAR", "2s to CONFIRM"));
  u8g2.setCursor(0, 60); u8g2.print(T("MENU para cancelar", "MENU to cancel"));
  u8g2.sendBuffer();
}

// Reemplazo NO bloqueante del antiguo delay(900)
static void drawToast(U8G2 &u8g2, const char* msg) {
  (void)u8g2;
  strncpy(s_toastMsg, msg, sizeof(s_toastMsg)-1);
  s_toastMsg[sizeof(s_toastMsg)-1] = '\0';
  s_toastActive  = true;
  s_toastUntilMs = millis() + TOAST_MS;
}

// ------- API -------
void logbookUiOpen() {
  logbookGetCount(s_count);
  s_idx    = 0;          // último salto
  s_active = true;
  s_erasePrompt = false;
  s_comboStartMs = 0;
  s_comboStartMs2 = 0;
  s_toastActive = false;
  s_toastUntilMs = 0;
  s_toastMsg[0] = '\0';
  lastMenuInteraction = millis(); // evita cierre por inactividad

  // >>> NUEVO: ventana anti-rebote y primado de prevs <<<
  s_blockInputsUntilMs = millis() + 220;
  s_primePrevOnFirst   = true;
}

bool logbookUiIsActive() { return s_active; }

void logbookUiDrawAndHandle(U8G2 &u8g2) {
  if (!s_active) return;
  lastMenuInteraction = millis();

  // Lectura actual de botones (ACTIVO-ALTO)
  const bool altDown  = (digitalRead(BUTTON_ALTITUDE) == HIGH);
  const bool oledDown = (digitalRead(BUTTON_OLED)     == HIGH);
  const bool menuDown = (digitalRead(BUTTON_MENU)     == HIGH);

  // Estados prev persistentes
  static bool altPrev=false, oledPrev=false, menuPrev=false;

  // >>> NUEVO: primer frame tras abrir — “primar” prevs para no ver flancos
  if (s_primePrevOnFirst) {
    altPrev = altDown;
    oledPrev = oledDown;
    menuPrev = menuDown;
    s_primePrevOnFirst = false;
  }

  // Si estamos en ventana de bloqueo, solo dibujar y salir sin procesar entradas
  if ((int32_t)(millis() - s_blockInputsUntilMs) < 0) {
    if (s_count == 0) {
      drawEmpty(u8g2);
    } else {
      JumpLog jl{};
      if (logbookGetByIndex((uint16_t)s_idx, jl)) drawEntry(u8g2, jl, s_idx, s_count);
      else drawEmpty(u8g2);
    }
    return;
  }

  // Si hay un TOAST activo, mostrarlo y no procesar entradas hasta que expire
  if (s_toastActive) {
    u8g2.clearBuffer();
    u8g2.setFont(u8g2_font_profont12_mf);
    int w = u8g2.getStrWidth(s_toastMsg);
    int x = (128 - w) / 2; if (x < 0) x = 0;
    u8g2.setCursor(x, 36);
    u8g2.print(s_toastMsg);
    u8g2.sendBuffer();

    if ((int32_t)(millis() - s_toastUntilMs) >= 0) {
      s_toastActive = false;
    }
    return;
  }

  // Flancos crudos (ANTES de actualizar prev)
  bool altRiseRaw  = (altDown  && !altPrev);
  bool oledRiseRaw = (oledDown && !oledPrev);
  bool menuRiseRaw = (menuDown && !menuPrev);

  // Debounce de flancos
  static const uint16_t EDGE_DEBOUNCE_MS = 40;
  static uint32_t lastAltEdgeMs=0, lastOledEdgeMs=0, lastMenuEdgeMs=0;
  uint32_t now = millis();
  const bool altRise  = altRiseRaw  && (now - lastAltEdgeMs  > EDGE_DEBOUNCE_MS);
  const bool oledRise = oledRiseRaw && (now - lastOledEdgeMs > EDGE_DEBOUNCE_MS);
  const bool menuRise = menuRiseRaw && (now - lastMenuEdgeMs > EDGE_DEBOUNCE_MS);
  if (altRise)  lastAltEdgeMs  = now;
  if (oledRise) lastOledEdgeMs = now;
  if (menuRise) lastMenuEdgeMs = now;

  // AHORA sí, actualizar prev
  altPrev  = altDown;
  oledPrev = oledDown;
  menuPrev = menuDown;

  // Salida diferida hasta soltar MENU
  static bool s_pendingExit = false;

  if (s_pendingExit && !menuDown) {
    s_pendingExit = false;
    s_erasePrompt = false;
    s_active = false;      // salir con botón ya suelto
    return;
  }

  if (menuRise) {
    s_pendingExit = true;
  }

  // Si hay salida pendiente, dibuja frame actual y espera
  if (s_pendingExit) {
    if (s_count == 0) {
      drawEmpty(u8g2);
    } else {
      JumpLog jl{};
      if (logbookGetByIndex((uint16_t)s_idx, jl)) drawEntry(u8g2, jl, s_idx, s_count);
      else drawEmpty(u8g2);
    }
    return;
  }

  // Vista normal o prompt de borrado
  if (!s_erasePrompt) {
    // Dibujo
    if (s_count == 0) {
      drawEmpty(u8g2);
    } else {
      JumpLog jl{};
      if (logbookGetByIndex((uint16_t)s_idx, jl)) drawEntry(u8g2, jl, s_idx, s_count);
      else { if (s_count > 0) s_idx = (s_idx + 1) % s_count; drawEmpty(u8g2); }
    }

    // Combo ALT+OLED (oculto) para borrar todo
    static uint32_t comboStartMs = 0;
    if (altDown && oledDown) {
      if (comboStartMs == 0) comboStartMs = millis();
      if (millis() - comboStartMs >= ERASE_HOLD_MS) {
        s_erasePrompt  = true;
        comboStartMs   = 0;
      }
    } else {
      comboStartMs = 0;
    }

    // -------------------------------
    // Navegación: TAP + HOLD ACELERADO con umbral (circular)
    // -------------------------------
    // Taps (paso 1)
    if (altRise)  { if (s_count > 0) s_idx = (s_idx + 1) % s_count; }                         // hacia más antiguo
    if (oledRise) { if (s_count > 0) s_idx = (s_idx + s_count - 1) % s_count; }               // hacia más reciente

    // Hold acelerado (exponencial)
    static uint32_t s_holdStartMs_l = 0;
    static uint16_t s_step_l = 1;
    static uint32_t s_lastRepeatMs = 0;
    static uint32_t s_accelAnchorMs = 0;
    static const uint16_t STEP_MAX_L = 512;
    static const uint16_t STEP_MIN_L = 1;
    static const uint16_t STEP_HOLD_MS_L = 250;
    static const uint16_t REPEAT_MS = 60;
    static const uint16_t HOLD_REPEAT_DELAY_MS = 350;

    const bool anyDown = (altDown || oledDown);
    const bool combo   = (altDown && oledDown);

    if (combo) {
      s_holdStartMs_l = 0;
      s_step_l = STEP_MIN_L;
      s_lastRepeatMs = 0;
      s_accelAnchorMs = 0;
    } else if (anyDown && s_count > 0) {
      uint32_t t = millis();
      if (s_holdStartMs_l == 0) {
        s_holdStartMs_l = t;
        s_step_l = STEP_MIN_L;
        s_lastRepeatMs = t;
        s_accelAnchorMs = 0;
      }

      if (t - s_holdStartMs_l >= HOLD_REPEAT_DELAY_MS) {
        if (s_accelAnchorMs == 0) s_accelAnchorMs = t;
        if (t - s_accelAnchorMs >= STEP_HOLD_MS_L && s_step_l < STEP_MAX_L) {
          s_step_l <<= 1;
          if (s_step_l > STEP_MAX_L) s_step_l = STEP_MAX_L;
          s_accelAnchorMs = t;
        }
        if (t - s_lastRepeatMs >= REPEAT_MS) {
          uint16_t step = s_step_l;
          if (oledDown) {
            s_idx = (s_idx + s_count - (step % s_count)) % s_count;   // hacia más reciente
          } else if (altDown) {
            s_idx = (s_idx + step) % s_count;                         // hacia más antiguo
          }
          s_lastRepeatMs = t;
        }
      }
    } else {
      s_holdStartMs_l = 0;
      s_step_l = STEP_MIN_L;
      s_lastRepeatMs = 0;
      s_accelAnchorMs = 0;
    }
  } else {
    // Prompt de confirmación
    drawErasePrompt(u8g2, true);

    static uint32_t confirmStartMs = 0;
    if (altDown && oledDown) {
      if (confirmStartMs == 0) confirmStartMs = millis();
      if (millis() - confirmStartMs >= ERASE_HOLD_MS) {
        logbookResetAll();
        s_erasePrompt   = false;
        confirmStartMs  = 0;
        logbookGetCount(s_count);
        s_idx = 0;
        drawToast(u8g2, T("Bitacora borrada", "Logbook erased")); // no bloquea
      }
    } else {
      confirmStartMs = 0;
    }
  }
}
