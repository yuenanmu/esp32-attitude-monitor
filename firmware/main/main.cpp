#include <cstdio>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/i2c_master.h"
#include "esp_log.h"
#include "esp_err.h"
#include "qmi8658.h"

static const char *TAG = "IMU_TEST"; 
extern "C" void app_main(void)
{
    // 初始化 I2C 配置结构体
    i2c_master_bus_config_t bus_config = {};

    bus_config.i2c_port = I2C_NUM_0;
    bus_config.sda_io_num = GPIO_NUM_11;
    bus_config.scl_io_num = GPIO_NUM_10;
    bus_config.clk_source = I2C_CLK_SRC_DEFAULT;
    bus_config.glitch_ignore_cnt = 7;
    bus_config.flags.enable_internal_pullup = true;

    // 创建 I2C 总线
    i2c_master_bus_handle_t bus_handle = nullptr;

    ESP_ERROR_CHECK(
        i2c_new_master_bus(&bus_config, &bus_handle)
    );

    ESP_LOGI("I2C", "I2C initialized successfully");

    // 2. 初始化 QMI8658
    qmi8658_dev_t imu = {};

    ESP_ERROR_CHECK(qmi8658_init(
        &imu, bus_handle, QMI8658_ADDRESS_HIGH
    ));

    // 3. 配置量程和输出单位
    qmi8658_set_accel_range(
        &imu, QMI8658_ACCEL_RANGE_8G
    );
    qmi8658_set_gyro_range(
        &imu, QMI8658_GYRO_RANGE_512DPS
    );
    qmi8658_set_accel_unit_mps2(&imu, true);
    qmi8658_set_gyro_unit_rads(&imu, true);

    ESP_LOGI(TAG, "QMI8658 initialized");

    // 4. 周期性读取
    while (1) {
        bool ready = false;
        esp_err_t err = qmi8658_is_data_ready(
            &imu, &ready
        );

        if (err == ESP_OK && ready) {
            qmi8658_data_t data{};

            err = qmi8658_read_sensor_data(
                &imu, &data
            );

            if (err == ESP_OK) {
                ESP_LOGI(TAG,
                    "ACC: %.3f %.3f %.3f m/s2",
                    data.accelX,
                    data.accelY,
                    data.accelZ
                );

                ESP_LOGI(TAG,
                    "GYRO: %.3f %.3f %.3f rad/s",
                    data.gyroX,
                    data.gyroY,
                    data.gyroZ
                );
            }
        }

        vTaskDelay(pdMS_TO_TICKS(100));
    }
}
