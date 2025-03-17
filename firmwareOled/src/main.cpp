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
#include <math.h> // Para usar pow() y floor()

// Pines I²C
#define SDA_PIN 4
#define SCL_PIN 5

// Direcciones I²C
#define OLED_ADDR 0x3C
#define BMP_ADDR  0x77

// Pin ADC para batería (GPIO1)
#define BATTERY_PIN 1  

// Pines de botones
#define BUTTON_ALTITUDE 6  // En modo normal: reinicia altitud; en menú: cicla opciones
#define BUTTON_OLED     8  // En modo normal: suspende/reactiva pantalla; en menú: aplica opción actual
#define BUTTON_MENU     7  // Abre/cierra el menú

// BLE - UUIDs
#define SERVICE_UUID        "4fafc201-1fb5-459e-8fcc-c5c9c331914b"
#define CHARACTERISTIC_UUID "beb5483e-36e1-4688-b7f5-ea07361b26a8"

// Variables de estado
bool pantallaEncendida = true;
bool menuActivo = false;
int menuOpcion = 0;  // Opciones: 0: Unidad, 1: Brillo, 2: Formato Altura, 3: Bateria
unsigned long menuStartTime = 0;
int brilloPantalla = 255;
bool unidadMetros = false;  // false = pies, true = metros
// altFormat: 0 = normal (muestra altitud completa sin escalado),
// 1,2,3 = se aplica escala (multiplicación por 10^-3) y se muestra con ese número de decimales.
int altFormat = 0;  // Por defecto en modo "normal"
bool batteryMenuActive = false;  // Vista "Bateria" en el menú

// Variables para la actualización del porcentaje de batería
unsigned long lastBatteryUpdate = 0;
const unsigned long batteryUpdateInterval = 5000; // 5000 ms = 5 segundos
int cachedBatteryPercentage = 0;

// Inicializar OLED (128x64)
U8G2_SH1106_128X64_NONAME_F_HW_I2C u8g2(U8G2_R0, U8X8_PIN_NONE);

// Sensor BMP390L
Adafruit_BMP3XX bmp;

// Altitud de referencia (por defecto 0)
float altitudReferencia = 0.0;

// BLE
BLECharacteristic *pCharacteristic;

// Función para calcular porcentaje de batería a partir del voltaje
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

// Función para actualizar la lectura de la batería (cada batteryUpdateInterval ms)
void updateBatteryReading() {
  if (millis() - lastBatteryUpdate >= batteryUpdateInterval) {
    int lecturaADC = adc1_get_raw(ADC1_CHANNEL_1);
    float voltajeADC = (lecturaADC / 4095.0) * 2.6;  // Lectura ADC real
    float v_adc = voltajeADC;
    float v_bat = v_adc * 2.0; // Se usa solo multiplicación por 2
    cachedBatteryPercentage = calcularPorcentajeBateria(v_bat);
    lastBatteryUpdate = millis();
  }
}

// -----------------------------------------------------------------------------
// Configurar BLE
// -----------------------------------------------------------------------------
void setupBLE() {
  Serial.println("Inicializando BLE...");
  NimBLEDevice::init("");
  NimBLEDevice::setDeviceName("ESP32-Altimetro");

  NimBLEServer *pServer = NimBLEDevice::createServer();
  Serial.println("Servidor BLE creado");

  NimBLEService *pService = pServer->createService(SERVICE_UUID);
  Serial.println("Servicio BLE creado");

  pCharacteristic = pService->createCharacteristic(
      CHARACTERISTIC_UUID,
      NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::NOTIFY
  );
  Serial.println("Característica BLE creada");

  pService->start();
  Serial.println("Servicio BLE iniciado");

  NimBLEAdvertising *pAdvertising = NimBLEDevice::getAdvertising();
  NimBLEAdvertisementData advData;
  advData.setName("ESP32-Altimetro");
  advData.setCompleteServices(pService->getUUID());
  pAdvertising->setAdvertisementData(advData);

  NimBLEAdvertisementData scanData;
  scanData.setName("ESP32-Altimetro");
  pAdvertising->setScanResponseData(scanData);

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

  Wire.begin(SDA_PIN, SCL_PIN);
  setupBLE();

  u8g2.begin();
  u8g2.setPowerSave(false);
  u8g2.setContrast(brilloPantalla);

  if (!bmp.begin_I2C(BMP_ADDR)) {
    Serial.println("¡Sensor BMP390L no encontrado!");
    while (1);
  }
  bmp.setTemperatureOversampling(BMP3_OVERSAMPLING_8X);
  bmp.setPressureOversampling(BMP3_OVERSAMPLING_4X);
  bmp.setIIRFilterCoeff(BMP3_IIR_FILTER_COEFF_3);

  // No se modifica altitudReferencia al arrancar; se muestra la lectura real

  pinMode(BUTTON_ALTITUDE, INPUT_PULLUP);
  pinMode(BUTTON_OLED, INPUT_PULLUP);
  pinMode(BUTTON_MENU, INPUT_PULLUP);

  adc1_config_width(ADC_WIDTH_BIT_12);
  adc1_config_channel_atten(ADC1_CHANNEL_1, ADC_ATTEN_DB_11);

  Serial.println("Setup completado");
}

// -----------------------------------------------------------------------------
// LOOP
// -----------------------------------------------------------------------------
void loop() {
  // Leer sensor BMP
  bool sensorOk = bmp.performReading();
  float altitudActual = sensorOk ? bmp.readAltitude(1013.25) : 0;

  // -------------------------------
  // Gestión de botones
  // -------------------------------
  // BOTÓN MENU (pin 8): abre o cierra el menú (sin modificar la opción actual)
  if (digitalRead(BUTTON_MENU) == LOW) {
    menuActivo = !menuActivo;
    if (menuActivo) {
      menuOpcion = 0;
      batteryMenuActive = false;
      menuStartTime = millis();
      Serial.println("Menú abierto.");
    } else {
      Serial.println("Menú cerrado.");
    }
    delay(50);
  }

  // BOTÓN ALTITUDE (pin 6): en modo menú, cicla entre opciones; en modo normal, reinicia altitud
  if (digitalRead(BUTTON_ALTITUDE) == LOW) {
    if (menuActivo) {
      menuOpcion = (menuOpcion + 1) % 4;
      if(menuOpcion != 3) batteryMenuActive = false;
      menuStartTime = millis();
      Serial.print("Opción del menú cambiada a: ");
      switch(menuOpcion) {
        case 0: Serial.println("Unidad"); break;
        case 1: Serial.println("Brillo"); break;
        case 2: Serial.println("Formato Altura"); break;
        case 3: Serial.println("Bateria"); break;
      }
    } else {
      altitudReferencia = altitudActual;
      Serial.println("Altitud reiniciada a cero.");
    }
    delay(50);
  }

  // BOTÓN OLED (pin 7): en modo menú, aplica la opción; en modo normal, suspende/reactiva la pantalla
  if (digitalRead(BUTTON_OLED) == LOW) {
    if (menuActivo) {
      switch(menuOpcion) {
        case 0:
          unidadMetros = !unidadMetros;
          Serial.print("Unidad cambiada a: ");
          Serial.println(unidadMetros ? "metros" : "pies");
          break;
        case 1:
          brilloPantalla += 50;
          if (brilloPantalla > 255) brilloPantalla = 50;
          u8g2.setContrast(brilloPantalla);
          Serial.print("Brillo cambiado a: ");
          Serial.println(brilloPantalla);
          break;
        case 2:
          altFormat = (altFormat + 1) % 4;
          Serial.print("Formato de altitud cambiado a: ");
          switch(altFormat) {
            case 0: Serial.println("normal"); break;
            case 1: Serial.println("1 decimal"); break;
            case 2: Serial.println("2 decimales"); break;
            case 3: Serial.println("3 decimales"); break;
          }
          break;
        case 3:
          batteryMenuActive = !batteryMenuActive;
          Serial.print("Vista Bateria en menú: ");
          Serial.println(batteryMenuActive ? "ON" : "OFF");
          break;
      }
      menuStartTime = millis();
    } else {
      if (pantallaEncendida) {
        u8g2.setPowerSave(true);
        pantallaEncendida = false;
        Serial.println("Pantalla suspendida.");
      } else {
        u8g2.setPowerSave(false);
        pantallaEncendida = true;
        Serial.println("Pantalla activada.");
      }
    }
    delay(50);
  }

  // Cerrar menú automáticamente tras 7 s de inactividad
  if (menuActivo && (millis() - menuStartTime > 7000)) {
    menuActivo = false;
    batteryMenuActive = false;
    Serial.println("Menú cerrado por timeout.");
  }

  // -------------------------------
  // Actualización de la batería (cada 5 s)
  updateBatteryReading();
  
  // Visualización y envío vía BLE
  if (menuActivo) {
    if (batteryMenuActive) {
      // Vista "Bateria" en el menú
      if (pantallaEncendida) {
        int lecturaADC = adc1_get_raw(ADC1_CHANNEL_1);
        float voltajeADC = (lecturaADC / 4095.0) * 2.6;
        float v_adc = voltajeADC;
        float v_bat = v_adc * 2.0; // Sin factor de corrección
        u8g2.clearBuffer();
        u8g2.setFont(u8g2_font_ncenB08_tr);
        u8g2.setCursor(0, 12);
        u8g2.print("Bateria:");
        u8g2.setCursor(0, 26);
        u8g2.print("ADC: ");
        u8g2.setCursor(50, 26);
        u8g2.print(lecturaADC);
        u8g2.setCursor(0, 40);
        u8g2.print("V_ADC: ");
        u8g2.setCursor(50, 40);
        u8g2.print(v_adc, 2);
        u8g2.print("V");
        u8g2.setCursor(0, 54);
        u8g2.print("V_Bat: ");
        u8g2.setCursor(50, 54);
        u8g2.print(v_bat, 2);
        u8g2.sendBuffer();
      }
    } else {
      // Menú normal: mostrar las 4 opciones en líneas ajustadas (y = 12, 24, 36, 48, y 60 para la última)
      if (pantallaEncendida) {
        u8g2.clearBuffer();
        u8g2.setFont(u8g2_font_ncenB08_tr);
        u8g2.setCursor(0, 12);
        u8g2.print("MENU:");
        u8g2.setCursor(0, 24);
        if (menuOpcion == 0)
          u8g2.print("> Unidad: ");
        else
          u8g2.print("  Unidad: ");
        u8g2.print(unidadMetros ? "metros" : "pies");
        u8g2.setCursor(0, 36);
        if (menuOpcion == 1)
          u8g2.print("> Brillo: ");
        else
          u8g2.print("  Brillo: ");
        u8g2.print(brilloPantalla);
        u8g2.setCursor(0, 48);
        if (menuOpcion == 2)
          u8g2.print("> Altura: ");
        else
          u8g2.print("  Altura: ");
        switch(altFormat) {
          case 0: u8g2.print("normal"); break;
          case 1: u8g2.print("1 decimal"); break;
          case 2: u8g2.print("2 decimales"); break;
          case 3: u8g2.print("3 decimales"); break;
        }
        u8g2.setCursor(0, 60);
        if (menuOpcion == 3)
          u8g2.print("> Bateria");
        else
          u8g2.print("  Bateria");
        u8g2.sendBuffer();
      }
    }
  } else {
    // Modo normal: pantalla principal muestra altitud en grande y porcentaje de batería en la esquina superior derecha.
    float altitudCorrigida = sensorOk ? (altitudActual - altitudReferencia) : 0;
    if (!unidadMetros) {
      altitudCorrigida *= 3.281;
    }
    String altDisplay;
    if (altFormat == 0) {
      altDisplay = String((long)altitudCorrigida);
    } else {
      float altScaled = altitudCorrigida * 0.001;
      float altTrunc = floor(altScaled * pow(10, altFormat)) / pow(10, altFormat);
      altDisplay = String(altTrunc, altFormat);
    }
    int lecturaADC = adc1_get_raw(ADC1_CHANNEL_1);
    float voltajeADC = (lecturaADC / 4095.0) * 2.6;
    float v_adc = voltajeADC;
    float v_bat = v_adc * 2.0; // Sin factor de corrección
    int porcentajeBateria = cachedBatteryPercentage; // Usar valor actualizado cada 5 s

    pCharacteristic->setValue(altDisplay.c_str());
    pCharacteristic->notify();

    if (pantallaEncendida) {
      u8g2.clearBuffer();
      // Usar una fuente ligeramente más pequeña para la altitud principal
      u8g2.setFont(u8g2_font_fub20_tr);
      String unitStr = (unidadMetros ? " m" : " ft");
      String fullAlt = altDisplay + unitStr;
      uint16_t totalWidth = u8g2.getStrWidth(fullAlt.c_str());
      uint16_t xPos = (128 - totalWidth) / 2;
      u8g2.setCursor(xPos, 40);
      u8g2.print(fullAlt);
      // Mostrar porcentaje de batería en la esquina superior derecha.
      u8g2.setFont(u8g2_font_ncenB08_tr);
      String batStr = String(porcentajeBateria) + "%";
      uint16_t batWidth = u8g2.getStrWidth(batStr.c_str());
      u8g2.setCursor(128 - batWidth - 2, 12);
      u8g2.print(batStr);
      u8g2.sendBuffer();
    }
  }
  delay(100);
}
