idf_component_register(SRCS "../../src/pn532.cpp" "../../src/nfc.cpp" "nfc_service.cpp" INCLUDE_DIRS "../../src" "include"
    REQUIRES esp_driver_i2c esp_driver_gpio esp_timer)
