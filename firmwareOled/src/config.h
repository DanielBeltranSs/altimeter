#ifndef CONFIG_H
#define CONFIG_H

#include <Arduino.h>
#include <Preferences.h>
#include <esp_ota_ops.h>

// Definiciones de pines y hardware
#define SDA_PIN 4
#define SCL_PIN 5

#define OLED_ADDR 0x3C
#define BMP_ADDR  0x77
#define BATTERY_PIN 1

// Pines de botones
#define BUTTON_ALTITUDE 6   // Navegación de menú
#define BUTTON_OLED     8   // Confirmar/seleccionar en UI
#define BUTTON_MENU     7   // Otras funciones de menú

// BLE UUIDs
#define SERVICE_UUID        "4fafc200-1fb5-459e-8fcc-c5c9c331914b"
#define CHARACTERISTIC_UUID "beb5483e-36e1-4688-b7f5-ea07361b26a8"
#define DFU_SERVICE_UUID         "e3c0f200-3b0b-4253-9f53-3a351d8a146e"
#define DFU_CHARACTERISTIC_UUID  "e3c0f200-3b0b-4253-9f53-3a351d8a146e"
#define USERNAME_SERVICE_UUID        "12345678-1234-1234-1234-1234567890ab"
#define USERNAME_CHARACTERISTIC_UUID "abcd1234-ab12-cd34-ef56-1234567890ab"

// Variables de configuración (cargadas desde NVS)
extern bool unidadMetros;         // false = pies, true = metros
extern int brilloPantalla;          // 0-255
extern int altFormat;               // formato de altitud
extern int ahorroTimeoutOption;     
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

// Funciones de configuración y manejo de NVS
void loadConfig();
void saveConfig();
void loadUserConfig();
void markFirmwareAsValid();

#endif
