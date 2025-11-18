#pragma once
#include "device_config.h"

bool cfgLoad(DeviceConfig& out);
bool cfgSave(const DeviceConfig& cfg);

// Helpers opcionales:
bool cfgSetOffsetMm(int32_t off_mm);        // fija y persiste solo el offset
