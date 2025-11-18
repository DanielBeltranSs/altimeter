#include "ui_module.h"
#include "config.h"
#include "ble_module.h"  // toggleBLE()
#include <Arduino.h>
#include <U8g2lib.h>
#include "sensor_module.h"
#include <math.h>        // fabsf(), powf
#include "buzzer_module.h"
#include "snake.h"
#include "power_lock.h"
#include "battery.h"     // <<< Nuevo: fuente única de voltaje/% batería

// ===== Idioma =====
extern int idioma; // LANG_ES / LANG_EN
static String T(const char* es, const char* en) {
  return (idioma == LANG_ES) ? String(es) : String(en);
}

// --- Helper: normalizar altFormat para que solo existan 0 (normal) o 4 (AUTO) ---
static inline int normalizeAltFormat(int v) {
  return (v == 4) ? 4 : 0;
}

// Se instancia el objeto de la pantalla
U8G2_SSD1306_128X64_NONAME_F_HW_I2C u8g2(U8G2_R0, U8X8_PIN_NONE);
bool startupDone = false;

// Constantes para el menú
const int TOTAL_OPCIONES      = 10;  // incluye Idioma
const int OPCIONES_POR_PAGINA = 4;

bool gameSnakeRunning = false;

// Variables para el modo edición de offset
bool  editingOffset = false;
float offsetTemp    = 0.0f;  // Valor temporal para el offset mientras se edita

// Variables globales para el menú
int  menuOpcion        = 0;
bool menuActivo        = false;
bool batteryMenuActive = false; // Para el menú de batería

// Declaración de variables externas de configuración (definidas en config.cpp)
extern bool  unidadMetros;
extern int   brilloPantalla;
extern int   altFormat;       // AHORA: solo 0=normal o 4=AUTO
extern bool  bleActivo;
extern unsigned long ahorroTimeoutMs;
extern int   ahorroTimeoutOption;
extern float alturaOffset;     // Offset de altitud en metros
extern String usuarioActual;

// Variables provenientes del módulo de sensor / UI
extern uint32_t   jumpCount;
extern bool       pantallaEncendida;   // Se mantiene por compatibilidad; en deep sleep el MCU se detiene.
extern float      altCalculada;        // Altitud relativa en metros (ya con offset), del sensor
extern bool       jumpArmed;           // armado (contorno)
extern bool       inJump;              // en salto (relleno)

// Variable para el timeout del menú
long lastMenuInteraction = 0;

// Getter que añadimos en main.cpp
unsigned long getLastActivityMs();

// ---------------------------------------------------------------------------
// AUTO (según unidad visible):
//   |alt| <   999   -> 0 dec (sin escalar)
//   |alt| <  9999   -> 2 dec (alt/1000)
//   |alt| >= 10000  -> 1 dec (alt/1000)
//   (sin histéresis, cortes exactos solicitados)
// ---------------------------------------------------------------------------
static int s_autoFmt  = 0;  // 0/1/2 decimales
static int s_autoBand = 0;  // 0:<999, 1:999..9999, 2:>=10000

static void autoUpdateFormat(float altRel_m) {
  float alt = unidadMetros ? fabsf(altRel_m) : fabsf(altRel_m * 3.281f);
  int newBand;
  if (alt < 999.0f)      newBand = 0;
  else if (alt < 9999.0f) newBand = 1;
  else                    newBand = 2;

  s_autoBand = newBand;
  switch (s_autoBand) {
    case 0: s_autoFmt = 0; break; // < 999    → 0 dec
    case 1: s_autoFmt = 2; break; // < 9999   → 2 dec
    case 2: s_autoFmt = 1; break; // ≥ 10000  → 1 dec
    default: s_autoFmt = 0; break;
  }
}


// ---------------------------------------------------------------------------
// Función para inicializar la UI
// ---------------------------------------------------------------------------
void initUI() {
  // Normaliza altFormat por si NVS tenía 1/2/3
  altFormat = normalizeAltFormat(altFormat);

  u8g2.begin();
  u8g2.setPowerSave(false);         // Mantener activo durante operación normal
  u8g2.setContrast(brilloPantalla);
  extern bool inversionActiva;
  if (inversionActiva) {
    u8g2.sendF("c", 0xA7);
    Serial.println(T("Display iniciado en modo invertido.", "Display started in inverted mode."));
  } else {
    u8g2.sendF("c", 0xA6);
    Serial.println(T("Display iniciado en modo normal.", "Display started in normal mode."));
  }
}

// ---------------------------------------------------------------------------
// Función para mostrar la cuenta regresiva de startup
// ---------------------------------------------------------------------------
void mostrarCuentaRegresiva() {
  static unsigned long startupStartTime = 0;
  if (startupStartTime == 0) startupStartTime = millis();
  unsigned long elapsed = millis() - startupStartTime;
  int secondsLeft = 3 - (elapsed / 1000);
  if (secondsLeft < 0) secondsLeft = 0;

  u8g2.clearBuffer();
  u8g2.setFont(u8g2_font_fub30_tr);
  String cuenta = String(secondsLeft);
  int xPos = (128 - u8g2.getStrWidth(cuenta.c_str())) / 2;
  u8g2.setCursor(xPos, 40);
  u8g2.print(cuenta);

  u8g2.setFont(u8g2_font_ncenB08_tr);
  String ini = T("Iniciando...", "Starting...");
  int w = u8g2.getStrWidth(ini.c_str());
  int x = (128 - w) / 2;
  u8g2.setCursor(x, 60);
  u8g2.print(ini);
  u8g2.sendBuffer();

  if (elapsed >= 3000) startupDone = true;

  // Beep de 1 segundo al finalizar la configuración inicial
  buzzerBeep(2000, 240, 1000);
  delay(100);
}

// ---------------------------------------------------------------------------
// Función para dibujar el menú principal (no en edición)
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
        // Solo 2 opciones: normal (0) y AUTO (4)
        if (normalizeAltFormat(altFormat) == 4) {
          u8g2.print("AUTO");
        } else {
          u8g2.print(T("normal", "normal"));
        }
        break;
      case 3:
        u8g2.print(T("Bateria", "Battery"));
        break;
      case 4:
        u8g2.print("BL: ");
        u8g2.print(bleActivo ? "ON" : "OFF");
        break;
      case 5: {
        u8g2.print(T("Invertir: ", "Invert: "));
        extern bool inversionActiva;
        u8g2.print(inversionActiva ? "ON" : "OFF");
        break;
      }
      case 6:
        u8g2.print(T("Ahorro: ", "Power save: "));
        if (ahorroTimeoutMs == 0)
          u8g2.print("OFF");
        else
          u8g2.print(String(ahorroTimeoutMs / 60000) + T(" min", " min"));
        break;
      case 7:
        u8g2.print("Offset: ");
        if (unidadMetros) {
          u8g2.print(alturaOffset, 2);
          u8g2.print(" m");
        } else {
          u8g2.print(alturaOffset * 3.281f, 0);
          u8g2.print(" ft");
        }
        break;
      case 8:
        u8g2.print("Snake");
        break;
      case 9: // Idioma
        u8g2.print(T("Idioma: ", "Language: "));
        u8g2.print((idioma == LANG_ES) ? "ES" : "EN");
        break;
    }
  }

  u8g2.setCursor(100, 63);
  u8g2.print(String(paginaActual + 1) + "/" + String(totalPaginas));
  if (paginaActual > 0) {
    u8g2.setCursor(90, 63);
    u8g2.print("<");
  }
  if (paginaActual < totalPaginas - 1) {
    u8g2.setCursor(120, 63);
    u8g2.print(">");
  }
  u8g2.sendBuffer();
}

// ---------------------------------------------------------------------------
// Función que dibuja la pantalla de edición del offset
// ---------------------------------------------------------------------------
void dibujarOffsetEdit() {
  u8g2.clearBuffer();
  u8g2.setFont(u8g2_font_ncenB08_tr);
  u8g2.setCursor(5, 20);
  u8g2.print(T("Editar Altura", "Edit Altitude"));
  u8g2.setCursor(5, 50);
  u8g2.setFont(u8g2_font_ncenB18_tr);
  if (unidadMetros) {
    u8g2.print(offsetTemp, 2);
    u8g2.print(" m");
  } else {
    u8g2.print(offsetTemp * 3.281f, 0);
    u8g2.print(" ft");
  }
  u8g2.sendBuffer();
}

// ---------------------------------------------------------------------------
// Función que ejecuta la acción correspondiente a la opción del menú
// ---------------------------------------------------------------------------
static void ejecutarOpcionMenu(int opcion) {
  switch(opcion) {
    case 0: // Cambiar unidad
      unidadMetros = !unidadMetros;
      Serial.print(T("Unidad cambiada a: ", "Units changed to: "));
      Serial.println(unidadMetros ? T("metros", "meters") : T("pies", "feet"));
      break;

    case 1: // Cambiar brillo
      brilloPantalla += 50;
      if (brilloPantalla > 255) brilloPantalla = 50;
      u8g2.setContrast(brilloPantalla);
      Serial.print(T("Brillo cambiado a: ", "Brightness changed to: "));
      Serial.println(brilloPantalla);
      break;

    case 2: { // Cambiar formato de altitud: SOLO 0 <-> 4
      altFormat = normalizeAltFormat(altFormat);
      altFormat = (altFormat == 0) ? 4 : 0;  // toggle entre normal y AUTO
      Serial.print(T("Formato de altitud cambiado a: ", "Altitude format: "));
      if (altFormat == 4) Serial.println("AUTO");
      else                Serial.println(T("normal", "normal"));
      } break;

    case 3: // Activar/desactivar vista de batería
      batteryMenuActive = !batteryMenuActive;
      Serial.print(T("Menú de batería ahora: ", "Battery menu now: "));
      Serial.println(batteryMenuActive ? "ON" : "OFF");
      break;

    case 4: // Alternar BLE
      toggleBLE();
      Serial.print("BLE ");
      Serial.println(bleActivo ? "ON" : "OFF");
      break;

    case 5: { // Cambiar modo de inversión
      extern bool inversionActiva;
      inversionActiva = !inversionActiva;
      if (inversionActiva) {
        u8g2.sendF("c", 0xA7);
        Serial.println(T("Display invertido.", "Display inverted."));
      } else {
        u8g2.sendF("c", 0xA6);
        Serial.println(T("Display en modo normal.", "Display normal."));
      }
      } break;

    case 6: // Cambiar modo ahorro (afecta deep sleep en main.cpp)
      ahorroTimeoutOption = (ahorroTimeoutOption + 1) % NUM_TIMEOUT_OPTIONS;
      ahorroTimeoutMs = TIMEOUT_OPTIONS[ahorroTimeoutOption];
      Serial.print(T("Modo ahorro configurado a: ", "Power save set to: "));
      if (ahorroTimeoutMs == 0) Serial.println("OFF");
      else                      Serial.println(String(ahorroTimeoutMs / 60000) + " min");
      break;

    case 7: // Opción de offset de altitud
      if (!editingOffset) {
        editingOffset = true;
        offsetTemp = alturaOffset;  // valor actual
        Serial.println(T("Modo edición de offset iniciado.", "Offset edit mode started."));
        dibujarOffsetEdit();
      }
      break;

    case 8:
      gameSnakeRunning = true;
      playSnakeGame();              // Durante el juego, main.cpp bloquea deep sleep
      gameSnakeRunning = false;
      Serial.println(T("Juego Snake finalizado.", "Snake game finished."));
      break;

    case 9: // Idioma
      idioma = (idioma == LANG_ES) ? LANG_EN : LANG_ES;
      Serial.print(T("Idioma cambiado a: ", "Language changed to: "));
      Serial.println((idioma == LANG_ES) ? "Español" : "English");
      break;
  }
  saveConfig();
}

// ---------------------------------------------------------------------------
// Función para procesar la navegación del menú
// ---------------------------------------------------------------------------
void processMenu() {
  // Si estamos en modo de edición del offset, procesamos los botones para incrementar/decrementar
  if (editingOffset) {
    bool updated = false;
    // Incrementar el offset con BUTTON_MENU
    if (digitalRead(BUTTON_MENU) == LOW) {
      delay(50);  // debounce
      if (unidadMetros) offsetTemp += 0.1f;
      else              offsetTemp += 10.0f / 3.281f; // 10 ft en metros
      updated = true;
      Serial.print(T("Offset temporal incrementado: ", "Temp offset increased: "));
      Serial.println(offsetTemp);
      while(digitalRead(BUTTON_MENU) == LOW) { delay(10); }
    }
    // Decrementar el offset con BUTTON_ALTITUDE
    if (digitalRead(BUTTON_ALTITUDE) == LOW) {
      delay(50);
      if (unidadMetros) offsetTemp -= 0.1f;
      else              offsetTemp -= 10.0f / 3.281f;
      updated = true;
      Serial.print(T("Offset temporal decrementado: ", "Temp offset decreased: "));
      Serial.println(offsetTemp);
      while(digitalRead(BUTTON_ALTITUDE) == LOW) { delay(10); }
    }
    // Si hubo un cambio, actualizar la pantalla de edición
    if (updated) dibujarOffsetEdit();

    // Confirmar la edición con BUTTON_OLED
    if (digitalRead(BUTTON_OLED) == LOW) {
      delay(50);
      alturaOffset = offsetTemp;
      saveConfig();
      editingOffset = false;
      Serial.print(T("Offset confirmado: ", "Offset confirmed: "));
      if (unidadMetros) { Serial.print(alturaOffset, 2); Serial.println(" m"); }
      else              { Serial.print(alturaOffset * 3.281f, 0); Serial.println(" ft"); }
      while(digitalRead(BUTTON_OLED) == LOW) { delay(10); }
    }
    return;  // Mientras se está editando, no procesar el resto del menú
  }

  // Si se presiona BUTTON_MENU y no estamos en modo edición, se cierra el menú
  if (digitalRead(BUTTON_MENU) == LOW) {
    delay(50);
    menuActivo = false;
    lastMenuInteraction = millis();
    Serial.println(T("Menú cerrado.", "Menu closed."));
    while(digitalRead(BUTTON_MENU) == LOW);
    return;
  }

  // Cambiar de opción con BUTTON_ALTITUDE
  if (digitalRead(BUTTON_ALTITUDE) == LOW) {
    delay(50);
    menuOpcion = (menuOpcion + 1) % TOTAL_OPCIONES;
    lastMenuInteraction = millis();
    Serial.print(T("Opción del menú cambiada a: ", "Menu option changed to: "));
    Serial.println(menuOpcion);
    while(digitalRead(BUTTON_ALTITUDE) == LOW);
  }

  // Confirmar opción con BUTTON_OLED (para opciones que no sean offset)
  if (digitalRead(BUTTON_OLED) == LOW) {
    delay(50);
    ejecutarOpcionMenu(menuOpcion);
    lastMenuInteraction = millis();
    while(digitalRead(BUTTON_OLED) == LOW);
  }

  // Si no hay interacción por 7 segundos, cerrar el menú
  if (millis() - lastMenuInteraction > 7000) {
    menuActivo = false;
    Serial.println(T("Menú cerrado por inactividad.", "Menu closed due to inactivity."));
  }
}

// --- Atenuación automática en modo Ahorro ---
static bool ahorroDimmed = false;
static unsigned long ahorroEnterMs = 0;
static const uint8_t AHORRO_DIM_CONTRAST = 5;     // brillo atenuado (0..255)
static const uint32_t ACTIVIDAD_VENTANA_MS = 200;  // pulsación reciente ~200 ms

static void gestionarBrilloAhorro() {
  SensorMode m = getSensorMode();
  unsigned long now = millis();

  // --- Recuperar brillo al presionar un botón (actividad reciente) ---
  unsigned long lastAct = getLastActivityMs();
  bool actividadReciente = (now - lastAct) <= ACTIVIDAD_VENTANA_MS;
  if (actividadReciente && ahorroDimmed) {
    u8g2.setContrast(brilloPantalla);   // o 255 si quieres forzar 100%
    ahorroDimmed = false;
    ahorroEnterMs = now;
  }

  // Condiciones para recuperar brillo al 100% (aunque no haya pulsación)
  bool mustRestore = (m != SENSOR_MODE_AHORRO) || powerLockActive() || enSalto;
  if (mustRestore) {
    if (ahorroDimmed) {
      u8g2.setContrast(brilloPantalla);
      ahorroDimmed = false;
    }
    ahorroEnterMs = 0;
    return;
  }

  // Seguimos en Ahorro
  if (ahorroEnterMs == 0) ahorroEnterMs = now;

  // 2 minutos = 120000 ms
  if (!ahorroDimmed && (now - ahorroEnterMs >= 120000UL)) {
    u8g2.setContrast(AHORRO_DIM_CONTRAST);
    ahorroDimmed = true;
    Serial.println(T("Atenuación de brillo: 2 min en modo Ahorro.", "Dimmed brightness: 2 min in Power save."));
  }
}


// ---------------------------------------------------------------------------
// Función de actualización de la UI (se llama en cada loop)
// ---------------------------------------------------------------------------
void updateUI() {
  if (!startupDone) {
    mostrarCuentaRegresiva();
    return;
  }
  if (gameSnakeRunning) return; // No actualizamos la UI si el juego está activo.
  if (!pantallaEncendida) return;

  gestionarBrilloAhorro();

  if (!menuActivo) {
    // ---------- Throttling del repintado por modo ----------
    static uint32_t t_last_ui = 0;
    uint16_t ui_interval = 140;                 // Ahorro:  ~7 Hz
    SensorMode m = getSensorMode();
    if (m == SENSOR_MODE_ULTRA_PRECISO) ui_interval = 100;  // ~10 Hz
    if (m == SENSOR_MODE_FREEFALL)     ui_interval = 80;    // ~12 Hz
    uint32_t now_ui = millis();
    if (now_ui - t_last_ui < ui_interval) return;
    t_last_ui = now_ui;
    // -------------------------------------------------------

    // Pantalla principal
    u8g2.clearBuffer();

    // Mostrar unidad (M o FT)
    u8g2.setFont(u8g2_font_ncenB08_tr);
    u8g2.setCursor(2, 12);
    u8g2.print(unidadMetros ? "M" : "FT");

    // Mostrar estado BLE centrado
    u8g2.setFont(u8g2_font_ncenB08_tr);
    String bleStr = "BL: " + String(bleActivo ? "ON" : "OFF");
    int xPosBle = (128 - u8g2.getStrWidth(bleStr.c_str())) / 2;
    u8g2.setCursor(xPosBle, 12);
    u8g2.print(bleStr);

    // --- Luna parpadeante si faltan ≤ 2 min para deep sleep ---
    // Mostrar SOLO si el sueño es posible: Ahorro + ahorro ON + sin power-lock
    if (ahorroTimeoutMs > 0 && !powerLockActive() && getSensorMode() == SENSOR_MODE_AHORRO) {
      unsigned long now      = millis();
      unsigned long lastAct  = getLastActivityMs();
      long msLeft            = (long)ahorroTimeoutMs - (long)(now - lastAct);

      if (msLeft > 0 && msLeft <= 120000L) {  // ≤ 2 minutos
        bool blinkOn = ((now / 500UL) % 2UL) == 0UL;
        if (blinkOn) {
          u8g2.setFont(u8g2_font_open_iconic_weather_1x_t);
          u8g2.drawGlyph(18, 12, 66);
          u8g2.setFont(u8g2_font_5x8_mf);
          u8g2.drawStr(27, 10, "zzz");
        }
      }
    }

    // Mostrar porcentaje de batería a la derecha (con parpadeo si ≤ 5%)
    {
      int pct = batteryGetPercent();
      bool blink = batteryIsLowPercent();
      bool showNow = true;
      if (blink) {
        // Parpadeo a 2 Hz (500 ms on/off)
        showNow = ((millis() / 500UL) % 2UL) == 0UL;
      }

      if (showNow) {
        u8g2.setFont(u8g2_font_ncenB08_tr);
        String batStr = String(pct) + "%";
        int batWidth = u8g2.getStrWidth(batStr.c_str());
        u8g2.setCursor(128 - batWidth - 2, 12);
        u8g2.print(batStr);
      }
    }

    // Altitud relativa ya calculada (NO leer el sensor aquí)
    float altRel_m = altCalculada;           // metros

    // Convertir a la unidad de visualización
    float altToShow = unidadMetros ? altRel_m : (altRel_m * 3.281f);

    // ---- Selección de formato (solo NORMAL o AUTO) ----
    String altDisplay;
    int fmt = normalizeAltFormat(altFormat);

    if (fmt == 4) {
      // ===== AUTO con escala =====
      float absAlt = fabsf(altToShow);
      if (absAlt < 999.0f) {
        long v = lroundf(altToShow);
        altDisplay = String(v);
      } else if (absAlt < 9999.0f) {
        float vK    = altToShow / 1000.0f;
        float vDisp = roundf(vK * 100.0f) / 100.0f;
        altDisplay  = String(vDisp, 2);
      } else {
        float vK    = altToShow / 1000.0f;
        float vDisp = roundf(vK * 10.0f) / 10.0f;
        altDisplay  = String(vDisp, 1);
      }
    } else {
      // ===== NORMAL =====
      float value = altToShow;
      altDisplay = String((long)value);
    }

    // Mostrar la altitud en grande, centrada
    u8g2.setFont(u8g2_font_fub30_tr);
    int xPosAlt = (128 - u8g2.getStrWidth(altDisplay.c_str())) / 2;
    if (xPosAlt < 0) xPosAlt = 0;
    u8g2.setCursor(xPosAlt, 50);
    u8g2.print(altDisplay);

    // Dibujar bordes (opcional)
    u8g2.drawHLine(0, 15, 128);
    u8g2.drawHLine(0, 52, 128);
    u8g2.drawHLine(0, 0, 128);
    u8g2.drawHLine(0, 63, 128);
    u8g2.drawVLine(0, 0, 64);
    u8g2.drawVLine(127, 0, 64);

    // Mostrar usuario centrado en la parte inferior
    u8g2.setFont(u8g2_font_ncenB08_tr);
    String user = usuarioActual;
    int xPosUser = (128 - u8g2.getStrWidth(user.c_str())) / 2;
    if (xPosUser < 0) xPosUser = 0;
    u8g2.setCursor(xPosUser, 62);
    u8g2.print(user);

    // Mostrar el contador de saltos en la esquina inferior derecha
    String jumpStr = String(jumpCount);
    int xPosJump = 128 - u8g2.getStrWidth(jumpStr.c_str()) - 14;
    int yPosJump = 62;
    u8g2.setCursor(xPosJump, yPosJump);
    u8g2.print(jumpStr);

    // Indicador de armado/salto en la esquina inferior izquierda
    if (jumpArmed || inJump) {
      if (inJump) u8g2.drawDisc(14, 58, 3);
      else        u8g2.drawCircle(14, 58, 3);
    }

    if (powerLockActive()) {
      u8g2.setFont(u8g2_font_open_iconic_thing_1x_t);
      u8g2.drawGlyph(26, 63, 79);
    }

    u8g2.sendBuffer();
  } else {
    // Menú / Batería / Edición
    if (editingOffset) {
      dibujarOffsetEdit();
    } else if (batteryMenuActive) {
      if (pantallaEncendida) {
        // Mostrar valores provenientes del módulo de batería (sin cálculos duplicados)
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

        if (digitalRead(BUTTON_OLED) == LOW) {
          delay(50);
          batteryMenuActive = false;
          lastMenuInteraction = millis();
          Serial.println(T("Menú de batería cerrado.", "Battery menu closed."));
          while(digitalRead(BUTTON_OLED) == LOW);
        }
      }
    } else {
      dibujarMenu();
    }
  }
}
