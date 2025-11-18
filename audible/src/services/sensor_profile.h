#pragma once
#include <stdint.h>
#include "app/flight_mode.h"
#include "drivers/bmp390_bosch.h"

// Aplica perfiles del BMP390 por modo y fija periodo de loop/streaming
class SensorProfile {
public:
  // Modifica loopPeriodMs y normalStreaming según el modo
  void applyFor(FlightMode m, BMP390Bosch& bmp,
                uint32_t& loopPeriodMs, bool& normalStreaming) const;
};
