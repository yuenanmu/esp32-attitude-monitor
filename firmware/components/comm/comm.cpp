#include "comm.hpp"
#include <cinttypes>
#include "history.hpp"

#include <cstdio>
#include <cstring>

#include <cerrno>
#include <cstdlib>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

namespace {

constexpr size_t COMMAND_CAPACITY = 128;

TaskHandle_t comm_task_handle = nullptr;

bool parse_uint64(const char* text, uint64_t& value);

// 发送一行响应。
void send_line(const char* text)
{
    std::printf("%s\n", text); // 调用者传入的 text 不需要自带换行。
    std::fflush(stdout); // 刷新 stdout
}

void send_samples(uint64_t after_seq, size_t limit)
{
    // 只由当前通信任务使用。
    static SampleRecord output[HISTORY_BATCH_MAX];

    HistoryBatch batch{};

    esp_err_t err = history_copy_after(
        after_seq,
        output,
        limit,
        batch
    );

    if (err != ESP_OK) {
        send_line("ERR HISTORY_READ_FAILED");
        return;
    }

    // history_copy_after 返回时已经释放缓存锁。
    std::printf(
        "BEGIN %" PRIu64 " %zu %" PRIu64
        " %" PRIu64 " %" PRIu64 " %u\n",
        after_seq,
        batch.count,
        batch.info.oldest_seq,
        batch.info.latest_seq,
        batch.info.overwrite_count,
        batch.gap_before_batch ? 1U : 0U
    );

    uint64_t last_seq = after_seq;

    for (size_t i = 0; i < batch.count; ++i) {
        const SampleRecord& record = output[i];

        // TODO 3.1
        // 用一次 printf 输出一条完整 DATA 行。
        //
        // 格式参考：
        // "DATA %" PRIu64 " %" PRId64
        // " %.6f %.6f %.6f %.6f %.6f %.6f\n"
        //
        // 依次填入：
        // record.seq、record.t_us
        // record.imu.ax/ay/az/gx/gy/gz
        std::printf(
            "DATA %" PRIu64 " %" PRIu64
            " %.6f %.6f %.6f %.6f %.6f %.6f\n",
            record.seq, record.t_us,
            record.imu.ax, record.imu.ay, record.imu.az,
            record.imu.gx, record.imu.gy, record.imu.gz
        );
        // TODO 3.2
        // 更新 last_seq，记录本批最后发送的序号。
        last_seq = record.seq;
    }

    std::printf("END %" PRIu64 "\n", last_seq);
    std::fflush(stdout);
}

// 处理一条完整命令。
// 收到这里时，末尾的换行已经去掉。
void handle_command(const char* command)
{
    // TODO 1：
    // 判断 command 是否等于 "PING"。
    // 提示：std::strcmp(command, "PING") == 0
    //
    // 如果匹配：
    //   调用 send_line() 返回 "PONG"；
    //   然后 return，避免继续执行下面的错误响应。
    printf("Received command: %s\n", command); // 调试输出，显示收到的命令
    std::fflush(stdout); // 确保调试输出立即显示
    if (std::strcmp(command, "PING") == 0) {
        send_line("PONG");
        return;
    }

    char verb[128]{}; // 128个字符，留一个位置给 '\0'，接受127个字符
    char after_text[128]{};
    char limit_text[128]{};
    char extra[128]{};

    int fields = std::sscanf( // 指令+参数（空格分隔参数）
        command,
        "%127s %127s %127s %127s",
        verb,
        after_text,
        limit_text,
        extra
    );

    if (std::strcmp(verb, "GET_SAMPLES") == 0) {
        uint64_t after_seq = 0;
        uint64_t limit = 0;

        if (fields != 3 || !parse_uint64(after_text, after_seq)
            || !parse_uint64(limit_text, limit)
            || limit < 1 || limit > HISTORY_BATCH_MAX) {
            send_line("ERR INVALID_ARGUMENT");
            return;
        }
        // 从history缓存中获取 after_seq 之后的记录，最多 limit 条。并且发送到 stdout。
        send_samples(after_seq, limit);
        return;
    }

    send_line("ERR UNKNOWN_COMMAND");
    return;
}

void comm_task(void* arg)
{
    (void)arg;

    char command[COMMAND_CAPACITY]{};
    size_t length = 0;
    bool discarding = false;

    while (true) {
        int ch = std::getchar();

        // 当前没有可读字符时，让出 CPU，稍后继续。
        if (ch == EOF) {
            std::clearerr(stdin);
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }

        // 支持常见的 CRLF 行结束格式。
        if (ch == '\r') {
            continue;
        }

        // 遇到换行，才认为收到一条完整命令。
        if (ch == '\n') {
            if (discarding) {
                send_line("ERR COMMAND_TOO_LONG");
            } else if (length > 0) {
                command[length] = '\0';
                handle_command(command);
            }

            length = 0;
            discarding = false;
            continue;
        }

        // 超长命令的剩余部分一直丢弃到换行，
        // 避免把后半截误当成下一条命令。
        if (discarding) {
            continue;
        }

        // 留一个位置存放字符串结束符 '\0'。
        if (length >= sizeof(command) - 1) {
            discarding = true;
            continue;
        }

        command[length++] = static_cast<char>(ch);
    }
}
// 解析字符串为 uint64_t。
bool parse_uint64(const char* text, uint64_t& value) {
    if (text == nullptr || *text == '\0') { // '\0' 为空字符串
        return false;
    }
    for (const char* p = text; *p != '\0'; ++p) {
        if (*p < '0' || *p > '9') {
            return false; // 非数字字符
        }
    }
    errno = 0;
    char* end = nullptr;
    unsigned long long parsed = std::strtoull(text, &end, 10);
    if (errno == ERANGE || end == text || *end != '\0') {
        return false; // 转换错误或未完全转换
    }
    value = static_cast<uint64_t>(parsed);
    return true;
}

} // namespace

esp_err_t comm_start()
{
    if (comm_task_handle != nullptr) {
        return ESP_ERR_INVALID_STATE;
    }

    BaseType_t result = xTaskCreate(
        comm_task,
        "comm_task",
        4096,
        nullptr,
        4,
        &comm_task_handle
    );

    if (result != pdPASS) {
        comm_task_handle = nullptr;
        return ESP_ERR_NO_MEM;
    }

    return ESP_OK;
}