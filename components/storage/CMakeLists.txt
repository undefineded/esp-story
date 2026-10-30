idf_component_register(SRCS "sd_volume.cpp" "storage_access.cpp" INCLUDE_DIRS "include"
    REQUIRES fatfs sdmmc esp_driver_sdmmc)
