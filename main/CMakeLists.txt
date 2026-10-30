idf_component_register(SRCS "app_main.cpp" "diagnostics.cpp" INCLUDE_DIRS "."
    REQUIRES esp_system heap spi_flash esp_psram board input storage audio display media settings connectivity hostlink nfc)
