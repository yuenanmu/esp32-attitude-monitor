#include "board_i2c.hpp"
#include "driver/gpio.h"

esp_err_t board_i2c_init(i2c_master_bus_handle_t *bus_handle) { 

    if (bus_handle == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }
    // i2c 结构体配置
    i2c_master_bus_config_t config{};
    config.i2c_port = I2C_NUM_0;
    config.sda_io_num = GPIO_NUM_11;
    config.scl_io_num = GPIO_NUM_10;
    config.clk_source = I2C_CLK_SRC_DEFAULT;
    config.glitch_ignore_cnt = 7;
    config.flags.enable_internal_pullup = true;

    return i2c_new_master_bus(&config, bus_handle);
}