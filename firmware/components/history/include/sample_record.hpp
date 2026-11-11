#pragma once

#include <cstdint>
#include "imu.hpp"

struct SampleRecord {
    uint64_t seq{};  // 成功采样序号，从 1 开始
    int64_t t_us{};  // 读取完成附近的开机微秒时间
    ImuData imu{};   // 六轴数据，复用已有结构
};