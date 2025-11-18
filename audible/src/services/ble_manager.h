#pragma once
#include <stdint.h>

// Gestor BLE on-demand (stub listo para conectar NimBLE)
class BleManager {
public:
  void begin() {} // opcional si necesitas inicialización global

  void enable(uint32_t now_ms, uint32_t window_ms);
  void onConnected(uint32_t now_ms);
  void onDisconnected(uint32_t now_ms, uint32_t grace_ms);
  void disable();

  // Llamar en cada loop: corta por timeout o por "must_off"
  void tick(uint32_t now_ms, bool must_off);

  bool active() const { return _active; }
  bool connected() const { return _connected; }

private:
  bool _active = false;
  bool _connected = false;
  uint32_t _window_deadline = 0;
  uint32_t _disc_grace_deadline = 0;
};
