#include "comm.hpp"

#include <cstdio>
#include <cstring>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

namespace {

constexpr size_t COMMAND_CAPACITY = 128;

TaskHandle_t comm_task_handle = nullptr;

// 发送一行响应。
void send_line(const char* text)
{
    std::printf("%s\n", text); // 调用者传入的 text 不需要自带换行。
    std::fflush(stdout); // 刷新 stdout
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
    if (std::strcmp(command, "PING") == 0) {
        send_line("PONG");
        return;
    }

    // TODO 2：
    // 把默认响应改为 "ERR UNKNOWN_COMMAND"。
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