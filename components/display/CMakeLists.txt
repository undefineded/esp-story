idf_component_register(SRCS "../../src/display.cpp" "../../src/fonts.cpp" "story_image.cpp"
    INCLUDE_DIRS "../../src" "include"
    REQUIRES LovyanGFX esp_timer esp_psram esp_driver_ledc esp_driver_gpio storage)
