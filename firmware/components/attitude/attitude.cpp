#include "attitude.hpp"

#include <cmath>

void AttitudeEstimator::reset(float nominal_hz)
{
    FusionAhrsInitialise(&ahrs_);

    FusionAhrsSettings settings = fusionAhrsDefaultSettings;

    settings.sampleRate = nominal_hz;
    settings.convention = FusionConventionNwu;
    settings.gain = 0.5f;
    settings.gyroscopeRange = 512.0f;

    FusionAhrsSetSettings(&ahrs_, &settings);

    previous_us_ = 0;
    have_previous_ = false;
}

bool AttitudeEstimator::update(
    const ImuData& data,
    int64_t t_us,
    AttitudeAngles& output)
{
    if (!have_previous_) {
        previous_us_ = t_us;
        have_previous_ = true;
        return false;
    }

    int64_t delta_us = t_us - previous_us_;
    previous_us_ = t_us;

    // 初版选择：超过 100 ms 的间隔视为一次中断。
    // 重置后重新建立时间基准。
    if (delta_us <= 0 || delta_us > 100'000) {
        FusionAhrsRestart(&ahrs_);
        have_previous_ = false;
        return false;
    }

    float dt_seconds =
        static_cast<float>(delta_us) * 1.0e-6f;

    FusionVector gyro{};
    FusionVector accel{};

    // TODO 3.1
    // 把 data.gx/gy/gz 从 rad/s 转换为 °/s。
    // 填入 gyro.axis.x/y/z。
    //
    // 提示：FusionRadiansToDegrees(...)
    gyro.axis.x = FusionRadiansToDegrees(data.gx);
    gyro.axis.y = FusionRadiansToDegrees(data.gy);
    gyro.axis.z = FusionRadiansToDegrees(data.gz);

    // TODO 3.2
    // 把 data.ax/ay/az 从 m/s² 转换为 g。
    // 填入 accel.axis.x/y/z。
    //
    // 提示：除以 9.80665f。
    //
    // 此处应包含重力分量，直接使用传感器加速度。
    accel.axis.x = data.ax / 9.80665f;
    accel.axis.y = data.ay / 9.80665f;
    accel.axis.z = data.az / 9.80665f;

    // TODO 3.3
    // 核对坐标映射。
    // 加速度和角速度必须使用同一个右手坐标系。
    // 初版沿用已验证的传感器轴方向。

    
    // TODO 3.4
    // 调用 FusionAhrsSetSamplePeriod，
    // 将 dt_seconds 设置到 ahrs_。s
    FusionAhrsSetSamplePeriod(&ahrs_, dt_seconds);

    // TODO 3.5
    // 调用 FusionAhrsUpdateNoMagnetometer，
    // 传入 ahrs_、gyro、accel。
    FusionAhrsUpdateNoMagnetometer(&ahrs_, gyro, accel);
    // TODO 3.6
    // 取得四元数，再调用 FusionQuaternionToEuler。
    // 将 euler.angle.roll/pitch/yaw 写入 output。
    FusionQuaternion quaternion =  FusionAhrsGetQuaternion(&ahrs_);
    FusionEuler euler = FusionQuaternionToEuler(quaternion);
    // output 赋值
    output.roll_deg = euler.angle.roll;
    output.pitch_deg = euler.angle.pitch;
    output.yaw_deg = euler.angle.yaw;
    // TODO 3.7
    // 检查输出三轴都为有限数。
    // 成功返回 true。
    // 完成前暂时保持 false，避免把空结果当有效姿态。
    return std::isfinite(output.roll_deg) &&
           std::isfinite(output.pitch_deg) &&
           std::isfinite(output.yaw_deg);
}