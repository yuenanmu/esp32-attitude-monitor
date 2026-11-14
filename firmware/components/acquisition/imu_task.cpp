#include <algorithm> // min/max算法
#include <cinttypes>
#include "sample_record.hpp"
#include "history.hpp"

#include "imu_task.hpp"
#include "imu.hpp"
#include "gyro_calibration.hpp"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

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
GyroCalibration calibration;
SampleRecord record{};
uint64_t history_write_errors = 0;
// 采样任务，读取 IMU 并写入 History
static void imu_task(void* arg)
{
    Imu* imu = static_cast<Imu*>(arg);
    
    while (true) {

        // 等待 Timer 通知
        uint32_t notifications =
            ulTaskNotifyTake(pdTRUE, portMAX_DELAY);

        if (notifications > 1) {
            ++stats.backlog_events;
            stats.extra_notifications += notifications - 1; // 统计额外通知次数
            ESP_LOGW(TAG, "Sampling task is falling behind");
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
                }

                // 校准样本暂不写进业务历史缓存。
                continue;
            }

            ImuData corrected = calibration.correct(data);

            // 更新采样记录
            record.seq = next_seq++;
            record.t_us = esp_timer_get_time();
            record.imu = corrected;
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
            }

            // TODO:
            // 将 data 交给通信模块
        }
        // 处理一次：60s后打印一次统计信息
        if (!report_stats && stats.success_count > 1 && stats.last_us - stats.first_us > 60000000) {
            report_stats = true;
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

    esp_err_t err = esp_timer_create(
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