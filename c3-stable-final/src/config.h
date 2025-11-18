#ifndef CONFIG_H
#define CONFIG_H

#include <Arduino.h>
#include <Preferences.h>
#include <esp_ota_ops.h>

// -------------------------
// Definiciones de pines y hardware
// -------------------------
#define SDA_PIN 4
#define SCL_PIN 5

#define OLED_ADDR 0x3C
#define BMP_ADDR  0x77
#define BATTERY_PIN 1

// >>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>
// Detección de carga (VBUS) por ADC + wake por HIGH (UN SOLO PIN)
// Divisor: 330k (arriba a 5V) / 510k (abajo a GND) -> ~3.0V en el pin.
// Usamos un GPIO que sea ADC + RTC para poder despertar desde deep sleep.
// En ESP32-C3 SuperMini, GPIO2 cumple ambas cosas y está libre aquí.
// >>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>
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
// <<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<

// Pines de botones
#define BUTTON_ALTITUDE 6   // Navegación de menú
#define BUTTON_OLED     3   // Confirmar/seleccionar en UI
#define BUTTON_MENU     7   // Otras funciones de menú

// Botón de despertar (RTC) para deep sleep
// Usaremos el mismo botón lógico de "reactivar pantalla", ahora cableado al GPIO 3 (RTC).
// Sugerido: botón a GND con pull-up interno => activo en nivel bajo.
#define WAKE_BTN_PIN       3
#define WAKE_ACTIVE_LEVEL  1   // 0 = activo en LOW, 1 = activo en HIGH

#define VIBRO_PIN 9   // cambia a tu pin, p.ej. 0 o 3 según tu placa
#define VIBRO_ACTIVE_HIGH 1 

// Idioma
#define LANG_ES 0
#define LANG_EN 1
extern int idioma;   // 0 = Español (por defecto), 1 = Inglés

// -------------------------
// Parámetros del OFFSET de altitud (canónicos)
// -------------------------
#ifndef ALTURA_OFFSET_MIN_M
#define ALTURA_OFFSET_MIN_M   (-300.0f)   // rango seguro mínimo (m)
#endif
#ifndef ALTURA_OFFSET_MAX_M
#define ALTURA_OFFSET_MAX_M   ( 300.0f)   // rango seguro máximo (m)
#endif
#ifndef OFFSET_STEP_M
#define OFFSET_STEP_M         0.1f        // paso por toque en metros
#endif
#ifndef OFFSET_STEP_FT
#define OFFSET_STEP_FT        1.0f        // paso por toque en pies
#endif
#ifndef OFFSET_ACCEL
#define OFFSET_ACCEL          10.0f       // multiplicador en pulsación larga
#endif
#ifndef OFFSET_ZERO_EPS_M
#define OFFSET_ZERO_EPS_M     0.05f       // |offset| < 5 cm => guarda 0
#endif

// -------------------------
// Deadband de visualización (alrededor del offset del usuario)
// -------------------------
#ifndef UI_DEADBAND_FT
#define UI_DEADBAND_FT   0.0f//10.0f   // ±10 ft
#endif

#ifndef UI_DEADBAND_M
#define UI_DEADBAND_M     0.0f//3.05f  // ±3.05 m (~10 ft)
#endif


// -------------------------
// Variables de configuración (cargadas desde NVS)
// -------------------------
extern bool unidadMetros;           // false = pies, true = metros
extern int  brilloPantalla;         // 0-255
extern int  altFormat;              // formato de altitud (0=normal, 4=AUTO)
extern int  ahorroTimeoutOption;    // índice de TIMEOUT_OPTIONS
extern unsigned long ahorroTimeoutMs;
extern bool inversionActiva;        // modo de inversión del display
extern String usuarioActual;
extern float alturaOffset;          // <-- offset en METROS (persistente)
extern Preferences prefs;

extern int cachedBatteryPercentage; // definido en módulo de batería (si aplica)

// Tabla de timeouts y su cantidad
extern const long TIMEOUT_OPTIONS[];
extern const int  NUM_TIMEOUT_OPTIONS;

// Estados globales relevantes para energía/UI (ya existentes en el proyecto)
extern bool menuActivo;             // true cuando el menú está abierto
extern bool gameSnakeRunning;       // true durante el juego Snake

// -------------------------
// Auto Ground Zero (AGZ) SIEMPRE ACTIVO
// Corrige drift barométrico en tierra sin tocar el offset del usuario.
// Todos los parámetros son internos y fijos para producto comercial.
// -------------------------
extern float agzBias;               // Sesgo acumulado (en METROS), persistente

// Parámetros comerciales (fijos); se pueden sobreescribir en compile-time si se desea
#ifndef AGZ_WINDOW_M
#define AGZ_WINDOW_M          8.0f        // aprende si |rel_sin_offset| < 8 m
#endif
#ifndef AGZ_VZ_QUIET_MPS
#define AGZ_VZ_QUIET_MPS      0.35f       // suelo si |Vz| < 0.35 m/s
#endif
#ifndef AGZ_STABLE_MS
#define AGZ_STABLE_MS         60000UL     // 60 s estable antes de aprender
#endif
#ifndef AGZ_TAU_SECONDS
#define AGZ_TAU_SECONDS       (20*60)     // τ = 20 min
#endif
#ifndef AGZ_RATE_LIMIT_MPH
#define AGZ_RATE_LIMIT_MPH    4.0f        // tope 2 m/h (≈ 6.5 ft/h)
#endif
#ifndef AGZ_SAVE_DELTA_M
#define AGZ_SAVE_DELTA_M      0.50f       // guardar si cambia ≥ 0.5 m
#endif
#ifndef AGZ_SAVE_PERIOD_MS
#define AGZ_SAVE_PERIOD_MS    1200000UL   // o cada 20 min
#endif
#ifndef AGZ_BIAS_CLAMP_M
#define AGZ_BIAS_CLAMP_M      12.0f       // |agzBias| ≤ 12 m (failsafe)
#endif

// Guardado ligero SOLO del sesgo (minimiza desgaste de NVS)
void saveAgzBias();

// -------------------------
// Funciones de configuración y manejo de NVS
// -------------------------
void loadConfig();
void saveConfig();
void loadUserConfig();

// -------------------------
// (Opcional) Parámetros del deep sleep post-aterrizaje
// -------------------------
#ifndef LANDING_DS_ENABLE
#define LANDING_DS_ENABLE    1
#endif

#ifndef LANDING_DS_DELAY_MS
#define LANDING_DS_DELAY_MS  300000UL  // 5 minutos para DeepSleep al aterrizar
#endif

#endif // CONFIG_H
