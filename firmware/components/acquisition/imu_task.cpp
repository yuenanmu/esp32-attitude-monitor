#include "imu_task.hpp"
#include "imu.hpp"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_timer.h"
#include "esp_log.h"

static const char* TAG = "IMU_TASK";
static TaskHandle_t imu_task_handle{nullptr};
static esp_timer_handle_t sample_timer{nullptr};

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
static void imu_task(void* arg)
{
    Imu* imu = static_cast<Imu*>(arg);

    while (true) {

        // 等待 Timer 通知
        uint32_t notifications =
            ulTaskNotifyTake(pdTRUE, portMAX_DELAY);

        if (notifications > 1) {
            ESP_LOGW(TAG, "Sampling task is falling behind");
        }

        // 读取 IMU
        ImuData data{};
        bool ready = false;

        esp_err_t ret = imu->read(data, ready);

        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "IMU read failed: %s",
                     esp_err_to_name(ret));
            continue;
        }

        if (ready) {
            // 每10次打印一次，避免日志影响采样
            static uint32_t count = 0;

            if (++count % 10 == 0) {
                ESP_LOGI(TAG,
                    "ACC: %.3f %.3f %.3f",
                    data.ax, data.ay, data.az);
            }

            // TODO:
            // 将 data 交给通信模块
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