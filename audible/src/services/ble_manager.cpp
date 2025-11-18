#include "ble_manager.h"
#include <Arduino.h>

void BleManager::enable(uint32_t now_ms, uint32_t window_ms) {
  _active = true;
  _connected = false;
  _window_deadline = now_ms + window_ms;
  _disc_grace_deadline = 0;
  // TODO: iniciar pila BLE + advertising
  Serial.println("BLE: ENABLE window");
}

void BleManager::onConnected(uint32_t /*now_ms*/) {
  if (!_active) return;
  _connected = true;
  Serial.println("BLE: CONNECTED");
}

void BleManager::onDisconnected(uint32_t now_ms, uint32_t grace_ms) {
  if (!_active) return;
  _connected = false;
  _disc_grace_deadline = now_ms + grace_ms;
  Serial.println("BLE: DISCONNECTED");
}

void BleManager::disable() {
  if (!_active) return;
  // TODO: apagar pila BLE
  _active = false;
  _connected = false;
  _window_deadline = 0;
  _disc_grace_deadline = 0;
  Serial.println("BLE: DISABLE");
}

void BleManager::tick(uint32_t now_ms, bool must_off) {
  if (!_active) return;
  if (must_off) { disable(); return; }
  if (!_connected && _window_deadline && now_ms >= _window_deadline) { disable(); return; }
  if (!_connected && _disc_grace_deadline && now_ms >= _disc_grace_deadline) { disable(); return; }
}
