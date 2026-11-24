idf_component_register(SRCS "../../src/pn532.cpp" "../../src/nfc.cpp" "nfc_service.cpp" INCLUDE_DIRS "../../src" "include"
    REQUIRES esp_driver_i2c esp_driver_gpio esp_timer)
# pn532.cpp reads config.h, which keeps PN532_RST_PIN overridable. The Kconfig
# value is opt-in (default -1) so a board without a wired RSTPD is unchanged.
if(DEFINED CONFIG_STORYBOX_NFC_RESET_GPIO AND
   NOT "${CONFIG_STORYBOX_NFC_RESET_GPIO}" STREQUAL "-1")
    target_compile_definitions(${COMPONENT_LIB} PRIVATE PN532_RST_PIN=${CONFIG_STORYBOX_NFC_RESET_GPIO})
endif()
if(DEFINED CONFIG_STORYBOX_NFC_POLL_MS)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE NFC_POLL_INTERVAL_MS=${CONFIG_STORYBOX_NFC_POLL_MS})
endif()
