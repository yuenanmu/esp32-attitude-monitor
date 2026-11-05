#include <cstdio>
#include "board_i2c.hpp"
#include "imu.hpp"
#include "imu_task.hpp"
#include "comm.hpp"

#include "esp_log.h"
#include "esp_err.h"

static Imu imu;
extern "C" void app_main(void)
{
    // 1. 初始化I2C总线
    i2c_master_bus_handle_t bus_handle;

    ESP_ERROR_CHECK(
        board_i2c_init(&bus_handle)
    );
    // 2. 初始化IMU

    ESP_ERROR_CHECK(
        imu.init(bus_handle)
    );

    // 3. 创建IMU定时读取任务
    ESP_ERROR_CHECK(
        imu_sampling_start(&imu)
    );
    // 4. 创建串口通信任务
    ESP_ERROR_CHECK(
        comm_start()
    );
}
