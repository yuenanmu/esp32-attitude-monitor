#include "imu.hpp"
#include "esp_log.h"

static const char *TAG = "IMU";

esp_err_t Imu::init (i2c_master_bus_handle_t bus_handle) {
    if (bus_handle == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t ret = qmi8658_init(&imu_dev_, bus_handle, QMI8658_ADDRESS_HIGH);

    if (ret != ESP_OK) {
        return ret;
    }
    initialized_ = true;
    return configure();
}

esp_err_t Imu::configure() {
    if (initialized_ == false) {
        return ESP_ERR_INVALID_STATE;
    }
    // 设置加速度/陀螺仪量程
    qmi8658_set_accel_range(
        &imu_dev_, QMI8658_ACCEL_RANGE_8G
    );
    qmi8658_set_gyro_range(
        &imu_dev_, QMI8658_GYRO_RANGE_512DPS
    );
    // 设置输出单位
    qmi8658_set_accel_unit_mps2(&imu_dev_, true);
    qmi8658_set_gyro_unit_rads(&imu_dev_, true);

    ESP_LOGI(TAG, "QMI8658 initialized");
    return ESP_OK;
}

// esp_err_t Imu::read(ImuData& data, bool& ready) {
//     if (!initialized_) {
//         return ESP_ERR_INVALID_STATE;
//     }

//     ready = false;
//     esp_err_t ret = qmi8658_is_data_ready(&imu_dev_, &ready);

//     if (ret == ESP_OK && ready) {
//         qmi8658_data_t raw_data{};
//         // 读取imu_dev_的数据
//         esp_err_t err = qmi8658_read_sensor_data(&imu_dev_, &raw_data);
//         if (err == ESP_OK) {
//             data.ax = raw_data.accelX;
//             data.ay = raw_data.accelY;
//             data.az = raw_data.accelZ;

//             data.gx = raw_data.gyroX;
//             data.gy = raw_data.gyroY;
//             data.gz = raw_data.gyroZ;
//         }
//         return ESP_OK;
//     }
//     return ret;
// }

esp_err_t Imu::read(ImuData& data, bool& ready)
{
    ready = false;

    if (!initialized_) {
        return ESP_ERR_INVALID_STATE;
    }

    bool sensor_ready = false;
    esp_err_t ret =
        qmi8658_is_data_ready(&imu_dev_, &sensor_ready);

    if (ret != ESP_OK) {
        return ret;
    }

    if (!sensor_ready) {
        return ESP_OK;
    }

    qmi8658_data_t raw_data{};
    ret = qmi8658_read_sensor_data(&imu_dev_, &raw_data);

    if (ret != ESP_OK) {
        return ret;
    }

    data.ax = raw_data.accelX;
    data.ay = raw_data.accelY;
    data.az = raw_data.accelZ;

    data.gx = raw_data.gyroX;
    data.gy = raw_data.gyroY;
    data.gz = raw_data.gyroZ;

    ready = true;
    return ESP_OK;
}


