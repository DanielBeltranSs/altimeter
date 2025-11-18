#ifndef CONFIG_H
#define CONFIG_H

#include <Arduino.h>
#include <Preferences.h>
#include <esp_ota_ops.h>

// =========================
// Pines y hardware base
// =========================

// --- I2C (bus de sensores/RTC) ---
#define SDA_PIN 3
#define SCL_PIN 2

// --- RTC externo (DS3231) ---
#ifndef USE_DS3231
  #define USE_DS3231 1          // 1=usar soporte DS3231; 0=compilar sin RTC externo
#endif
#ifndef DS3231_ADDR
  #define DS3231_ADDR 0x68
#endif
#ifndef DS3231_STORES_UTC
  #define DS3231_STORES_UTC 1   // guardamos y leemos UTC en el DS3231
#endif
#ifndef DS3231_INT_PIN
  #define DS3231_INT_PIN (-1)   // INT/SQW para wake (−1 = no usado)
#endif
#ifndef RTC_REQUIRE_DS3231
  #define RTC_REQUIRE_DS3231 0   // 1 = si no hay DS3231, no hay hora válida
#endif

// ===============================
// LCD ST7567 (JLX12864) – SPI SW
// ===============================
#ifndef LCD_SCK
  #define LCD_SCK   4 //SDC
#endif
#ifndef LCD_MOSI
  #define LCD_MOSI  5
#endif
#ifndef LCD_CS
  #define LCD_CS    8
#endif
#ifndef LCD_DC
  #define LCD_DC    6
#endif
#ifndef LCD_RST
  #define LCD_RST   7
#endif
#ifndef LCD_LED
  #define LCD_LED   9           // Backlight activo-bajo
#endif

// PWM de backlight (255 = apagado total, activo-bajo)
#ifndef LCD_LEDC_CH
  #define LCD_LEDC_CH 0
#endif
#ifndef LCD_LEDC_FREQ
  #define LCD_LEDC_FREQ 3000
#endif
#ifndef LCD_LEDC_RES_BITS
  #define LCD_LEDC_RES_BITS 8
#endif

// =========================
// Sensores / ADCs
// =========================
#define BMP_ADDR     0x77
#define BATTERY_PIN  1

// -----------------------------------------------------------
// Detección de carga (VBUS) por ADC + posible wake por HIGH
// Divisor: 330k (a 5V) / 510k (a GND) -> ~3.0V en el pin.
// -----------------------------------------------------------
#ifndef CHARGE_R_TOP_OHMS
  #define CHARGE_R_TOP_OHMS  330000.0   // a VBUS (5V)
#endif
#ifndef CHARGE_R_BOT_OHMS
  #define CHARGE_R_BOT_OHMS  510000.0   // a GND
#endif
#ifndef CHARGE_ENTER_MV
  #define CHARGE_ENTER_MV    4200       // umbral de “cargador presente” (mV)
#endif
#ifndef CHARGE_EXIT_MV
  #define CHARGE_EXIT_MV     3800       // histéresis de salida (mV)
#endif

// =========================
// Botones / Wake
// =========================
#define BUTTON_ALTITUDE 14   // navegación
#define BUTTON_OLED    12   // confirmar/lock
#define BUTTON_MENU     13   // menú

// Botón/entrada de wake (si se usa ext0/ext1; aquí solo referencia lógica)
#define WAKE_BTN_PIN       12
#define WAKE_ACTIVE_LEVEL  1   // 0 = activo en LOW, 1 = activo en HIGH

// Salida de vibrador / MOSFET
#define VIBRO_PIN        15
#define VIBRO_ACTIVE_HIGH 1

// =========================
// Idioma
// =========================
#define LANG_ES 0
#define LANG_EN 1
extern int idioma;   // 0 = Español (por defecto), 1 = Inglés

// ===========================================
// Parámetros de OFFSET de altitud (canónicos)
// ===========================================
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
  #define OFFSET_ZERO_EPS_M     0.05f      // |offset| < 5 cm => guarda 0
#endif

// ===========================================
// Deadband de visualización (alrededor de 0)
// ===========================================
#ifndef UI_DEADBAND_FT
  #define UI_DEADBAND_FT   0.00f//10.0f           // ±10 ft
#endif
#ifndef UI_DEADBAND_M
  #define UI_DEADBAND_M    0.00f //3.05f          // ±3.05 m (~10 ft)
#endif

// ===========================================
// Variables de configuración (desde NVS)
// ===========================================
extern bool   unidadMetros;        // false = pies, true = metros
extern int    brilloPantalla;      // 0-255
extern int    altFormat;           // 0=normal, 4=AUTO
extern int    ahorroTimeoutOption; // índice en TIMEOUT_OPTIONS
extern unsigned long ahorroTimeoutMs;
extern bool   inversionActiva;     // invertir display
extern String usuarioActual;
extern float  alturaOffset;        // offset en METROS (persistente)
extern Preferences prefs;

extern int cachedBatteryPercentage; // definido en módulo de batería

extern const long TIMEOUT_OPTIONS[];
extern const int  NUM_TIMEOUT_OPTIONS;

extern bool menuActivo;             // menú abierto
extern bool gameSnakeRunning;       // juego activo

// ===============================
// Auto Ground Zero (AGZ) – drift
// ===============================
extern float agzBias;               // sesgo acumulado (METROS), persistente

#ifndef AGZ_WINDOW_M
  #define AGZ_WINDOW_M          8.0f
#endif
#ifndef AGZ_VZ_QUIET_MPS
  #define AGZ_VZ_QUIET_MPS      0.35f
#endif
#ifndef AGZ_STABLE_MS
  #define AGZ_STABLE_MS         60000UL   // 60 s
#endif
#ifndef AGZ_TAU_SECONDS
  #define AGZ_TAU_SECONDS       (20*60)   // τ = 20 min
#endif
#ifndef AGZ_RATE_LIMIT_MPH
  #define AGZ_RATE_LIMIT_MPH    4.0f      // ≈ 2 m/h
#endif
#ifndef AGZ_SAVE_DELTA_M
  #define AGZ_SAVE_DELTA_M      0.50f
#endif
#ifndef AGZ_SAVE_PERIOD_MS
  #define AGZ_SAVE_PERIOD_MS    1200000UL // 20 min
#endif
#ifndef AGZ_BIAS_CLAMP_M
  #define AGZ_BIAS_CLAMP_M      12.0f
#endif

// Guardado ligero SOLO del sesgo (minimiza desgaste de NVS)
void saveAgzBias();

// ===============================
// Config y NVS
// ===============================
void loadConfig();
void saveConfig();
void loadUserConfig();

// ================================================
// (Opcional) Deep sleep post-aterrizaje automático
// ================================================
#ifndef LANDING_DS_ENABLE
  #define LANDING_DS_ENABLE    1
#endif
#ifndef LANDING_DS_DELAY_MS
  #define LANDING_DS_DELAY_MS  300000UL   // 5 min
#endif

#endif // CONFIG_H
