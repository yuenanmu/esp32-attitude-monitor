idf_component_register(SRCS "board_i2c.cpp"
                    INCLUDE_DIRS "include"                    
                    REQUIRES esp_driver_i2c
                    PRIV_REQUIRES esp_driver_gpio)