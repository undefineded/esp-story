idf_component_register(SRCS "fram_device.cpp" "i2c_bus.cpp" INCLUDE_DIRS "include"
    REQUIRES esp_driver_i2c)
