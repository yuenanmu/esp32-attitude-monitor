#pragma once

#include <cstdint>

#include "Fusion.h"
#include "imu.hpp"
#include "attitude_types.hpp"

class AttitudeEstimator {
public:
    void reset(float nominal_hz);

    // 输入已经进行零偏修正的六轴数据。
    // 成功更新姿态时返回 true。
    bool update(
        const ImuData& data,
        int64_t t_us,
        AttitudeAngles& output
    );

private:
    FusionAhrs ahrs_{};
    int64_t previous_us_{};
    bool have_previous_{};
};