#pragma once

#include "esp_err.h"
#include "driver/i2c_master.h"
#include "qmi8658.h"

struct ImuData {
    float ax{}, ay{}, az{}; // Accelerometer data in m/s^2
    float gx{}, gy{}, gz{}; // Gyroscope data in rad/s
};

class Imu {
public: 
    esp_err_t init(i2c_master_bus_handle_t bus_handle);
    esp_err_t configure();
    esp_err_t read(ImuData& data, bool& ready);
private:
    qmi8658_dev_t imu_dev_{}; // Imu对象自动创建一个设备对象。在init中绑定，不从外传进来
    bool initialized_{false};
};