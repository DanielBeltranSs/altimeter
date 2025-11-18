#ifndef BLE_MODULE_H
#define BLE_MODULE_H

#include <NimBLEDevice.h>

// Variables globales que se usarán en otros módulos
extern bool deviceConnected;
extern NimBLECharacteristic *pCharacteristic;

// Funciones para la gestión de BLE
void setupBLE();
void toggleBLE();

#endif
