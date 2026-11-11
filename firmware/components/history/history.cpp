#include "history.hpp"

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include "esp_log.h"
#include "esp_err.h"

static const char* TAG = "history";

namespace {
// 历史记录缓存,一个结构体数组，大小为 HISTORY_CAPACITY。
SampleRecord records[HISTORY_CAPACITY]{}; 

// 下一次写入的位置。
size_t write_index = 0;

// 当前保存的记录数。
size_t record_count = 0;

uint64_t overwrite_count = 0;

StaticSemaphore_t mutex_storage;
SemaphoreHandle_t history_mutex = nullptr;

class ScopedLock {
public:
    ScopedLock()
    {
        xSemaphoreTake(history_mutex, portMAX_DELAY);
    }

    ~ScopedLock()
    {
        xSemaphoreGive(history_mutex);
    }

    ScopedLock(const ScopedLock&) = delete;
    ScopedLock& operator=(const ScopedLock&) = delete;
};

} // namespace

esp_err_t history_init()
{
    if (history_mutex != nullptr) {
        return ESP_ERR_INVALID_STATE;
    }

    history_mutex = xSemaphoreCreateMutexStatic(&mutex_storage);

    return history_mutex != nullptr
        ? ESP_OK
        : ESP_ERR_NO_MEM;
}

esp_err_t history_push(const SampleRecord& record)
{
    if (history_mutex == nullptr) {
        return ESP_ERR_INVALID_STATE;
    }

    ScopedLock lock;

    // TODO 4.1：
    // 把 record 复制到 records[write_index]。
    records[write_index] = record;

    // TODO 4.2：
    // 如果缓存没满：record_count 加一。
    // 如果已经满了：overwrite_count 加一。
    // record_count 不能超过 HISTORY_CAPACITY。
    if (record_count < HISTORY_CAPACITY) {
        record_count++;
    } else {
        overwrite_count++;
    }

    // TODO 4.3：
    // write_index 前进一格，到末尾后回到 0。
    // 提示：使用 % HISTORY_CAPACITY。
    write_index = (write_index + 1) % HISTORY_CAPACITY; // 循环写入,范围[0,HISTORY_CAPACITY)

    return ESP_OK;
}
esp_err_t history_copy_after(
    uint64_t after_seq,
    SampleRecord* output,
    size_t output_capacity,
    HistoryBatch& batch)
{
    batch = {};

    if (history_mutex == nullptr) {
        return ESP_ERR_INVALID_STATE;
    }

    if (output == nullptr ||
        output_capacity == 0 ||
        output_capacity > HISTORY_BATCH_MAX) {
        return ESP_ERR_INVALID_ARG;
    }

    ScopedLock lock;

    // TODO 4.4：
    // 填写 batch.info.size 和 overwrite_count。
    // 空缓存直接返回 ESP_OK，count 保持为 0。
    batch.info.size = record_count;
    batch.info.overwrite_count = overwrite_count;
    if (record_count == 0) {
        return ESP_OK;
    }
    // TODO 4.5：
    // 计算最旧记录的物理下标：
    // (write_index + HISTORY_CAPACITY - record_count)
    //     % HISTORY_CAPACITY
    //
    // 最新记录下标：
    // (write_index + HISTORY_CAPACITY - 1)
    //     % HISTORY_CAPACITY
    //
    // 填写 oldest_seq、latest_seq。
    size_t oldest_index = (write_index + HISTORY_CAPACITY - record_count) % HISTORY_CAPACITY;
    size_t lastest_index = (write_index + HISTORY_CAPACITY - 1) % HISTORY_CAPACITY;
    batch.info.oldest_seq = records[oldest_index].seq;
    batch.info.latest_seq = records[lastest_index].seq;
    // TODO 4.6：
    // 从最旧到最新遍历。
    // 第 i 条记录的物理下标：
    // (oldest_index + i) % HISTORY_CAPACITY
    //
    // 只复制 seq > after_seq 的记录。
    // 输出满 output_capacity 条就停止。
    // 更新 batch.count。
    for (size_t i = 0; i < record_count && batch.count < output_capacity; i++) {
        size_t index = (oldest_index + i) % HISTORY_CAPACITY;
        if (records[index].seq > after_seq) {
            output[batch.count++] = records[index];
        }
    }

    // TODO 4.7：
    // 根据第 5 步规则填写 gap_before_batch。
    batch.gap_before_batch = batch.count > 0 && batch.info.oldest_seq - 1 > after_seq;

    return ESP_OK;
}

void history_self_test()
{
    // 静态分配，避免占用过多任务栈。
    static SampleRecord output[HISTORY_BATCH_MAX];

    // TODO 6.1：
    // 构造 seq=1 到 HISTORY_CAPACITY+10 的记录。
    // t_us = seq * 10000。  每条调用 history_push()。
    for (size_t seq = 1; seq <= HISTORY_CAPACITY + 10; ++seq) {    // 最终保存[11, 1034]共1024条记录。
        SampleRecord record{};
        record.seq = seq;
        record.t_us = seq * 10000;
        history_push(record);
    }

    HistoryBatch batch{};

    // TODO 6.2：
    // 查询 after_seq=0，复制最多 32 条。
    // 检查返回码。
    esp_err_t err = history_copy_after(0, output, 32, batch);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "history_copy_after failed: %s", esp_err_to_name(err));
        return;
    }
    // TODO 6.3：
    // 检查以下预期，并打印 PASS/FAIL：
    //
    // size == HISTORY_CAPACITY   oldest_seq == 11   latest_seq == HISTORY_CAPACITY + 10
    // overwrite_count == 10
    // count == 32
    // gap_before_batch == true
    // output 中的 seq 为 11～42
    if (batch.info.size == HISTORY_CAPACITY && batch.info.oldest_seq == 11 && batch.info.latest_seq == HISTORY_CAPACITY + 10
        && batch.info.overwrite_count == 10 && batch.count == 32 && batch.gap_before_batch == true) {
        bool pass = true;
        for (size_t i = 0; i < batch.count; ++i) {
            if (output[i].seq != 11 + i) {
                pass = false;
                break;
            }
        }
        if (pass) {
            ESP_LOGI(TAG, "history_self_test: PASS");
        } else {
            ESP_LOGE(TAG, "history_self_test: FAIL (output seq mismatch)");
        }
    } else {
        ESP_LOGE(TAG, "history_self_test: FAIL (batch info mismatch)");
    }
    // TODO 6.4：
    // 再查询 after_seq=10。
    // 第一条仍为 11，但 gap_before_batch 应为 false。
    err = history_copy_after(10, output, 32, batch);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "history_copy_after failed: %s", esp_err_to_name(err));
        return;
    }
    bool pass = batch.count > 0 && output[0].seq == 11 && !batch.gap_before_batch;
    if (pass) {
        ESP_LOGI(TAG, "history_self_test: PASS (after_seq=10)");
    } else {
        ESP_LOGE(TAG, "history_self_test: FAIL (after_seq=10)");
    }
    // TODO 6.5：
    // 连续两次用相同 after_seq 查询。
    // 没有并发写入时，结果应相同，证明查询不删除。
    err = history_copy_after(10, output, 32, batch);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "history_copy_after failed: %s", esp_err_to_name(err));
        return;
    }

    // TODO 6.6：
    // 查询 after_seq=HISTORY_CAPACITY+10。
    // 应返回 0 条。
    err = history_copy_after(HISTORY_CAPACITY + 10, output, 32, batch);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "history_copy_after failed: %s", esp_err_to_name(err));
        return;
    }
    if (batch.count == 0) {
        ESP_LOGI(TAG, "history_self_test: PASS (no records after HISTORY_CAPACITY+10)");
    } else {
        ESP_LOGE(TAG, "history_self_test: FAIL (unexpected records after HISTORY_CAPACITY+10)");
    }
}