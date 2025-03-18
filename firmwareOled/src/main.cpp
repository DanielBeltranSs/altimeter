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

// Funciones OTA
#include "esp_ota_ops.h"
#include "esp_partition.h"

// -------------------------
// Pines y Constantes
// -------------------------

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
#define BUTTON_OLED     8  // Suspende/reactiva pantalla en modo normal; en menú: aplica opción actual
#define BUTTON_MENU     7  // Abre/cierra el menú

// BLE – Servicio principal y característica
#define SERVICE_UUID        "4fafc201-1fb5-459e-8fcc-c5c9c331914b"
#define CHARACTERISTIC_UUID "beb5483e-36e1-4688-b7f5-ea07361b26a8"

// BLE – Servicio DFU (para OTA)
#define DFU_SERVICE_UUID         "e3c0f200-3b0b-4253-9f53-3a351d8a146e"
#define DFU_CHARACTERISTIC_UUID  "e3c0f201-3b0b-4253-9f53-3a351d8a146e"

// -------------------------
// Variables Globales
// -------------------------

bool pantallaEncendida = true;
bool menuActivo = false;
// Opciones del menú:
// 0: Unidad, 1: Brillo, 2: Formato Altura, 3: Batería, 4: BLE
int menuOpcion = 0;
unsigned long menuStartTime = 0;
int brilloPantalla = 255;
bool unidadMetros = false;  // false = pies, true = metros
// altFormat: 0 = normal, 1..3 = decimales
int altFormat = 0;
// Variable para vista detallada de batería en el menú
bool batteryMenuActive = false;

// Estado del BLE (true: ON, false: OFF)
bool bleActivo = true;

// Variables para actualización de batería (cada 5 s)
unsigned long lastBatteryUpdate = 0;
const unsigned long batteryUpdateInterval = 5000;
int cachedBatteryPercentage = 0;

// -------------------------
// Inicialización de Hardware
// -------------------------

// OLED (128x64)
U8G2_SH1106_128X64_NONAME_F_HW_I2C u8g2(U8G2_R0, U8X8_PIN_NONE);

// Sensor BMP390L
Adafruit_BMP3XX bmp;

// Altitud de referencia (para altitud relativa)
float altitudReferencia = 0.0;

// BLE – Característica principal
NimBLECharacteristic *pCharacteristic = nullptr;

// -------------------------
// Funciones Auxiliares
// -------------------------

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

void updateBatteryReading() {
  if (millis() - lastBatteryUpdate >= batteryUpdateInterval) {
    int lecturaADC = adc1_get_raw(ADC1_CHANNEL_1);
    float voltajeADC = (lecturaADC / 4095.0) * 2.6;  // Voltaje en V
    float v_adc = voltajeADC;
    float v_bat = v_adc * 2.0;
    cachedBatteryPercentage = calcularPorcentajeBateria(v_bat);
    lastBatteryUpdate = millis();
  }
}

// -------------------------
// DFU OTA vía BLE
// -------------------------

esp_ota_handle_t ota_handle = 0;
const esp_partition_t* update_partition = NULL;
bool dfu_in_progress = false;

class MyDFUCallbacks : public NimBLECharacteristicCallbacks {
  void onWrite(NimBLECharacteristic *pCharacteristic) {
    std::string value = pCharacteristic->getValue();
    if (value.size() > 0) {
      if (!dfu_in_progress) {
        update_partition = esp_ota_get_next_update_partition(NULL);
        if (update_partition == NULL) {
          Serial.println("No hay partición OTA disponible");
          return;
        }
        esp_err_t err = esp_ota_begin(update_partition, OTA_SIZE_UNKNOWN, &ota_handle);
        if (err != ESP_OK) {
          Serial.println("esp_ota_begin falló");
          return;
        }
        dfu_in_progress = true;
        Serial.println("Actualización DFU iniciada");
      }
      if (value == "EOF") {
        esp_err_t err = esp_ota_end(ota_handle);
        if (err != ESP_OK) {
          Serial.println("esp_ota_end falló");
        } else {
          err = esp_ota_set_boot_partition(update_partition);
          if (err != ESP_OK) {
            Serial.println("esp_ota_set_boot_partition falló");
          } else {
            Serial.println("Actualización OTA exitosa, reiniciando...");
            esp_restart();
          }
        }
      } else {
        esp_err_t err = esp_ota_write(ota_handle, value.data(), value.size());
        if (err != ESP_OK) {
          Serial.println("esp_ota_write falló");
        } else {
          Serial.print("Chunk recibido, tamaño: ");
          Serial.println(value.size());
        }
      }
    }
  }
};

void setupDFU() {
  NimBLEServer* pServer = NimBLEDevice::getServer();
  if (!pServer) {
    Serial.println("Servidor BLE no disponible para DFU");
    return;
  }
  NimBLEService *pDFUService = pServer->createService(DFU_SERVICE_UUID);
  NimBLECharacteristic *pDFUCharacteristic = pDFUService->createCharacteristic(
      DFU_CHARACTERISTIC_UUID,
      NIMBLE_PROPERTY::WRITE
  );
  pDFUCharacteristic->setCallbacks(new MyDFUCallbacks());
  pDFUService->start();
  NimBLEDevice::getAdvertising()->addServiceUUID(DFU_SERVICE_UUID);
  Serial.println("Servicio DFU iniciado");
}

// -------------------------
// Configurar BLE (servicio principal y DFU)
// -------------------------
void setupBLE() {
  Serial.println("Inicializando BLE...");
  NimBLEDevice::init("");
  NimBLEDevice::setDeviceName("ESP32-Altimetro-ota");
  NimBLEServer *pServer = NimBLEDevice::createServer();
  Serial.println("Servidor BLE creado");
  NimBLEService *pService = pServer->createService(SERVICE_UUID);
  Serial.println("Servicio principal BLE creado");
  pCharacteristic = pService->createCharacteristic(
      CHARACTERISTIC_UUID,
      NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::NOTIFY
  );
  Serial.println("Característica principal BLE creada");
  pService->start();
  Serial.println("Servicio principal BLE iniciado");
  setupDFU();
  NimBLEAdvertising *pAdvertising = NimBLEDevice::getAdvertising();
  NimBLEAdvertisementData advData;
  advData.setName("ESP32-Altimetro-ota");
  advData.setCompleteServices(pService->getUUID());
  pAdvertising->setAdvertisementData(advData);
  NimBLEAdvertisementData scanData;
  scanData.setName("ESP32-Altimetro");
  pAdvertising->setScanResponseData(scanData);
  pAdvertising->start();
  Serial.println("BLE advertising iniciado");
}

// Función para apagar/encender BLE mediante publicidad
void toggleBLE() {
  bleActivo = !bleActivo;
  NimBLEAdvertising* adv = NimBLEDevice::getAdvertising();
  if (!bleActivo) {
    adv->stop();
    Serial.println("BLE advertising stopped");
  } else {
    adv->start();
    Serial.println("BLE advertising started");
  }
}

// -------------------------
// SETUP
// -------------------------
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

  pinMode(BUTTON_ALTITUDE, INPUT_PULLUP);
  pinMode(BUTTON_OLED, INPUT_PULLUP);
  pinMode(BUTTON_MENU, INPUT_PULLUP);

  adc1_config_width(ADC_WIDTH_BIT_12);
  adc1_config_channel_atten(ADC1_CHANNEL_1, ADC_ATTEN_DB_12);

  Serial.println("Setup completado");
}

// -------------------------
// LOOP
// -------------------------
void loop() {
  bool sensorOk = bmp.performReading();
  float altitudActual = sensorOk ? bmp.readAltitude(1013.25) : 0;

  // Gestión de botones
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

  if (digitalRead(BUTTON_ALTITUDE) == LOW) {
    if (menuActivo) {
      menuOpcion = (menuOpcion + 1) % 5; // 5 opciones: 0..4
      if (menuOpcion != 3) batteryMenuActive = false;
      menuStartTime = millis();
      Serial.print("Opción del menú cambiada a: ");
      switch (menuOpcion) {
        case 0: Serial.println("Unidad"); break;
        case 1: Serial.println("Brillo"); break;
        case 2: Serial.println("Formato Altura"); break;
        case 3: Serial.println("Batería"); break;
        case 4: Serial.println("BL"); break;
      }
    } else {
      altitudReferencia = altitudActual;
      Serial.println("Altitud reiniciada a cero.");
    }
    delay(50);
  }

  if (digitalRead(BUTTON_OLED) == LOW) {
    if (menuActivo) {
      switch (menuOpcion) {
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
          switch (altFormat) {
            case 0: Serial.println("normal"); break;
            case 1: Serial.println("1 decimal"); break;
            case 2: Serial.println("2 decimales"); break;
            case 3: Serial.println("3 decimales"); break;
          }
          break;
        case 3:
          batteryMenuActive = !batteryMenuActive;
          Serial.print("Vista Batería en menú: ");
          Serial.println(batteryMenuActive ? "ON" : "OFF");
          break;
        case 4:
          toggleBLE();
          Serial.print("BLE ahora está: ");
          Serial.println(bleActivo ? "ON" : "OFF");
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

  if (menuActivo && (millis() - menuStartTime > 7000)) {
    menuActivo = false;
    batteryMenuActive = false;
    Serial.println("Menú cerrado por timeout.");
  }

  updateBatteryReading();

  // Visualización y envío vía BLE
  if (menuActivo) {
    if (batteryMenuActive) {
      if (pantallaEncendida) {
        int lecturaADC = adc1_get_raw(ADC1_CHANNEL_1);
        float voltajeADC = (lecturaADC / 4095.0) * 2.6;
        float v_adc = voltajeADC;
        float v_bat = v_adc * 2.0; // Sin factor de corrección
        u8g2.clearBuffer();
        u8g2.setFont(u8g2_font_ncenB08_tr);
        u8g2.setCursor(0,12);
        u8g2.print("BATERIA:");
        u8g2.setCursor(0,26);
        u8g2.print("ADC: ");
        u8g2.setCursor(50,26);
        u8g2.print(lecturaADC);
        u8g2.setCursor(0,40);
        u8g2.print("V_ADC: ");
        u8g2.setCursor(50,40);
        u8g2.print(v_adc,2);
        u8g2.print("V");
        u8g2.setCursor(0,54);
        u8g2.print("V_Bat: ");
        u8g2.setCursor(50,54);
        u8g2.print(v_bat,2);
        u8g2.sendBuffer();
      }
    } else {
      if (pantallaEncendida) {
        u8g2.clearBuffer();
        u8g2.setFont(u8g2_font_ncenB08_tr);
        u8g2.setCursor(0,12);
        u8g2.print("MENU:");
        u8g2.setCursor(0,24);
        if (menuOpcion == 0)
          u8g2.print("> Unidad: ");
        else
          u8g2.print("  Unidad: ");
        u8g2.print(unidadMetros ? "metros" : "pies");
  
        u8g2.setCursor(0,36);
        if (menuOpcion == 1)
          u8g2.print("> Brillo: ");
        else
          u8g2.print("  Brillo: ");
        u8g2.print(brilloPantalla);
  
        u8g2.setCursor(0,48);
        if (menuOpcion == 2)
          u8g2.print("> Altura: ");
        else
          u8g2.print("  Altura: ");
        switch (altFormat) {
          case 0: u8g2.print("normal"); break;
          case 1: u8g2.print("1 decimal"); break;
          case 2: u8g2.print("2 decimales"); break;
          case 3: u8g2.print("3 decimales"); break;
        }
  
        u8g2.setCursor(0,60);
        if (menuOpcion == 3)
          u8g2.print("> Bateria");
        else
          u8g2.print("  Bateria");
  
        // Mostrar opción BLE en la misma línea, a la derecha:
        u8g2.setCursor(60,60);
        if (menuOpcion == 4)
          u8g2.print("> BL: " + String(bleActivo ? "ON" : "OFF"));
        else
          u8g2.print("  BL: " + String(bleActivo ? "ON" : "OFF"));
  
        u8g2.sendBuffer();
      }
    }
  } else {
    float altitudCorrigida = sensorOk ? (altitudActual - altitudReferencia) : 0;
    if (!unidadMetros)
      altitudCorrigida *= 3.281;
    String altDisplay;
    if (altFormat == 0)
      altDisplay = String((long)altitudCorrigida);
    else {
      float altScaled = altitudCorrigida * 0.001;
      float altTrunc = floor(altScaled * pow(10, altFormat)) / pow(10, altFormat);
      altDisplay = String(altTrunc, altFormat);
    }
    int lecturaADC = adc1_get_raw(ADC1_CHANNEL_1);
    float voltajeADC = (lecturaADC / 4095.0) * 2.6;
    float v_adc = voltajeADC;
    float v_bat = v_adc * 2.0; // Sin factor de corrección
    int porcentajeBateria = cachedBatteryPercentage;
  
    pCharacteristic->setValue(altDisplay.c_str());
    pCharacteristic->notify();
  
    if (pantallaEncendida) {
      u8g2.clearBuffer();
      // Imprimir unidad en la esquina superior izquierda
      u8g2.setFont(u8g2_font_ncenB08_tr);
      u8g2.setCursor(2,12);
      u8g2.print(unidadMetros ? "M" : "FT");
      
      // Agregar estatus BLE centrado en la parte superior:
      u8g2.setFont(u8g2_font_ncenB08_tr);
      String bleStr = "BL: " + String(bleActivo ? "ON" : "OFF");
      uint16_t bleWidth = u8g2.getStrWidth(bleStr.c_str());
      int xPosBle = (128 - bleWidth) / 2;
      u8g2.setCursor(xPosBle,12);
      u8g2.print(bleStr);
      
      // Mostrar porcentaje de batería en la esquina superior derecha
      u8g2.setFont(u8g2_font_ncenB08_tr);
      String batStr = String(porcentajeBateria) + "%";
      uint16_t batWidth = u8g2.getStrWidth(batStr.c_str());
      u8g2.setCursor(128 - batWidth - 2,12);
      u8g2.print(batStr);
      
      // Altitud grande centrada con fuente fub25
      u8g2.setFont(u8g2_font_fub30_tr);
      uint16_t yPosAlt = 50;
      String altStr = "13000"; // Para altitud real, usar altDisplay
      uint16_t altWidth = u8g2.getStrWidth(altStr.c_str());
      int16_t xPosAlt = (128 - altWidth) / 2;
      if (xPosAlt < 0) xPosAlt = 0;
      u8g2.setCursor(xPosAlt, yPosAlt);
      u8g2.print(altStr);
      u8g2.drawHLine(0, 15, 128);  // Primera barra horizontal en y = 20
      u8g2.drawHLine(0, 52, 128); 
      u8g2.drawVLine(0, 0, 64);
      u8g2.drawVLine(125, 0, 64);


      
      u8g2.setFont(u8g2_font_ncenB08_tr);
      String user = "elDani"; // Para altitud real, usar altDisplay
      uint16_t UserWidth = u8g2.getStrWidth(user.c_str());
      int16_t xPosUser = (128 - UserWidth) / 2;
      if (xPosAlt < 0) xPosAlt = 0;
      u8g2.setCursor(xPosUser, 64);
      u8g2.print(user);

      u8g2.sendBuffer();
    }
  }
  delay(100);
}
