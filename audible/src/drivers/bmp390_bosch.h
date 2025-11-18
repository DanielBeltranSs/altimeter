#pragma once
#include <Arduino.h>
#include <Wire.h>

extern "C" {
  #include "bmp3.h"
}

class BMP390Bosch {
public:
  bool begin(TwoWire& wire, uint8_t addr = 0x77, uint32_t i2cHz = 400000);
  bool setNormalMode(uint8_t osrP = BMP3_OVERSAMPLING_8X,
                     uint8_t osrT = BMP3_OVERSAMPLING_2X,
                     uint8_t iir  = BMP3_IIR_FILTER_COEFF_3,
                     uint8_t odr  = BMP3_ODR_50_HZ);
  bool setForcedMode(uint8_t osrP = BMP3_OVERSAMPLING_8X,
                     uint8_t osrT = BMP3_OVERSAMPLING_2X,
                     uint8_t iir  = BMP3_IIR_FILTER_COEFF_3);
  bool triggerForcedMeasurement();
  bool read(float& pressurePa, float& tempC);

  bool setOdr(uint8_t odr);
  bool setIIR(uint8_t iir);
  bool setOversampling(uint8_t osrP, uint8_t osrT);

  bool softReset();
  bool whoAmI(uint8_t& chip_id);
  int8_t lastError() const { return last_error_; }

  uint8_t address() const { return ctx_.addr; }
  bool ok() const { return initialized_; }

private:
  struct IntfCtx { TwoWire* wire; uint8_t addr; } ctx_ {nullptr, 0x77};

  static int8_t i2cWrite(uint8_t reg, const uint8_t* data, uint32_t len, void* intf_ptr);
  static int8_t i2cRead (uint8_t reg, uint8_t* data, uint32_t len, void* intf_ptr);
  static void   delayUs (uint32_t period, void* intf_ptr);

  bmp3_dev dev_ {};
  bmp3_settings settings_ {};
  bool initialized_ = false;
  int8_t last_error_ = 0;

  bool tryInit_(uint8_t addr);
  bool applySettings_(uint32_t sel);
};
