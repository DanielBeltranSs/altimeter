#include "ble_module.h"
#include "config.h"          // Para acceder a usuarioActual, bleActivo, etc.
#include <Arduino.h>
#include <esp_task_wdt.h>
#include <Update.h>


// Variables globales
bool deviceConnected = false;
NimBLECharacteristic *pCharacteristic = nullptr;
bool updateStarted = false;

// Nota: Según la versión de NimBLE en ESP32-C3, la firma de onConnect/onDisconnect podría no incluir el segundo parámetro.
class MyServerCallbacks : public NimBLEServerCallbacks {
  void onConnect(NimBLEServer* pServer, NimBLEConnInfo& connInfo) override {
    deviceConnected = true;
    Serial.println("[BLE] Cliente conectado");
  }
  void onDisconnect(NimBLEServer* pServer, NimBLEConnInfo& connInfo, int reason) override {
    deviceConnected = false;
    Serial.println("[BLE] Cliente desconectado");
    // Se recomienda evitar operaciones bloqueantes en este callback.
    // En lugar de utilizar delay(), se puede iniciar la publicidad de forma inmediata:
    NimBLEDevice::startAdvertising();
    Serial.println("[BLE] Publicidad reiniciada");
  }
  // Se pueden agregar los demás callbacks siguiendo la documentación.
};




  class DFUCallbacks : public NimBLECharacteristicCallbacks {
    public:
      DFUCallbacks() {
        Serial.println("[DFU] DFUCallbacks instanciado");
      }
      void onWrite(NimBLECharacteristic *pCharacteristic, NimBLEConnInfo &connInfo) {
        Serial.println("[DFU] onWrite llamado");
        esp_task_wdt_reset();
        std::string rxValue = pCharacteristic->getValue();
        Serial.print("[DFU] Recibido chunk, tamaño: ");
        Serial.println(rxValue.size());
        
        // Si se recibe el marcador EOF, finalizamos la actualización
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
        
        // Si la actualización aún no ha iniciado, iniciarla directamente
        if (!updateStarted) {
          Serial.println("[DFU] Iniciando actualización OTA...");
          if (!Update.begin(UPDATE_SIZE_UNKNOWN)) {
            Serial.println("[DFU] Update.begin() falló");
            return;
          }
          updateStarted = true;
        }
        
        // Procesar el chunk (si ya se inició la actualización)
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
  void onWrite(NimBLECharacteristic *pCharacteristic, NimBLEConnInfo &connInfo) override {
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
  
  // Servicio DFU OTA
  Serial.println("Creando servicio DFU OTA...");
  NimBLEService *pDFUService = pServer->createService(DFU_SERVICE_UUID);
  NimBLECharacteristic *pDFUCharacteristic = pDFUService->createCharacteristic(
    DFU_CHARACTERISTIC_UUID,
    NIMBLE_PROPERTY::WRITE | NIMBLE_PROPERTY::WRITE_NR
  );
  pDFUCharacteristic->setCallbacks(new DFUCallbacks());
  pDFUService->start();
  Serial.println("Servicio DFU OTA iniciado");
  
  // Servicio de actualización de usuario
  Serial.println("Creando servicio de actualización de usuario...");
  NimBLEService *pUsernameService = pServer->createService(USERNAME_SERVICE_UUID);
  NimBLECharacteristic *pUsernameCharacteristic = pUsernameService->createCharacteristic(
    USERNAME_CHARACTERISTIC_UUID,
    NIMBLE_PROPERTY::WRITE | NIMBLE_PROPERTY::WRITE_NR
  );
  pUsernameCharacteristic->setCallbacks(new UsernameCallbacks());
  pUsernameService->start();
  Serial.println("Servicio de usuario iniciado");
  
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
  NimBLEAdvertising *pAdvertising = NimBLEDevice::getAdvertising();
  if (!bleActivo) {
    pAdvertising->start();
    Serial.println("BLE advertising started");
  } else {
    pAdvertising->stop();
    Serial.println("BLE advertising stopped");
  }
  bleActivo = !bleActivo;
}
