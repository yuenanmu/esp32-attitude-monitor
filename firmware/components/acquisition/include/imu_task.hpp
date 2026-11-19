#pragma once 

#include "esp_err.h"
#include <cstdint>

// 采样系统状态
struct AcquisitionStatus {
    bool calibrating{true};
    bool command_pending{};
    uint32_t rate_hz{100};

    uint32_t request_id{};
    esp_err_t last_result{ESP_OK};
};

// ESP_OK 表示请求成功入队。
// 具体执行结果通过状态查询。
esp_err_t acquisition_request_calibration(uint32_t& request_id);

esp_err_t acquisition_request_rate(
    uint32_t hz,
    uint32_t& request_id
);

esp_err_t acquisition_get_status(AcquisitionStatus& output);

class Imu;

esp_err_t imu_sampling_start(Imu* imu);