#include <algorithm> // min/max算法
#include <cinttypes>
#include "sample_record.hpp"
#include "history.hpp"

#include "imu_task.hpp"
#include "imu.hpp"
#include "gyro_calibration.hpp"
#include "attitude.hpp"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"

#include "esp_timer.h"
#include "esp_log.h"

static const char* TAG = "IMU_TASK";
static TaskHandle_t imu_task_handle{nullptr};
static esp_timer_handle_t sample_timer{nullptr};

struct SamplingStats {
    // 采样计数统计
    uint64_t success_count{}; // 成功采样计数
    uint64_t read_errors{}; // 读取错误计数
    uint64_t not_ready{};
    uint64_t backlog_events{};
    uint64_t extra_notifications{};
    // 采样时间统计
    int64_t first_us{};
    int64_t last_us{};
    // 采样间隔统计
    int64_t min_dt_us{};
    int64_t max_dt_us{};
    int64_t sum_dt_us{};
    uint64_t interval_count{};
};

static void update_stats(
    SamplingStats& stats,
    const SampleRecord& record)
{
    // TODO 2.1：
    // 如果这是第一个有效样本：
    //   first_us = record.t_us
    // 否则：
    //   dt = record.t_us - last_us
    //   更新 min/max/sum/interval_count
    //
    // 注意：第一个间隔需要直接初始化 min，
    // 不能用初始值 0 和正数取最小值。
    if (stats.first_us == 0) {
        stats.first_us = record.t_us;
    } else {
        int64_t dt = record.t_us - stats.last_us;
        if (stats.interval_count ==0) {
            stats.min_dt_us = dt;
            stats.max_dt_us = dt;
            stats.sum_dt_us = dt;
            stats.interval_count = 1;
        } else {
            stats.min_dt_us = std::min(stats.min_dt_us, dt);
            stats.max_dt_us = std::max(stats.max_dt_us, dt);
            stats.sum_dt_us += dt;
            stats.interval_count++;
        }
    }
    // TODO 2.2：
    // 更新 last_us 和 success_count。
    stats.last_us = record.t_us;
    stats.success_count++; // 成功采样计数
}

// 采样任务的控制命令类型，是一个枚举类型
enum class ControlType {
    CalibrateGyro,
    SetRate,
};

struct ControlCommand {
    ControlType type{};
    uint32_t value{};
};

static QueueHandle_t control_queue = nullptr;
static SemaphoreHandle_t status_mutex = nullptr; // 状态锁
static AcquisitionStatus acquisition_status{};  //全局，加锁保证序列号自增安全

static esp_err_t controls_init()
{
    status_mutex = xSemaphoreCreateMutex();

    if (status_mutex == nullptr) {
        return ESP_ERR_NO_MEM;
    }

    control_queue = xQueueCreate(1, sizeof(ControlCommand));

    if (control_queue == nullptr) {
        vSemaphoreDelete(status_mutex);
        status_mutex = nullptr;
        return ESP_ERR_NO_MEM;
    }

    acquisition_status = {};
    return ESP_OK;
}

esp_err_t acquisition_get_status(AcquisitionStatus& output)
{
    if (status_mutex == nullptr) {
        return ESP_ERR_INVALID_STATE;
    }

    xSemaphoreTake(status_mutex, portMAX_DELAY);
    output = acquisition_status;
    xSemaphoreGive(status_mutex);

    return ESP_OK;
}

// 提交命令入队，返回 ESP_OK 表示成功入队。
static esp_err_t submit_control(
    const ControlCommand& command,
    uint32_t& request_id)
{
    if (control_queue == nullptr || status_mutex == nullptr) {
        return ESP_ERR_INVALID_STATE;
    }

    xSemaphoreTake(status_mutex, portMAX_DELAY);

    if (acquisition_status.command_pending ||
        acquisition_status.calibrating) {
        xSemaphoreGive(status_mutex);
        return ESP_ERR_INVALID_STATE;
    }

    if (xQueueSend(control_queue, &command, 0) != pdPASS) {
        xSemaphoreGive(status_mutex);
        return ESP_ERR_NO_MEM;
    }

    acquisition_status.command_pending = true;
    ++acquisition_status.request_id;

    request_id = acquisition_status.request_id;   // 返回 request_id

    xSemaphoreGive(status_mutex);
    return ESP_OK;
}

// 矫正请求
esp_err_t acquisition_request_calibration(uint32_t& request_id)
{
    ControlCommand command{};
    command.type = ControlType::CalibrateGyro;

    return submit_control(command, request_id);
}

// 采样率设置请求
esp_err_t acquisition_request_rate(
    uint32_t hz,
    uint32_t& request_id)
{
    if (hz != 100 && hz != 200) {
        return ESP_ERR_INVALID_ARG;
    }

    ControlCommand command{};
    command.type = ControlType::SetRate;
    command.value = hz;

    return submit_control(command, request_id);
}

// Timer 回调：只负责通知任务
static void timer_callback(void* arg)
{
    TaskHandle_t task =
        static_cast<TaskHandle_t>(arg);

    if (task != nullptr) {
        xTaskNotifyGive(task);
    }
}


// IMU 采集任务
uint64_t next_seq = 1;

SamplingStats stats{};
bool report_stats = false;
GyroCalibration calibration;  // 陀螺仪校准
SampleRecord record{};
uint64_t history_write_errors = 0;
// 姿态估计器
AttitudeEstimator estimator;

// 采样任务，读取 IMU 并写入 History
static void imu_task(void* arg)
{
    Imu* imu = static_cast<Imu*>(arg);
    // 初始化姿态估计器
    estimator.reset(100.0f);
    while (true) {

        // 等待 Timer 通知
        uint32_t notifications =
            ulTaskNotifyTake(pdTRUE, portMAX_DELAY);

        if (notifications > 1) {
            ++stats.backlog_events;
            stats.extra_notifications += notifications - 1; // 统计额外通知次数
            ESP_LOGW(TAG, "Sampling task is falling behind");
        }
        ControlCommand command{};
        // 从队列中获取command，并且分命令处理
        if (xQueueReceive(control_queue, &command, 0) == pdPASS) {
            if (command.type == ControlType::CalibrateGyro) {
                // 重置校准器
                calibration.reset();
                // 重置姿态估计器，使用当前采样率
                xSemaphoreTake(status_mutex, portMAX_DELAY);
                uint32_t rate_hz = acquisition_status.rate_hz;
                xSemaphoreGive(status_mutex);
                estimator.reset(rate_hz);
                // 重置采样统计
                xSemaphoreTake(status_mutex, portMAX_DELAY);
                acquisition_status.calibrating = true;
                acquisition_status.command_pending = false;
                xSemaphoreGive(status_mutex);
                
                continue;
            } else if (command.type == ControlType::SetRate) {
                // 计算新的定时器周期，单位微秒
                uint64_t period_us = 1'000'000 / command.value; 

                esp_err_t err =
                    esp_timer_restart(sample_timer, period_us);

                if (err == ESP_OK) {
                    // TODO 7.2
                    // 1. estimator.reset(command.value)
                    // 2. 重置 stats、report_stats。
                    // 3. 在状态锁内更新 rate_hz。
                    //
                    // 保留当前陀螺仪零偏。
                    estimator.reset(command.value);
                    stats = {};
                    report_stats = false;

                    xSemaphoreTake(status_mutex, portMAX_DELAY);
                    acquisition_status.rate_hz = command.value;
                    xSemaphoreGive(status_mutex);

                    // 清除切换前已经积累的采样通知。
                    ulTaskNotifyTake(pdTRUE, 0);
                }

                // TODO 7.3
                // 在状态锁内：
                // last_result = err
                // command_pending = false
                //
                // 定时器修改失败时，rate_hz 保持原值。
                xSemaphoreTake(status_mutex, portMAX_DELAY);
                acquisition_status.last_result = err; 
                acquisition_status.command_pending = false;
                xSemaphoreGive(status_mutex);

                continue;
            }

        }
        // 读取 IMU
        ImuData data{};
        bool ready = false;

        esp_err_t ret = imu->read(data, ready);

        // 处理读取结果
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "IMU read failed: %s",
                     esp_err_to_name(ret));
            ++stats.read_errors;
            continue;
        }
        if (!ready) {
            ++stats.not_ready;
            continue;
        }
        if (ready) {
            // 进行陀螺仪角速度校准
            if (!calibration.ready()) {
                calibration.add(data);

                if (calibration.ready()) {
                    ESP_LOGI(TAG, "Gyro calibration complete");
                    stats = {}; // 重置采样统计
                    report_stats = false;
                    // 初始化
                    xSemaphoreTake(status_mutex, portMAX_DELAY);
                    acquisition_status.calibrating = false;
                    acquisition_status.command_pending = false;
                    acquisition_status.last_result = ESP_OK; 
                    xSemaphoreGive(status_mutex);
                }

                // 校准样本暂不写进业务历史缓存。
                continue;
            }

            ImuData corrected = calibration.correct(data);
            int64_t now_us = esp_timer_get_time();
            AttitudeAngles attitude{};

            if (!estimator.update(corrected, now_us, attitude)) {
                continue;
            }

            // 更新采样记录
            record.seq = next_seq++;
            record.t_us = now_us;
            record.imu = corrected;
            record.attitude = attitude;
            // record.attitude = estimator.update(corrected, now_us, attitude) ? attitude : AttitudeAngles{};
            // 统计
            update_stats(stats, record);

            // 写入 History
            esp_err_t save_ret = history_push(record);
            if (save_ret != ESP_OK) {
                history_write_errors++;
                ESP_LOGE(TAG, "history_push failed: %s",
                         esp_err_to_name(save_ret));
            }
            // 每10次打印一次，避免日志影响采样
            static uint32_t count = 0;

            if (++count % 50 == 0) {
                ESP_LOGI(TAG,
                    "ACC: %.3f %.3f %.3f",
                    data.ax, data.ay, data.az);
            }
            if (record.seq % 100 == 0) {
                // ESP_LOGI(TAG,
                // "seq: %" PRIu64 ", t_us: %" PRId64,
                // record.seq, record.t_us);
                ESP_LOGI(TAG,
                "RPY deg: %.2f %.2f %.2f",
                record.attitude.roll_deg, record.attitude.pitch_deg, record.attitude.yaw_deg);
            }

            // TODO:
            // 将 data 交给通信模块
        }
        // 处理一次：60s后打印一次统计信息
        if (!report_stats && stats.success_count > 1 && stats.last_us - stats.first_us > 60000000) {
            double measured_hz =
            stats.interval_count * 1'000'000.0 /
            static_cast<double>(stats.last_us - stats.first_us);

            report_stats = true;

            ESP_LOGI(TAG, "Measured sampling rate: %.2f Hz", measured_hz);

            ESP_LOGI(TAG, "Sampling stats: "
                "success_count=%" PRIu64 ", "
                "read_errors=%" PRIu64 ", "
                "not_ready=%" PRIu64 ", "
                "backlog_events=%" PRIu64 ", "
                "extra_notifications=%" PRIu64 ", "
                "first_us=%" PRId64 ", "
                "last_us=%" PRId64 ", "
                "min_dt_us=%" PRId64 ", "
                "max_dt_us=%" PRId64 ", "
                "avg_dt_us=%.2f",
                stats.success_count,
                stats.read_errors,
                stats.not_ready,
                stats.backlog_events,
                stats.extra_notifications,
                stats.first_us,
                stats.last_us,
                stats.min_dt_us,
                stats.max_dt_us,
                static_cast<double>(stats.sum_dt_us) / stats.interval_count);

        }
    }
}


// 启动采样系统
esp_err_t imu_sampling_start(Imu* imu)
{
    if (imu == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }
    // 初始化控制模块
    esp_err_t err = controls_init();

    if (err != ESP_OK) {
        return err;
    }

    // 1. 创建采集任务
    BaseType_t ret = xTaskCreate(
        imu_task,
        "imu_task",
        4096,
        imu,
        5,
        &imu_task_handle
    );

    if (ret != pdPASS) {
        return ESP_ERR_NO_MEM;
    }

    // 2. 创建定时器
    esp_timer_create_args_t timer_args{};

    timer_args.callback = timer_callback;
    timer_args.arg = static_cast<void*>(imu_task_handle);
    timer_args.dispatch_method = ESP_TIMER_TASK;
    timer_args.name = "imu_100hz";

    err = esp_timer_create(
        &timer_args,
        &sample_timer
    );

    if (err != ESP_OK) {
        vTaskDelete(imu_task_handle);
        imu_task_handle = nullptr;
        return err;
    }

    // 3. 启动周期定时器：10000us = 10ms
    return esp_timer_start_periodic(
        sample_timer, 10000
    );
}