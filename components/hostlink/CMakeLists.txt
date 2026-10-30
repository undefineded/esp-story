idf_component_register(SRCS "protocol.cpp" "usb_transport.cpp" INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "../../src" REQUIRES json storage esp_driver_usb_serial_jtag esp_timer)
