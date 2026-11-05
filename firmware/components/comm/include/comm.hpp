#pragma once

#include "esp_err.h"

// 启动通信任务，只允许启动一次。
esp_err_t comm_start();