idf_component_register(SRCS "src/app.cpp" "src/app_theme.cpp" "src/pages_home.cpp"
    "src/pages_settings.cpp" "src/pages_music.cpp" "src/pages_story.cpp" "src/pages_video.cpp"
    "src/pages_parental.cpp" "src/player_view.cpp"
    INCLUDE_DIRS "include"
    REQUIRES display input storage media audio settings board video ble hostlink nfc power ota
    bootloader_support esp_partition app_update fatfs spi_flash esp_timer)
