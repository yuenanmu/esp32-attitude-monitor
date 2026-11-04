#pragma once 

#include "esp_err.h"

class Imu;

esp_err_t imu_sampling_start(Imu* imu);