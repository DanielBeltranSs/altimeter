#include <Arduino.h>
#include "esp_bt.h"
#include <Wire.h>
#include <U8g2lib.h>
#include <Adafruit_BMP3XX.h>
#include <NimBLEDevice.h>
#include <NimBLEServer.h>
#include <NimBLEUtils.h>
#include <NimBLE2904.h>
#include <driver/adc.h>
#include <math.h>  // Para usar la función floor

// Pines I²C para la pantalla OLED y el sensor BMP390L
#define SDA_PIN 4
#define SCL_PIN 5

// Dirección I²C de la pantalla OLED y el sensor BMP390L
#define OLED_ADDR 0x3C
#define BMP_ADDR  0x77

// **Pin del ADC para la batería (GPIO1 - A1)**
#define BATTERY_PIN 1  

// Pines de los botones
#define BUTTON_ALTITUDE 6  
#define BUTTON_OLED 7      
#define BUTTON_MENU 8      

// BLE - Definir servicio y característica UUIDs
#define SERVICE_UUID "12345678-1234-5678-1234-56789abcdef0"
#define CHARACTERISTIC_UUID "87654321-4321-6789-4321-abcdef987654"

// Variables de estado
bool pantallaEncendida = true;
bool menuActivo = false;
int menuOpcion = 0;
int brilloPantalla = 255;
// Unidad por defecto: false indica que se usa pies (se puede cambiar desde el menú)
bool unidadMetros = false;

// Inicializar pantalla OLED
U8G2_SH1106_128X64_NONAME_F_HW_I2C u8g2(U8G2_R0, /* reset=*/ U8X8_PIN_NONE);

// Inicializar sensor BMP390L
Adafruit_BMP3XX bmp;

// Variable para altitud de referencia
float altitudReferencia = 0.0;

// Característica BLE
BLECharacteristic *pCharacteristic;

// Función para calcular el porcentaje de batería
int calcularPorcentajeBateria(float voltaje) {
    if (voltaje >= 4.2) return 100;
    if (voltaje >= 4.1) return 95;
    if (voltaje >= 4.0) return 90;
    if (voltaje >= 3.9) return 85;
    if (voltaje >= 3.8) return 80;
    if (voltaje >= 3.7) return 75;
    if (voltaje >= 3.6) return 50;
    if (voltaje >= 3.5) return 25;
    if (voltaje >= 3.4) return 10;
    return 5; 
}

// Inicializar BLE
void setupBLE() {
    NimBLEDevice::init("ESP32-Altimetro");
    NimBLEServer *pServer = NimBLEDevice::createServer();
    NimBLEService *pService = pServer->createService(SERVICE_UUID);
    pCharacteristic = pService->createCharacteristic(
        CHARACTERISTIC_UUID,
        NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::NOTIFY
    );
    pService->start();
    NimBLEAdvertising *pAdvertising = NimBLEDevice::getAdvertising();
    pAdvertising->addServiceUUID(SERVICE_UUID);
    pAdvertising->start();
}

void setup() {
    Serial.begin(115200);
    Wire.begin(SDA_PIN, SCL_PIN);

    // Inicializar BLE
    setupBLE();

    // Inicializar pantalla OLED
    u8g2.begin();
    u8g2.setPowerSave(false);
    u8g2.setContrast(brilloPantalla);

    // Inicializar sensor BMP390L
    if (!bmp.begin_I2C(BMP_ADDR)) {
        Serial.println("¡Sensor BMP390L no encontrado!");
        while (1);
    }

    bmp.setTemperatureOversampling(BMP3_OVERSAMPLING_8X);
    bmp.setPressureOversampling(BMP3_OVERSAMPLING_4X);
    bmp.setIIRFilterCoeff(BMP3_IIR_FILTER_COEFF_3);

    // Configurar botones con pull-up interno
    pinMode(BUTTON_ALTITUDE, INPUT_PULLUP);
    pinMode(BUTTON_OLED, INPUT_PULLUP);
    pinMode(BUTTON_MENU, INPUT_PULLUP);

    // **Configurar ADC en GPIO1 con atenuación de 11dB**
    adc1_config_width(ADC_WIDTH_BIT_12);
    adc1_config_channel_atten(ADC1_CHANNEL_1, ADC_ATTEN_DB_11);
}

void loop() {
    if (digitalRead(BUTTON_MENU) == LOW) {
        menuActivo = !menuActivo;
        delay(500);
    }

    // Leer voltaje de la batería en GPIO1 y calcular porcentaje
    int lecturaADC = adc1_get_raw(ADC1_CHANNEL_1);
    float voltajeADC = (lecturaADC / 4095.0) * 2.6;  // Convertir lectura ADC a voltaje

    // **Aplicar Factor de Corrección**
    float factorCorreccion = 1.238;  // Ajuste basado en medición real
    float voltajeBateria = voltajeADC * 2.0 * factorCorreccion;
    int porcentajeBateria = calcularPorcentajeBateria(voltajeBateria);

    // Leer altitud y corregirla
    float altitudCorrigida = 0.0;
    if (!bmp.performReading()) {
        Serial.println("Error al leer el sensor BMP390L");
    } else {
        float altitudActual = bmp.readAltitude(1013.25);
        altitudCorrigida = altitudActual - altitudReferencia;
        // Convertir a pies si la unidad es false (por defecto)
        if (!unidadMetros) {
            altitudCorrigida *= 3.281;
        }
    }

    // Aplicar factor de escala: multiplicar la altitud por 10^-3 y truncar a 2 decimales
    float altitudEscalada = altitudCorrigida * 0.001;
    float altitudMostrar = floor(altitudEscalada * 100) / 100.0;  // Truncamiento a 2 decimales

    // 📟 **Mostrar datos en la pantalla OLED**
    u8g2.clearBuffer();
    u8g2.setFont(u8g2_font_ncenB08_tr);

    u8g2.setCursor(5, 10);
    u8g2.print("ADC: ");
    u8g2.setCursor(50, 10);
    u8g2.print(lecturaADC);

    u8g2.setCursor(5, 20);
    u8g2.print("V_ADC: ");
    u8g2.setCursor(50, 20);
    u8g2.print(voltajeADC, 2);
    u8g2.print("V");

    u8g2.setCursor(5, 30);
    u8g2.print("V_Bat: ");
    u8g2.setCursor(50, 30);
    u8g2.print(voltajeBateria, 2);
    u8g2.print("V");

    u8g2.setCursor(5, 40);
    u8g2.print("Bat: ");
    u8g2.setCursor(50, 40);
    u8g2.print(porcentajeBateria);
    u8g2.print("%");

    u8g2.setCursor(5, 50);
    u8g2.print("Alt: ");
    u8g2.setCursor(50, 50);
    u8g2.print(altitudMostrar, 2);
    u8g2.print(unidadMetros ? "m" : "ft");

    u8g2.sendBuffer();

    delay(1000);
}
