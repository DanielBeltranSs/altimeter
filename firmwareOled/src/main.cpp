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
#include <math.h>
#include "esp_ota_ops.h"
#include "esp_partition.h"
#include <mbedtls/sha256.h>

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
#define BUTTON_ALTITUDE 6  // Reinicia altitud o cicla opciones en menú/alerta
#define BUTTON_OLED     8  // Apaga/reactiva la pantalla o confirma selección en menú/alerta
#define BUTTON_MENU     7  // Abre/cierra el menú

// BLE – Servicio principal y característica
#define SERVICE_UUID        "4fafc200-1fb5-459e-8fcc-c5c9c331914b"
#define CHARACTERISTIC_UUID "beb5483e-36e1-4688-b7f5-ea07361b26a8"

// BLE – Servicio DFU (para OTA)
#define DFU_SERVICE_UUID         "e3c0f200-3b0b-4253-9f53-3a351d8a146e"
#define DFU_CHARACTERISTIC_UUID  "e3c0f200-3b0b-4253-9f53-3a351d8a146e"

// -------------------------
// Variables Globales
// -------------------------

bool pantallaEncendida = true;
bool menuActivo = false;
int menuOpcion = 0;
unsigned long menuStartTime = 0;
int brilloPantalla = 255;
bool unidadMetros = false;  // false = pies, true = metros
int altFormat = 0;
bool batteryMenuActive = false;
bool bleActivo = true;
unsigned long lastBatteryUpdate = 0;
const unsigned long batteryUpdateInterval = 5000;
int cachedBatteryPercentage = 0;
String usuarioActual = "";

// Variables para la cuenta regresiva de estabilización
bool startupDone = false;
unsigned long startupStartTime = 0;
const unsigned long startupCountdownTime = 5000;  // 5 segundos

// OLED (128x64)
U8G2_SSD1306_128X64_NONAME_F_HW_I2C u8g2(U8G2_R0, U8X8_PIN_NONE);

// Sensor BMP390L
Adafruit_BMP3XX bmp;

// Altitud de referencia (para altitud relativa)
float altitudReferencia = 0.0;

// BLE – Característica principal
NimBLECharacteristic *pCharacteristic = nullptr;

// Variables para OTA DFU
esp_ota_handle_t ota_handle = 0;
const esp_partition_t* update_partition = NULL;
bool dfu_in_progress = false;

// Variables para alerta de actualización OTA
volatile bool otaConfirmationPending = false; // Activa modo confirmación
volatile int otaConfirmationOption = 0;         // 0: Aceptar, 1: Cancelar

// -------------------------
// Variables para Validación de Integridad
// -------------------------

// Valor predefinido del hash esperado (32 bytes para SHA-256)
// En producción este valor se debe definir o recibir de forma segura.
uint8_t hashEsperado[32] = { /* Rellenar con el hash esperado */ };
std::string firmaDigitalRecibida;

// Contexto para cálculo incremental del hash
mbedtls_sha256_context sha256_ctx;
bool checksum_inicializado = false;

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
    float factorCorreccion = 1.238;
    float v_adc = voltajeADC;
    float v_bat = v_adc * 2.0 * factorCorreccion;
    cachedBatteryPercentage = calcularPorcentajeBateria(v_bat);
    lastBatteryUpdate = millis();
  }
}

// Actualiza el checksum incrementalmente con cada chunk recibido
void actualizarChecksum(const uint8_t* data, size_t len) {
  if (!checksum_inicializado) {
    mbedtls_sha256_init(&sha256_ctx);
    mbedtls_sha256_starts_ret(&sha256_ctx, 0); // 0 para SHA-256
    checksum_inicializado = true;
  }
  mbedtls_sha256_update_ret(&sha256_ctx, data, len);
}

// Calcula el hash completo de la imagen almacenada en la partición OTA
void calcularHashFirmware(const esp_partition_t* partition, uint8_t* hashCalculado) {
  const size_t bufferSize = 1024;
  uint8_t buffer[bufferSize];
  mbedtls_sha256_context ctx;
  mbedtls_sha256_init(&ctx);
  mbedtls_sha256_starts_ret(&ctx, 0);

  size_t offset = 0;
  size_t remaining = partition->size;
  while (remaining > 0) {
    size_t toRead = (remaining > bufferSize) ? bufferSize : remaining;
    esp_err_t err = esp_partition_read(partition, offset, buffer, toRead);
    if (err != ESP_OK) {
      Serial.println("Error al leer la particion para hash");
      break;
    }
    mbedtls_sha256_update_ret(&ctx, buffer, toRead);
    offset += toRead;
    remaining -= toRead;
  }
  mbedtls_sha256_finish_ret(&ctx, hashCalculado);
  mbedtls_sha256_free(&ctx);
}

// Verifica la integridad del firmware comparando el hash calculado con el esperado
bool verificarIntegridadFirmware() {
  uint8_t hashCalculado[32];
  calcularHashFirmware(update_partition, hashCalculado);
  if(memcmp(hashCalculado, hashEsperado, 32) == 0) {
    Serial.println("Integridad del firmware verificada correctamente.");
    return true;
  } else {
    Serial.println("Error: Integridad del firmware fallida.");
    return false;
  }
}

// Almacena la firma digital recibida (para verificación posterior)
void almacenarFirmaDigital(const std::string& firma) {
  firmaDigitalRecibida = firma;
  // Aquí se debe implementar la verificación de la firma digital utilizando una clave pública.
}

// -------------------------
// Función Base64 Decode
// -------------------------

// Devuelve el valor base64 para un carácter o -1 si es inválido.
int base64_char_value(char c) {
  if (c >= 'A' && c <= 'Z') return c - 'A';
  if (c >= 'a' && c <= 'z') return c - 'a' + 26;
  if (c >= '0' && c <= '9') return c - '0' + 52;
  if (c == '+') return 62;
  if (c == '/') return 63;
  return -1;
}

// Decodifica un string base64 en el buffer "out" (debe tener suficiente espacio).
// Retorna la cantidad de bytes decodificados o -1 en caso de error.
int base64_decode(const std::string &in, uint8_t *out, int out_max) {
  int in_len = in.length();
  int i = 0, j = 0;
  int value;
  uint32_t buffer = 0;
  int bits_collected = 0;
  while (i < in_len) {
    char c = in[i++];
    if (c == '=' || c == '\n' || c == '\r' || c == ' ') {
      continue;
    }
    value = base64_char_value(c);
    if (value < 0) {
      // Carácter inválido
      return -1;
    }
    buffer = (buffer << 6) | value;
    bits_collected += 6;
    if (bits_collected >= 8) {
      bits_collected -= 8;
      if (j >= out_max) return -1; // Salida insuficiente
      out[j++] = (uint8_t)((buffer >> bits_collected) & 0xFF);
    }
  }
  return j;
}

// -------------------------
// Función para dibujar el recuadro de alerta en la pantalla (para OTA)
// -------------------------
void dibujarAlertaOTA() {
  u8g2.clearBuffer();
  // Dibujar recuadro central
  int rectX = 5, rectY = 20, rectW = 118, rectH = 40;
  u8g2.drawFrame(rectX, rectY, rectW, rectH);
  u8g2.setFont(u8g2_font_ncenB08_tr);
  // Mensaje de alerta
  u8g2.setCursor(rectX + 10, rectY + 15);
  u8g2.print("Desea aceptar");
  u8g2.setCursor(rectX + 10, rectY + 28);
  u8g2.print("la actualizacion?");
  
  // Opciones
  String opcionAceptar = "Aceptar";
  String opcionCancelar = "Cancelar";
  
  // Resaltar la opción seleccionada
  if (otaConfirmationOption == 0) {
    u8g2.drawBox(rectX + 10, rectY + 32, u8g2.getStrWidth(opcionAceptar.c_str())+2, 10);
    u8g2.setDrawColor(0); // Texto en blanco sobre fondo negro
    u8g2.setCursor(rectX + 11, rectY + 41);
    u8g2.print(opcionAceptar);
    u8g2.setDrawColor(1);
    u8g2.setCursor(rectX + 60, rectY + 41);
    u8g2.print(opcionCancelar);
  } else {
    u8g2.drawBox(rectX + 60, rectY + 32, u8g2.getStrWidth(opcionCancelar.c_str())+2, 10);
    u8g2.setDrawColor(0);
    u8g2.setCursor(rectX + 60, rectY + 41);
    u8g2.print(opcionCancelar);
    u8g2.setDrawColor(1);
    u8g2.setCursor(rectX + 10, rectY + 41);
    u8g2.print(opcionAceptar);
  }
  
  u8g2.sendBuffer();
}

// -------------------------
// Función para finalizar la OTA (al aceptar la actualización)
// -------------------------
void finalizarOTA() {
  // Verificar la integridad antes de finalizar la OTA
  if (!verificarIntegridadFirmware()) {
    Serial.println("Verificacion de integridad fallida, abortando OTA.");
    esp_ota_abort(ota_handle);
    dfu_in_progress = false;
  } else {
    esp_err_t err = esp_ota_end(ota_handle);
    if (err != ESP_OK) {
      Serial.println("esp_ota_end fallo");
      return;
    }
    err = esp_ota_set_boot_partition(update_partition);
    if (err != ESP_OK) {
      Serial.println("esp_ota_set_boot_partition fallo");
    } else {
      Serial.println("Actualizacion OTA exitosa, reiniciando...");
      esp_restart();
    }
  }
  otaConfirmationPending = false;
}

// -------------------------
// Función para cancelar la OTA
// -------------------------
void cancelarOTA() {
  Serial.println("Actualizacion OTA cancelada por el usuario.");
  esp_ota_abort(ota_handle);
  dfu_in_progress = false;
  otaConfirmationPending = false;
}

// -------------------------
// Clase para DFU OTA vía BLE con validación, alerta y decodificacion Base64
// -------------------------
class MyDFUCallbacks : public NimBLECharacteristicCallbacks {
  void onWrite(NimBLECharacteristic *pCharacteristic) {
    std::string value = pCharacteristic->getValue();
    if (value.empty()) return;

    // Si viene un nombre de usuario (no inicia OTA)
    if (!dfu_in_progress && value.find("SIG:") != 0 && value != "EOF") {
      // Decodificar Base64
      int maxDecodedSize = (value.size() * 3) / 4;
      uint8_t *decodedBuffer = new uint8_t[maxDecodedSize];
      int decodedLength = base64_decode(value, decodedBuffer, maxDecodedSize);
      if (decodedLength < 0) {
        Serial.println("Error decodificando Base64");
      } else {
        usuarioActual = String((char*)decodedBuffer, decodedLength);
        menuActivo = false;
        Serial.print("Usuario recibido: ");
        Serial.println(usuarioActual);
      }
      delete[] decodedBuffer;
      return;
    }

    // --- Lógica OTA existente ---
    if (!dfu_in_progress) {
      update_partition = esp_ota_get_next_update_partition(NULL);
      if (!update_partition) {
        Serial.println("No hay particion OTA disponible");
        return;
      }
      esp_err_t err = esp_ota_begin(update_partition, OTA_SIZE_UNKNOWN, &ota_handle);
      if (err != ESP_OK) {
        Serial.println("esp_ota_begin fallo");
        return;
      }
      mbedtls_sha256_free(&sha256_ctx);
      checksum_inicializado = false;
      dfu_in_progress = true;
      Serial.println("Actualizacion DFU iniciada");
    }

    if (value.substr(0, 4) == "SIG:") {
      almacenarFirmaDigital(value.substr(4));
      Serial.println("Firma digital recibida.");
    }
    else if (value == "EOF") {
      otaConfirmationPending = true;
      Serial.println("Esperando confirmacion del usuario para aplicar la actualizacion OTA.");
    }
    else {
      int maxDecoded = (value.size() * 3) / 4;
      uint8_t *buf = new uint8_t[maxDecoded];
      int len = base64_decode(value, buf, maxDecoded);
      if (len < 0) {
        Serial.println("Error decodificando base64 OTA");
      } else {
        actualizarChecksum(buf, len);
        esp_err_t err = esp_ota_write(ota_handle, buf, len);
        if (err != ESP_OK) Serial.println("esp_ota_write fallo");
        else {
          Serial.print("Chunk OTA recibido, tamaño: ");
          Serial.println(len);
        }
      }
      delete[] buf;
    }
  }
};

// -------------------------
// Configuración de DFU y BLE
// -------------------------
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
    NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::WRITE | NIMBLE_PROPERTY::NOTIFY
  );
  pCharacteristic->setCallbacks(new MyDFUCallbacks());
  Serial.println("Caracteristica principal BLE creada");
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
// Función para mostrar la cuenta regresiva de 5 segundos
// -------------------------
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
  
  // Mensaje adicional
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
// SETUP
// -------------------------
void setup() {
  Serial.begin(115200);
  delay(1000);
  Serial.println("Setup iniciado");
  
  Wire.begin(SDA_PIN, SCL_PIN);
  setupBLE();
  
  u8g2.begin();
  u8g2.sendF("c", 0xA7);  
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
  
  const esp_partition_t* running = esp_ota_get_running_partition();
  esp_ota_img_states_t ota_state;
  if (esp_ota_get_state_partition(running, &ota_state) == ESP_OK) {
    if (ota_state == ESP_OTA_IMG_PENDING_VERIFY) {
      Serial.println("Primer arranque del nuevo firmware. Marcándolo como válido.");
      esp_ota_mark_app_valid_cancel_rollback();
    }
  }

  Serial.println("Setup completado");
}

// -------------------------
// LOOP
// -------------------------
void loop() {
  // Mostrar cuenta regresiva de estabilizacion al inicio
  if (!startupDone) {
    mostrarCuentaRegresiva();
    return;
  }
  
  // Si hay una alerta pendiente de confirmacion OTA, la mostramos y gestionamos sus botones
  if (otaConfirmationPending) {
    dibujarAlertaOTA();
    // Botón para alternar la opción (usando BUTTON_ALTITUDE)
    if (digitalRead(BUTTON_ALTITUDE) == LOW) {
      otaConfirmationOption = (otaConfirmationOption + 1) % 2; // 0 o 1
      delay(200);
    }
    // Botón para confirmar la selección (usando BUTTON_OLED)
    if (digitalRead(BUTTON_OLED) == LOW) {
      if (otaConfirmationOption == 0) {
        Serial.println("Usuario acepto la actualizacion OTA.");
        finalizarOTA();
      } else {
        Serial.println("Usuario cancelo la actualizacion OTA.");
        cancelarOTA();
      }
      delay(200);
    }
    return;
  }
  
  // Proceso normal del loop
  bool sensorOk = bmp.performReading();
  float altitudActual = sensorOk ? bmp.readAltitude(1013.25) : 0;
  
  // Gestión de botones para el menú normal
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
      menuOpcion = (menuOpcion + 1) % 5;
      if (menuOpcion != 3) batteryMenuActive = false;
      menuStartTime = millis();
      Serial.print("Opcion del menu cambiada a: ");
      switch (menuOpcion) {
        case 0: Serial.println("Unidad"); break;
        case 1: Serial.println("Brillo"); break;
        case 2: Serial.println("Formato Altura"); break;
        case 3: Serial.println("Bateria"); break;
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
          Serial.print("Vista Bateria en menu: ");
          Serial.println(batteryMenuActive ? "ON" : "OFF");
          break;
        case 4:
          toggleBLE();
          Serial.print("BLE ahora esta: ");
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
    Serial.println("Menu cerrado por timeout.");
  }
  
  updateBatteryReading();
  
  // Visualización y envío vía BLE
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
    float v_bat = v_adc * 2.0; // No se usa en el display; el porcentaje se obtiene de la lectura caché.
    int porcentajeBateria = cachedBatteryPercentage;
  
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
      u8g2.drawHLine(0, 15, 128);
      u8g2.drawHLine(0, 52, 128); 
      u8g2.drawHLine(0, 0, 128);
      u8g2.drawHLine(0, 63, 128); 
      u8g2.drawVLine(0, 0, 64);
      u8g2.drawVLine(127, 0, 64);
      
      u8g2.setFont(u8g2_font_ncenB08_tr);
      String user = "elDani";
      uint16_t UserWidth = u8g2.getStrWidth(user.c_str());
      int16_t xPosUser = (128 - UserWidth) / 2;
      if (xPosUser < 0) xPosUser = 0;
      u8g2.setCursor(xPosUser, 62);
      u8g2.print(user);
  
      u8g2.sendBuffer();
    }
  }
  delay(100);
}
