#include "bmp390_bosch.h"

int8_t BMP390Bosch::i2cWrite(uint8_t reg, const uint8_t* data, uint32_t len, void* intf_ptr) {
  auto* ctx = static_cast<IntfCtx*>(intf_ptr);
  ctx->wire->beginTransmission(ctx->addr);
  ctx->wire->write(reg);
  for (uint32_t i = 0; i < len; ++i) ctx->wire->write(data[i]);
  uint8_t res = ctx->wire->endTransmission();
  return (res == 0) ? BMP3_OK : BMP3_E_COMM_FAIL;
}

int8_t BMP390Bosch::i2cRead(uint8_t reg, uint8_t* data, uint32_t len, void* intf_ptr) {
  auto* ctx = static_cast<IntfCtx*>(intf_ptr);
  ctx->wire->beginTransmission(ctx->addr);
  ctx->wire->write(reg);
  if (ctx->wire->endTransmission(false) != 0) return BMP3_E_COMM_FAIL; // repeated start
  uint32_t idx = 0;
  ctx->wire->requestFrom((int)ctx->addr, (int)len);
  while (ctx->wire->available() && idx < len) data[idx++] = ctx->wire->read();
  return (idx == len) ? BMP3_OK : BMP3_E_COMM_FAIL;
}

void BMP390Bosch::delayUs(uint32_t period, void*) {
  if (period >= 1000) delay(period / 1000);
  else delayMicroseconds(period);
}

bool BMP390Bosch::tryInit_(uint8_t addr) {
  ctx_.addr      = addr;
  dev_.intf      = BMP3_I2C_INTF;
  dev_.intf_ptr  = &ctx_;
  dev_.read      = &BMP390Bosch::i2cRead;
  dev_.write     = &BMP390Bosch::i2cWrite;
  dev_.delay_us  = &BMP390Bosch::delayUs;

  last_error_ = bmp3_init(&dev_);
  return (last_error_ == BMP3_OK);
}

bool BMP390Bosch::begin(TwoWire& wire, uint8_t addr, uint32_t i2cHz) {
  ctx_.wire = &wire;
  ctx_.wire->setClock(i2cHz);

  initialized_ = tryInit_(addr) || tryInit_(addr == 0x77 ? 0x76 : 0x77);
  return initialized_;
}

bool BMP390Bosch::applySettings_(uint32_t sel) {
  last_error_ = bmp3_set_sensor_settings(sel, &settings_, &dev_);
  if (last_error_ != BMP3_OK) return false;
  last_error_ = bmp3_set_op_mode(&settings_, &dev_);
  return (last_error_ == BMP3_OK);
}

bool BMP390Bosch::setNormalMode(uint8_t osrP, uint8_t osrT, uint8_t iir, uint8_t odr) {
  if (!initialized_) return false;

  // Enable sensors
  settings_.press_en = BMP3_ENABLE;
  settings_.temp_en  = BMP3_ENABLE;

  // Sampling/filter/ODR
  settings_.odr_filter.odr        = odr;
  settings_.odr_filter.iir_filter = iir;
  settings_.odr_filter.press_os   = osrP;
  settings_.odr_filter.temp_os    = osrT;

  // Power mode
  settings_.op_mode = BMP3_MODE_NORMAL;

  uint32_t sel = 0;
  sel |= BMP3_SEL_PRESS_EN | BMP3_SEL_TEMP_EN;
  sel |= BMP3_SEL_ODR | BMP3_SEL_IIR_FILTER;
  sel |= BMP3_SEL_PRESS_OS | BMP3_SEL_TEMP_OS;

  return applySettings_(sel);
}

bool BMP390Bosch::setForcedMode(uint8_t osrP, uint8_t osrT, uint8_t iir) {
  if (!initialized_) return false;

  settings_.press_en = BMP3_ENABLE;
  settings_.temp_en  = BMP3_ENABLE;
  settings_.odr_filter.iir_filter = iir;
  settings_.odr_filter.press_os   = osrP;
  settings_.odr_filter.temp_os    = osrT;
  settings_.op_mode  = BMP3_MODE_FORCED;

  uint32_t sel = 0;
  sel |= BMP3_SEL_PRESS_EN | BMP3_SEL_TEMP_EN;
  sel |= BMP3_SEL_IIR_FILTER;
  sel |= BMP3_SEL_PRESS_OS | BMP3_SEL_TEMP_OS;

  return applySettings_(sel);
}

bool BMP390Bosch::triggerForcedMeasurement() {
  if (!initialized_) return false;
  settings_.op_mode = BMP3_MODE_FORCED;
  last_error_ = bmp3_set_op_mode(&settings_, &dev_);
  return (last_error_ == BMP3_OK);
}

bool BMP390Bosch::setOdr(uint8_t odr) {
  if (!initialized_) return false;
  settings_.odr_filter.odr = odr;
  last_error_ = bmp3_set_sensor_settings(BMP3_SEL_ODR, &settings_, &dev_);
  return (last_error_ == BMP3_OK);
}

bool BMP390Bosch::setIIR(uint8_t iir) {
  if (!initialized_) return false;
  settings_.odr_filter.iir_filter = iir;
  last_error_ = bmp3_set_sensor_settings(BMP3_SEL_IIR_FILTER, &settings_, &dev_);
  return (last_error_ == BMP3_OK);
}

bool BMP390Bosch::setOversampling(uint8_t osrP, uint8_t osrT) {
  if (!initialized_) return false;
  settings_.odr_filter.press_os   = osrP;
  settings_.odr_filter.temp_os    = osrT;
  last_error_ = bmp3_set_sensor_settings(BMP3_SEL_PRESS_OS | BMP3_SEL_TEMP_OS, &settings_, &dev_);
  return (last_error_ == BMP3_OK);
}

bool BMP390Bosch::softReset() {
  if (!initialized_) return false;
  last_error_ = bmp3_soft_reset(&dev_);
  return (last_error_ == BMP3_OK);
}

bool BMP390Bosch::whoAmI(uint8_t& chip_id) {
  if (!initialized_) return false;
  chip_id = dev_.chip_id;
  return true;
}

bool BMP390Bosch::read(float& pressurePa, float& tempC) {
  if (!initialized_) return false;
  bmp3_data data {};
  last_error_ = bmp3_get_sensor_data(BMP3_PRESS | BMP3_TEMP, &data, &dev_);
  if (last_error_ != BMP3_OK) return false;
  pressurePa = static_cast<float>(data.pressure);
  tempC      = static_cast<float>(data.temperature);
  return (pressurePa > 1000.f && pressurePa < 120000.f);
}
