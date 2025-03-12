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
#include <math.h> // Para usar floor/trunc

// Pines I²C para la pantalla OLED y el sensor BMP390L
#define SDA_PIN 4
#define SCL_PIN 5

// Dirección I²C de la pantalla OLED y el sensor BMP390L
#define OLED_ADDR 0x3C
#define BMP_ADDR  0x77

// Pin del ADC para la batería (se vuelve a usar GPIO1)
#define BATTERY_PIN 1  

// Pines de los botones
#define BUTTON_ALTITUDE 6
#define BUTTON_OLED 7
#define BUTTON_MENU 8

// BLE - Nuevos UUIDs para servicio y característica
#define SERVICE_UUID        "4fafc201-1fb5-459e-8fcc-c5c9c331914b"
#define CHARACTERISTIC_UUID "beb5483e-36e1-4688-b7f5-ea07361b26a8"

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

// -----------------------------------------------------------------------------
// Inicializar BLE con nombre, servicio y característica
// -----------------------------------------------------------------------------
void setupBLE() {
  Serial.println("Inicializando BLE...");

  // Inicializar NimBLE sin nombre (se lo asignamos a continuación)
  NimBLEDevice::init("");
  NimBLEDevice::setDeviceName("ESP32-Altimetro");

  // Creamos un servidor BLE
  NimBLEServer *pServer = NimBLEDevice::createServer();
  Serial.println("Servidor BLE creado");

  // Creamos un servicio BLE con el UUID indicado
  NimBLEService *pService = pServer->createService(SERVICE_UUID);
  Serial.println("Servicio BLE creado");

  // Creamos la característica (READ y NOTIFY)
  pCharacteristic = pService->createCharacteristic(
      CHARACTERISTIC_UUID,
      NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::NOTIFY
  );
  Serial.println("Característica BLE creada");

  // Iniciamos el servicio
  pService->start();
  Serial.println("Servicio BLE iniciado");

  // Configuramos la publicidad
  NimBLEAdvertising *pAdvertising = NimBLEDevice::getAdvertising();

  // Datos de publicidad
  NimBLEAdvertisementData advData;
  advData.setName("ESP32-Altimetro");
  advData.setCompleteServices(pService->getUUID());
  pAdvertising->setAdvertisementData(advData);

  // Datos de respuesta al escaneo (scan response)
  NimBLEAdvertisementData scanData;
  scanData.setName("ESP32-Altimetro");
  pAdvertising->setScanResponseData(scanData);

  // Iniciamos la publicidad
  pAdvertising->start();
  Serial.println("BLE advertising iniciado");
}

// -----------------------------------------------------------------------------
// SETUP
// -----------------------------------------------------------------------------
void setup() {
  Serial.begin(115200);
  delay(1000);
  Serial.println("Setup iniciado");

  // Inicializar I2C
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

  // Configurar ADC en el pin BATTERY_PIN (GPIO1) usando ADC1_CHANNEL_1
  adc1_config_width(ADC_WIDTH_BIT_12);
  adc1_config_channel_atten(ADC1_CHANNEL_1, ADC_ATTEN_DB_11);

  Serial.println("Setup completado");
}

// -----------------------------------------------------------------------------
// LOOP
// -----------------------------------------------------------------------------
void loop() {
  // Manejo del botón de menú
  if (digitalRead(BUTTON_MENU) == LOW) {
    menuActivo = !menuActivo;
    delay(500);
  }

  // Leer voltaje de la batería en el ADC (usando ADC1_CHANNEL_1)
  int lecturaADC = adc1_get_raw(ADC1_CHANNEL_1);
  float voltajeADC = (lecturaADC / 4095.0) * 2.6;  // Convertir la lectura ADC a voltaje

  // Aplicar factor de corrección
  float factorCorreccion = 1.238;  // Ajuste basado en medición real
  float voltajeBateria = voltajeADC * 2.0 * factorCorreccion;
  int porcentajeBateria = calcularPorcentajeBateria(voltajeBateria);

  // Leer altitud y corregirla
  float altitudCorrigida = 0.0;
  if (!bmp.performReading()) {
    Serial.println("Error al leer el sensor BMP390L");
  } else {
    float altitudActual = bmp.readAltitude(1013.25);  // Presión estándar al nivel del mar
    altitudCorrigida = altitudActual - altitudReferencia;
    // Convertir a pies si la unidad es false (por defecto)
    if (!unidadMetros) {
      altitudCorrigida *= 3.281;
    }
  }

  // Aplicar factor de escala: multiplicar la altitud por 10^-3
  float altitudEscalada = altitudCorrigida * 0.001;
  // Mostrar con tres decimales (se deja comentada la versión de 2 decimales para el futuro)
  // float altitudMostrar = floor(altitudEscalada * 100) / 100.0;  // Truncamiento a 2 decimales
  float altitudMostrar = floor(altitudEscalada * 1000) / 1000.0;  // Truncamiento a 3 decimales

  // ---------------------------------------------------------------------------
  // Enviar la altitud vía BLE
  // ---------------------------------------------------------------------------
  String altString = String(altitudMostrar, 3);
  pCharacteristic->setValue(altString.c_str());
  pCharacteristic->notify();

  // ---------------------------------------------------------------------------
  // Mostrar datos en la pantalla OLED
  // ---------------------------------------------------------------------------
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
  u8g2.print(altitudMostrar, 3);
  u8g2.print(unidadMetros ? "m" : "ft");

  u8g2.sendBuffer();

  delay(1000);
}
