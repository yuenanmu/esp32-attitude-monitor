idf_component_register(
    SRCS "history.cpp"
    INCLUDE_DIRS "include"
    REQUIRES imu
    PRIV_REQUIRES freertos
)