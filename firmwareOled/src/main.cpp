#include <Arduino.h>
#include <Wire.h>
#include <U8g2lib.h>
#include <Adafruit_BMP3XX.h>
#include <NimBLEDevice.h>
#include <NimBLEServer.h>
#include <NimBLEUtils.h>
#include <NimBLE2904.h>
#include <driver/adc.h>
#include <math.h>
#include <esp_task_wdt.h>
#include <Preferences.h>
#include <Update.h>

// NVS
Preferences prefs;

// -------------------------
// Pines y Constantes
// -------------------------
#define SDA_PIN 4
#define SCL_PIN 5

#define OLED_ADDR 0x3C
#define BMP_ADDR  0x77

#define BATTERY_PIN 1  

// Pines de botones
#define BUTTON_ALTITUDE 6   // Reinicia altitud o cicla opciones en menú/alerta
#define BUTTON_OLED     8   // Apaga/reactiva la pantalla o confirma selección en menú/alerta
#define BUTTON_MENU     7   // Abre/cierra el menú

// BLE – UUIDs para altímetro y DFU
#define SERVICE_UUID        "4fafc200-1fb5-459e-8fcc-c5c9c331914b"  // Servicio principal para altímetro
#define CHARACTERISTIC_UUID "beb5483e-36e1-4688-b7f5-ea07361b26a8"  // Característica para altitud

#define DFU_SERVICE_UUID         "e3c0f200-3b0b-4253-9f53-3a351d8a146e"  // Servicio DFU OTA
#define DFU_CHARACTERISTIC_UUID  "e3c0f200-3b0b-4253-9f53-3a351d8a146e"  // Característica para recibir firmware

// BLE – UUIDs para actualización de usuario
#define USERNAME_SERVICE_UUID        "12345678-1234-1234-1234-1234567890ab"
#define USERNAME_CHARACTERISTIC_UUID "abcd1234-ab12-cd34-ef56-1234567890ab"

// -------------------------
// Variables Globales
// -------------------------
bool pantallaEncendida = true;
bool menuActivo = false;
int menuOpcion = 0;
unsigned long menuStartTime = 0;
int brilloPantalla = 255;
bool unidadMetros = false;   // false = pies, true = metros
int altFormat = 0;
bool batteryMenuActive = false;
bool bleActivo = true;
unsigned long lastBatteryUpdate = 0;
const unsigned long batteryUpdateInterval = 5000;
int cachedBatteryPercentage = 0;
String usuarioActual = "";

const int TOTAL_OPCIONES = 7;
const int OPCIONES_POR_PAGINA = 3;

bool inversionActiva = true;  // Estado de inversión de pantalla

const unsigned long TIMEOUT_OPTIONS[] = {0, 60000, 120000, 300000};
const int NUM_TIMEOUT_OPTIONS = 4;
int ahorroTimeoutOption = 0;
unsigned long ahorroTimeoutMs = TIMEOUT_OPTIONS[ahorroTimeoutOption];

float lastAltForAhorro = 0;
unsigned long lastAltChangeTime = 0;
const float ALT_CHANGE_THRESHOLD = 1;

bool startupDone = false;
unsigned long startupStartTime = 0;
const unsigned long startupCountdownTime = 5000;  // 5 segundos

// OLED (128x64)
U8G2_SSD1306_128X64_NONAME_F_HW_I2C u8g2(U8G2_R0, U8X8_PIN_NONE);

// Sensor BMP390L
Adafruit_BMP3XX bmp;
float altitudReferencia = 0.0;

// BLE – Variables globales NimBLE
NimBLECharacteristic *pCharacteristic = nullptr;
bool updateStarted = false;
bool deviceConnected = false;

// -------------------------
// Funciones de configuración no volátil (NVS)
// -------------------------
void loadConfig() {
  prefs.begin("config", false);
  unidadMetros   = prefs.getBool("unit", false);
  brilloPantalla = prefs.getInt("brillo", 255);
  altFormat      = prefs.getInt("altFormat", 0);
  ahorroTimeoutOption = prefs.getInt("ahorro", 0);
  ahorroTimeoutMs = TIMEOUT_OPTIONS[ahorroTimeoutOption];
  inversionActiva = prefs.getBool("invert", true);
  prefs.end();
}

void saveConfig() {
  prefs.begin("config", false);
  prefs.putBool("unit", unidadMetros);
  prefs.putInt("brillo", brilloPantalla);
  prefs.putInt("altFormat", altFormat);
  prefs.putInt("ahorro", ahorroTimeoutOption);
  prefs.putBool("invert", inversionActiva);
  prefs.end();
}

// -------------------------
// Callbacks de BLE
// -------------------------
class MyServerCallbacks: public NimBLEServerCallbacks {
  void onConnect(NimBLEServer* pServer, ble_gap_conn_desc* desc) { 
    deviceConnected = true;
    Serial.println("[BLE] Cliente conectado");
  }
  void onDisconnect(NimBLEServer* pServer, ble_gap_conn_desc* desc) { 
    deviceConnected = false;
    Serial.println("[BLE] Cliente desconectado");
    pServer->getAdvertising()->start();
    Serial.println("[BLE] Publicidad reiniciada");
  }
};

class DFUCallbacks : public NimBLECharacteristicCallbacks {
public:
  DFUCallbacks() {
    Serial.println("[DFU] DFUCallbacks instanciado");
  }
  void onWrite(NimBLECharacteristic *pCharacteristic, NimBLEConnInfo &connInfo) {
    Serial.println("[DFU] onWrite personalizado llamado");
    esp_task_wdt_reset();
    std::string rxValue = pCharacteristic->getValue();
    Serial.print("[DFU] Recibido chunk, tamaño: ");
    Serial.println(rxValue.size());

    if (rxValue == "EOF") {
      Serial.println("[DFU] Marcador EOF recibido, finalizando actualización");
      if (Update.end(true)) {
        Serial.println("[DFU] Actualización completa. Reiniciando...");
        ESP.restart();
      } else {
        Serial.println("[DFU] Error finalizando la actualización");
      }
      return;
    }

    if (!updateStarted) {
      Serial.println("[DFU] Iniciando actualización OTA");
      if (!Update.begin(UPDATE_SIZE_UNKNOWN)) {
        Serial.println("[DFU] Update.begin() falló");
        return;
      }
      updateStarted = true;
    }

    size_t written = Update.write((uint8_t*)rxValue.data(), rxValue.length());
    if (written != rxValue.length()) {
      Serial.println("[DFU] Error escribiendo el chunk");
    } else {
      Serial.print("[DFU] Chunk escrito, longitud: ");
      Serial.println(written);
    }
    delay(1);
    yield();
    esp_task_wdt_reset();
  }
};

class UsernameCallbacks : public NimBLECharacteristicCallbacks {
public:
  UsernameCallbacks() {
    Serial.println("[USERNAME] Callbacks instanciado");
  }
  // Cuando se escribe el nombre de usuario se guarda en NVS
  void onWrite(NimBLECharacteristic *pCharacteristic, NimBLEConnInfo &connInfo) {
    std::string rxValue = pCharacteristic->getValue();
    Serial.print("[USERNAME] Recibido, tamaño: ");
    Serial.println(rxValue.size());
    usuarioActual = String(rxValue.c_str());
    Serial.print("[USERNAME] Actualizado a: ");
    Serial.println(usuarioActual);
    prefs.begin("config", false);
    prefs.putString("user", usuarioActual);
    prefs.end();
  }
};

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
    float voltajeADC = (lecturaADC / 4095.0) * 2.6;
    float factorCorreccion = 1.238;
    float v_adc = voltajeADC;
    float v_bat = v_adc * 2.0 * factorCorreccion;
    cachedBatteryPercentage = calcularPorcentajeBateria(v_bat);
    lastBatteryUpdate = millis();
  }
}

void dibujarMenu() {
  int paginaActual = menuOpcion / OPCIONES_POR_PAGINA;
  int totalPaginas = (TOTAL_OPCIONES + OPCIONES_POR_PAGINA - 1) / OPCIONES_POR_PAGINA;
  int inicio = paginaActual * OPCIONES_POR_PAGINA;
  int fin = inicio + OPCIONES_POR_PAGINA;
  if (fin > TOTAL_OPCIONES) fin = TOTAL_OPCIONES;

  u8g2.clearBuffer();
  u8g2.setFont(u8g2_font_ncenB08_tr);
  u8g2.setCursor(0, 12);
  u8g2.print("MENU:");
  
  for (int i = inicio; i < fin; i++) {
    int y = 24 + (i - inicio) * 12;
    u8g2.setCursor(0, y);
    if (i == menuOpcion) {
      u8g2.print("> ");
    } else {
      u8g2.print("  ");
    }
    switch(i) {
      case 0:
        u8g2.print("Unidad: ");
        u8g2.print(unidadMetros ? "metros" : "pies");
        break;
      case 1:
        u8g2.print("Brillo: ");
        u8g2.print(brilloPantalla);
        break;
      case 2:
        u8g2.print("Altura: ");
        switch (altFormat) {
          case 0: u8g2.print("normal"); break;
          case 1: u8g2.print("1 decimal"); break;
          case 2: u8g2.print("2 decimales"); break;
          case 3: u8g2.print("3 decimales"); break;
        }
        break;
      case 3:
        u8g2.print("Bateria");
        break;
      case 4:
        u8g2.print("BL: ");
        u8g2.print(bleActivo ? "ON" : "OFF");
        break;
      case 5:
        u8g2.print("Invertir: ");
        u8g2.print(inversionActiva ? "ON" : "OFF");
        break;
      case 6:
        u8g2.print("Ahorro: ");
        if (ahorroTimeoutMs == 0)
          u8g2.print("OFF");
        else
          u8g2.print(String(ahorroTimeoutMs / 60000) + " min");
        break;
    }
  }
  
  u8g2.setCursor(100, 63);
  u8g2.print(String(paginaActual + 1) + "/" + String(totalPaginas));
  if (paginaActual > 0) {
    u8g2.setCursor(0, 63);
    u8g2.print("<");
  }
  if (paginaActual < totalPaginas - 1) {
    u8g2.setCursor(120, 63);
    u8g2.print(">");
  }
  u8g2.sendBuffer();
}

void mostrarCuentaRegresiva() {
  if (startupStartTime == 0) {
    startupStartTime = millis();
  }
  unsigned long elapsed = millis() - startupStartTime;
  int secondsLeft = 5 - (elapsed / 1000);
  if (secondsLeft < 0) secondsLeft = 0;
  
  u8g2.clearBuffer();
  u8g2.setFont(u8g2_font_fub30_tr);
  String cuenta = String(secondsLeft);
  uint16_t strWidth = u8g2.getStrWidth(cuenta.c_str());
  int xPos = (128 - strWidth) / 2;
  int yPos = 40;
  u8g2.setCursor(xPos, yPos);
  u8g2.print(cuenta);
  
  u8g2.setFont(u8g2_font_ncenB08_tr);
  u8g2.setCursor(10, 60);
  u8g2.print("Calibrando...");
  u8g2.sendBuffer();
  
  if (elapsed >= startupCountdownTime) {
    startupDone = true;
  }
  delay(100);
}

// -------------------------
// Configuración BLE con NimBLE (Altímetro, DFU OTA y Username)
// -------------------------
void setupBLE() {
  Serial.println("Inicializando BLE con NimBLE...");
  NimBLEDevice::init("ESP32-FIR");
  NimBLEDevice::setSecurityAuth(false, false, false);
  NimBLEServer *pServer = NimBLEDevice::createServer();
  pServer->setCallbacks(new MyServerCallbacks());
  
  // Servicio principal para altímetro
  Serial.println("Creando servicio principal BLE...");
  NimBLEService *pService = pServer->createService(SERVICE_UUID);
  pCharacteristic = pService->createCharacteristic(
    CHARACTERISTIC_UUID,
    NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::WRITE | NIMBLE_PROPERTY::NOTIFY
  );
  pService->start();
  Serial.println("Servicio principal BLE iniciado");
  
  // Servicio DFU OTA para actualización
  Serial.println("Creando servicio DFU OTA...");
  NimBLEService *pDFUService = pServer->createService(DFU_SERVICE_UUID);
  NimBLECharacteristic *pDFUCharacteristic = pDFUService->createCharacteristic(
    DFU_CHARACTERISTIC_UUID,
    NIMBLE_PROPERTY::WRITE | NIMBLE_PROPERTY::WRITE_NR
  );
  pDFUCharacteristic->setCallbacks(new DFUCallbacks());
  pDFUService->start();
  Serial.println("Servicio DFU OTA iniciado");
  
  // Servicio para actualización de usuario
  Serial.println("Creando servicio de actualización de usuario...");
  NimBLEService *pUsernameService = pServer->createService(USERNAME_SERVICE_UUID);
  NimBLECharacteristic *pUsernameCharacteristic = pUsernameService->createCharacteristic(
    USERNAME_CHARACTERISTIC_UUID,
    NIMBLE_PROPERTY::WRITE | NIMBLE_PROPERTY::WRITE_NR
  );
  pUsernameCharacteristic->setCallbacks(new UsernameCallbacks());
  pUsernameService->start();
  Serial.println("Servicio de usuario iniciado");
  
  // Configuración de advertising:
  NimBLEAdvertising *pAdvertising = NimBLEDevice::getAdvertising();
  NimBLEAdvertisementData advData;
  advData.setName("ESP32-FIR");
  advData.addServiceUUID(SERVICE_UUID);
  pAdvertising->setAdvertisementData(advData);
  
  NimBLEAdvertisementData scanData;
  scanData.setName("ESP32-FIR");
  scanData.addServiceUUID(DFU_SERVICE_UUID);
  scanData.addServiceUUID(USERNAME_SERVICE_UUID);
  pAdvertising->setScanResponseData(scanData);
  
  pAdvertising->start();
  Serial.println("BLE advertising iniciado");
}

void toggleBLE() {
  bleActivo = !bleActivo;
  NimBLEAdvertising *pAdvertising = NimBLEDevice::getAdvertising();
  if (!bleActivo) {
    pAdvertising->stop();
    Serial.println("BLE advertising stopped");
  } else {
    pAdvertising->start();
    Serial.println("BLE advertising started");
  }
}

// -------------------------
// Setup
// -------------------------
void setup() {
  Serial.begin(115200);
  delay(1000);
  Serial.println("Setup iniciado");
  
  Wire.begin(SDA_PIN, SCL_PIN);
  
  // Cargar configuración guardada (o usar valores por defecto)
  loadConfig();
  
  prefs.begin("config", false);
  usuarioActual = prefs.getString("user", "default_user");
  prefs.end();
  Serial.print("Usuario cargado desde NVS: ");
  Serial.println(usuarioActual);
  
  setupBLE();
  
  u8g2.begin();
  // Se selecciona el modo de inversión según la configuración almacenada
  if (inversionActiva) {
    u8g2.sendF("c", 0xA7);
    Serial.println("Display iniciado en modo invertido.");
  } else {
    u8g2.sendF("c", 0xA6);
    Serial.println("Display iniciado en modo normal.");
  }
  u8g2.setPowerSave(false);
  u8g2.setContrast(brilloPantalla);
  
  if (!bmp.begin_I2C(BMP_ADDR)) {
    Serial.println("¡Sensor BMP390L no encontrado!");
    while (1);
  }
  bmp.setTemperatureOversampling(BMP3_OVERSAMPLING_4X);
  bmp.setPressureOversampling(BMP3_OVERSAMPLING_8X);
  bmp.setIIRFilterCoeff(BMP3_IIR_FILTER_COEFF_7);
  bmp.setOutputDataRate(BMP3_ODR_50_HZ);
  
  pinMode(BUTTON_ALTITUDE, INPUT_PULLUP);
  pinMode(BUTTON_OLED, INPUT_PULLUP);
  pinMode(BUTTON_MENU, INPUT_PULLUP);
  
  adc1_config_width(ADC_WIDTH_BIT_12);
  adc1_config_channel_atten(ADC1_CHANNEL_1, ADC_ATTEN_DB_12);
  
  if (bmp.performReading()) {
    lastAltForAhorro = bmp.readAltitude(1013.25);
  }
  lastAltChangeTime = millis();
  
  Serial.println("Setup completado");
}

// -------------------------
// Loop
// -------------------------
void loop() {
  if (!startupDone) {
    mostrarCuentaRegresiva();
    return;
  }
  
  bool sensorOk = bmp.performReading();
  float altitudActual = sensorOk ? bmp.readAltitude(1013.25) : 0;
  
  // Modo ahorro: verificar inactividad en la altitud
  if (!menuActivo && sensorOk && ahorroTimeoutMs > 0) {
    if (fabs(altitudActual - lastAltForAhorro) > ALT_CHANGE_THRESHOLD) {
      lastAltForAhorro = altitudActual;
      lastAltChangeTime = millis();
    } else {
      if ((millis() - lastAltChangeTime) >= ahorroTimeoutMs && pantallaEncendida) {
        u8g2.setPowerSave(true);
        pantallaEncendida = false;
        Serial.println("Modo ahorro: Pantalla suspendida por inactividad.");
      }
    }
  }
  
  // Gestión de botones para el menú
  if (digitalRead(BUTTON_MENU) == LOW) {
    menuActivo = !menuActivo;
    if (menuActivo) {
      menuOpcion = 0;
      batteryMenuActive = false;
      menuStartTime = millis();
      Serial.println("Menu abierto.");
    } else {
      Serial.println("Menu cerrado.");
    }
    delay(50);
  }
  
  if (digitalRead(BUTTON_ALTITUDE) == LOW) {
    if (menuActivo) {
      menuOpcion = (menuOpcion + 1) % TOTAL_OPCIONES;
      if (menuOpcion != 3) batteryMenuActive = false;
      menuStartTime = millis();
      Serial.print("Opción del menú cambiada a: ");
      switch (menuOpcion) {
        case 0: Serial.println("Unidad"); break;
        case 1: Serial.println("Brillo"); break;
        case 2: Serial.println("Formato Altura"); break;
        case 3: Serial.println("Batería"); break;
        case 4: Serial.println("BL"); break;
        case 5: Serial.println("Invertir"); break;
        case 6: Serial.println("Ahorro"); break;
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
          saveConfig();
          break;
        case 1:
          brilloPantalla += 50;
          if (brilloPantalla > 255) brilloPantalla = 50;
          u8g2.setContrast(brilloPantalla);
          Serial.print("Brillo cambiado a: ");
          Serial.println(brilloPantalla);
          saveConfig();
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
          saveConfig();
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
        case 5:
          inversionActiva = !inversionActiva;
          if (inversionActiva) {
            u8g2.sendF("c", 0xA7);
            Serial.println("Display invertido.");
          } else {
            u8g2.sendF("c", 0xA6);
            Serial.println("Display en modo normal.");
          }
          saveConfig();
          break;
        case 6:
          ahorroTimeoutOption = (ahorroTimeoutOption + 1) % NUM_TIMEOUT_OPTIONS;
          ahorroTimeoutMs = TIMEOUT_OPTIONS[ahorroTimeoutOption];
          Serial.print("Modo ahorro configurado a: ");
          if (ahorroTimeoutMs == 0)
            Serial.println("OFF");
          else
            Serial.println(String(ahorroTimeoutMs / 60000) + " min");
          saveConfig();
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
        lastAltChangeTime = millis();
        if (bmp.performReading()) {
          lastAltForAhorro = bmp.readAltitude(1013.25);
        }
        Serial.println("Pantalla activada y temporizador reiniciado.");
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
  
  // Si estamos en menú, mostramos las opciones
  if (menuActivo) {
    if (batteryMenuActive) {
      if (pantallaEncendida) {
        int lecturaADC = adc1_get_raw(ADC1_CHANNEL_1);
        float voltajeADC = (lecturaADC / 4095.0) * 2.6;
        float factorCorreccion = 1.238;
        float v_adc = voltajeADC;
        float v_bat = v_adc * 2.0 * factorCorreccion;
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
        dibujarMenu();
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
    float v_bat = v_adc * 2.0;
    int porcentajeBateria = cachedBatteryPercentage;
  
    // Actualizamos la característica principal (altímetro) con el valor de altitud
    pCharacteristic->setValue(altDisplay.c_str());
    pCharacteristic->notify();
  
    if (pantallaEncendida) {
      u8g2.clearBuffer();
      u8g2.setFont(u8g2_font_ncenB08_tr);
      u8g2.setCursor(2,12);
      u8g2.print(unidadMetros ? "M" : "FT");
      
      u8g2.setFont(u8g2_font_ncenB08_tr);
      String bleStr = "BL: " + String(bleActivo ? "ON" : "OFF");
      uint16_t bleWidth = u8g2.getStrWidth(bleStr.c_str());
      int xPosBle = (128 - bleWidth) / 2;
      u8g2.setCursor(xPosBle,12);
      u8g2.print(bleStr);
      
      u8g2.setFont(u8g2_font_ncenB08_tr);
      String batStr = String(porcentajeBateria) + "%";
      uint16_t batWidth = u8g2.getStrWidth(batStr.c_str());
      u8g2.setCursor(128 - batWidth - 2,12);
      u8g2.print(batStr);
      
      u8g2.setFont(u8g2_font_fub30_tr);
      uint16_t yPosAlt = 50;
      String altStr = altDisplay;
      uint16_t altWidth = u8g2.getStrWidth(altStr.c_str());
      int16_t xPosAlt = (128 - altWidth) / 2;
      if (xPosAlt < 0) xPosAlt = 0;
      u8g2.setCursor(xPosAlt, yPosAlt);
      u8g2.print(altStr);
      u8g2.drawHLine(0,15,128);
      u8g2.drawHLine(0,52,128);
      u8g2.drawHLine(0,0,128);
      u8g2.drawHLine(0,63,128);
      u8g2.drawVLine(0,0,64);
      u8g2.drawVLine(127,0,64);
      
      u8g2.setFont(u8g2_font_ncenB08_tr);
      String user = usuarioActual;
      uint16_t userWidth = u8g2.getStrWidth(user.c_str());
      int16_t xPosUser = (128 - userWidth) / 2;
      if (xPosUser < 0) xPosUser = 0;
      u8g2.setCursor(xPosUser,62);
      u8g2.print(user);
      
      u8g2.sendBuffer();
    }
  }
  delay(101);
}
