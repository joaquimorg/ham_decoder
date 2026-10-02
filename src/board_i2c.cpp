#include "board_i2c.h"

#if defined(BOARD_FNK0104S)

#include "esp_err.h"

i2c_master_bus_handle_t board_i2c_bus()
{
    static i2c_master_bus_handle_t bus = nullptr;
    if (bus)
        return bus;

    i2c_master_bus_config_t cfg = {};
    cfg.i2c_port = I2C_NUM_0;
    cfg.sda_io_num = (gpio_num_t)BOARD_I2C_SDA;
    cfg.scl_io_num = (gpio_num_t)BOARD_I2C_SCL;
    cfg.clk_source = I2C_CLK_SRC_DEFAULT;
    cfg.glitch_ignore_cnt = 7;
    cfg.flags.enable_internal_pullup = true;    // external 10 k are fitted too
    ESP_ERROR_CHECK(i2c_new_master_bus(&cfg, &bus));
    return bus;
}

#endif
