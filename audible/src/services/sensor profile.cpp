#include "sensor_profile.h"

void SensorProfile::applyFor(FlightMode m, BMP390Bosch& bmp,
                             uint32_t& loopPeriodMs, bool& normalStreaming) const {
  switch (m) {
    case FlightMode::GROUND:
      bmp.setForcedMode(BMP3_OVERSAMPLING_2X, BMP3_OVERSAMPLING_16X, BMP3_IIR_FILTER_COEFF_7);
      loopPeriodMs = 1000;  // 1 Hz
      normalStreaming = false;
      break;

    case FlightMode::CLIMB:
      bmp.setForcedMode(BMP3_OVERSAMPLING_2X, BMP3_OVERSAMPLING_8X, BMP3_IIR_FILTER_COEFF_3);
      loopPeriodMs = 200;   // 5 Hz
      normalStreaming = false;
      break;

    case FlightMode::FREEFALL:
      // NORMAL 100 Hz para mínima latencia (usa IIR=1 por compatibilidad)
      bmp.setNormalMode(BMP3_ODR_100_HZ, BMP3_OVERSAMPLING_4X, BMP3_NO_OVERSAMPLING,
                        BMP3_IIR_FILTER_COEFF_1);
      loopPeriodMs = 10;    // ~100 Hz
      normalStreaming = true;
      break;

    case FlightMode::CANOPY:
      bmp.setForcedMode(BMP3_OVERSAMPLING_4X, BMP3_NO_OVERSAMPLING, BMP3_IIR_FILTER_COEFF_1);
      loopPeriodMs = 100;   // 10 Hz
      normalStreaming = false;
      break;
  }
}
