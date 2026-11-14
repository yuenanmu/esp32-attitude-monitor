#pragma once

#include <cstddef>
#include "imu.hpp"

class GyroCalibration {
public:
    void reset()
    {
        count_ = 0;
        sum_x_ = 0;
        sum_y_ = 0;
        sum_z_ = 0;
        bias_x_ = 0;
        bias_y_ = 0;
        bias_z_ = 0;
        ready_ = false;
    }

    bool ready() const
    {
        return ready_;
    }

    void add(const ImuData& raw)
    {
        if (ready_) {
            return;
        }
        
        sum_x_ += raw.gx;
        sum_y_ += raw.gy;
        sum_z_ += raw.gz;
        count_++;
        if (count_ >= TARGET_COUNT) {
            bias_x_ = sum_x_ / count_;
            bias_y_ = sum_y_ / count_;
            bias_z_ = sum_z_ / count_;

            ready_ = true;
        }
    }

    ImuData correct(const ImuData& raw) const
    {
        ImuData corrected = raw;

        if (ready_) {
            corrected.gx = raw.gx - bias_x_;
            corrected.gy = raw.gy - bias_y_;
            corrected.gz = raw.gz - bias_z_;
        }
        return corrected;
    }

private:
    static constexpr size_t TARGET_COUNT = 200; // 采样次数，约 2 秒钟

    size_t count_{};

    double sum_x_{};
    double sum_y_{};
    double sum_z_{};

    float bias_x_{};
    float bias_y_{};
    float bias_z_{};

    bool ready_{};
};