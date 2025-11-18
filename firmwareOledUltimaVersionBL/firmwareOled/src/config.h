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

// Pines de botones
#define BUTTON_ALTITUDE 6   // Navegación de menú
#define BUTTON_OLED     3   // Confirmar/seleccionar en UI
#define BUTTON_MENU     7   // Otras funciones de menú

// Botón de despertar (RTC) para deep sleep
// Usaremos el mismo botón lógico de "reactivar pantalla", ahora cableado al GPIO 3 (RTC).
// Sugerido: botón a GND con pull-up interno => activo en nivel bajo.
#define WAKE_BTN_PIN       3
#define WAKE_ACTIVE_LEVEL  0   // 0 = activo en LOW, 1 = activo en HIGH

// Idioma
#define LANG_ES 0
#define LANG_EN 1
extern int idioma;   // 0 = Español (por defecto), 1 = Inglés

// -------------------------
// BLE UUIDs
// -------------------------
#define SERVICE_UUID             "4fafc200-1fb5-459e-8fcc-c5c9c331914b"
#define CHARACTERISTIC_UUID      "beb5483e-36e1-4688-b7f5-ea07361b26a8"
#define DFU_SERVICE_UUID         "e3c0f200-3b0b-4253-9f53-3a351d8a146e"
#define DFU_CHARACTERISTIC_UUID  "e3c0f200-3b0b-4253-9f53-3a351d8a146e"
#define USERNAME_SERVICE_UUID        "12345678-1234-1234-1234-1234567890ab"
#define USERNAME_CHARACTERISTIC_UUID "abcd1234-ab12-cd34-ef56-1234567890ab"

// -------------------------
// Variables de configuración (cargadas desde NVS)
// -------------------------
extern bool unidadMetros;           // false = pies, true = metros
extern int  brilloPantalla;         // 0-255
extern int  altFormat;              // formato de altitud
extern int  ahorroTimeoutOption;    // índice de TIMEOUT_OPTIONS
extern unsigned long ahorroTimeoutMs;
extern bool inversionActiva;        // modo de inversión del display
extern String usuarioActual;
extern bool bleActivo;              // BLE activo o no
extern Preferences prefs;
extern int cachedBatteryPercentage;
extern const int TOTAL_OPCIONES;
extern const int OPCIONES_POR_PAGINA;
extern const int NUM_TIMEOUT_OPTIONS;
extern const long TIMEOUT_OPTIONS[];

// Estados globales relevantes para energía/UI (ya existentes en el proyecto)
extern bool menuActivo;             // true cuando el menú está abierto
extern bool gameSnakeRunning;       // true durante el juego Snake

// -------------------------
// Funciones de configuración y manejo de NVS
// -------------------------
void loadConfig();
void saveConfig();
void loadUserConfig();
void markFirmwareAsValid();

// -------------------------
// (Opcional) Parámetros del deep sleep post-aterrizaje
// Si no quieres parametrizar aquí, puedes borrar estas líneas:
// -------------------------
#ifndef LANDING_DS_ENABLE
#define LANDING_DS_ENABLE    1
#endif

#ifndef LANDING_DS_DELAY_MS
#define LANDING_DS_DELAY_MS  300000UL  // 5 minutos para DeepSleep al aterrizar
#endif

#endif
