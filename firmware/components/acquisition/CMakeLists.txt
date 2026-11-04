idf_component_register(
    SRCS "imu_task.cpp"
    INCLUDE_DIRS "include"
    PRIV_REQUIRES imu esp_timer freertos log
)