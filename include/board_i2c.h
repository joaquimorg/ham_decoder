#pragma once

#include "config.h"

#if defined(BOARD_FNK0104S)
#include "driver/i2c_master.h"

// The I2C bus shared by the touch controller and the ES8311 codec. Created on
// the first call; call it from app_main before any task uses the bus.
i2c_master_bus_handle_t board_i2c_bus();
#endif
