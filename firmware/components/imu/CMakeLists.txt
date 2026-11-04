idf_component_register(
    SRCS "imu.cpp"
    INCLUDE_DIRS "include"
    REQUIRES esp_driver_i2c  waveshare__qmi8658
    PRIV_REQUIRES log
)