#pragma once

#include "driver/i2c_master.h"
#include "esp_err.h"

// 初始化 I2C总线，移交总线句柄。
esp_err_t board_i2c_init(i2c_master_bus_handle_t *bus_handle); 