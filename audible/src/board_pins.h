#pragma once

// ESP32-S3 Tiny SuperMini (según tu mapeo)
static constexpr int PIN_I2C_SDA = 6;   // GP6
static constexpr int PIN_I2C_SCL = 5;   // GP5
static constexpr uint32_t I2C_HZ   = 400000;

// Dirección típica del BMP390L (0x77 si SDO=VDD, 0x76 si SDO=GND)
static constexpr uint8_t BMP3_ADDR_DEFAULT = 0x77; // cambia a 0x76 si corresponde
// static constexpr uint8_t BMP3_ADDR_DEFAULT = 0x76;

// OLED 0.91" SSD1306
static constexpr int OLED_RESET_PIN   = -1;     // la mayoría sin RST
static constexpr uint8_t OLED_I2C_ADDR = 0x3C;  // a veces 0x3D

// Botón de usuario (INPUT_PULLUP, activo-bajo)
// Elige un GPIO "RTC-capable" si quieres wakeup desde light-sleep (ej. 0, 1, 2, 3, 4, 5… según tu placa)
static constexpr int PIN_BUTTON = 4;   // <-- AJUSTA al pin que realmente uses
static constexpr int PIN_BUZZER   = 3;