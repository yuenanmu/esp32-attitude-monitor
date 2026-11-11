#pragma once

#include <cstddef> // size_t
#include <cstdint> // uint64_t

#include "esp_err.h"
#include "sample_record.hpp"

constexpr size_t HISTORY_CAPACITY = 1024;
constexpr size_t HISTORY_BATCH_MAX = 32;

struct HistoryInfo {
    size_t size{};
    uint64_t oldest_seq{};
    uint64_t latest_seq{};
    uint64_t overwrite_count{};
};

struct HistoryBatch {
    HistoryInfo info{};
    size_t count{};
    bool gap_before_batch{};
};

// 必须在采集任务启动前调用。
esp_err_t history_init();

esp_err_t history_push(const SampleRecord& record);

// 复制 seq > after_seq 的记录，不删除。
// info 与记录在同一把锁内取得。
esp_err_t history_copy_after(
    uint64_t after_seq,
    SampleRecord* output,
    size_t output_capacity,
    HistoryBatch& batch);
    
void history_self_test();