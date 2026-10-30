cmake_minimum_required(VERSION 3.16)
set(IDF_TARGET esp32s3)
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(storybox)

# ST7789 uses SPI only. LovyanGFX 1.2.7's unused S3 RGB backend calls
# gpio_hal_iomux_func_sel, removed by IDF 5.5. Exclude that backend explicitly
# without modifying generated dependencies. Revisit when adding an RGB panel.
idf_component_get_property(lgfx_target LovyanGFX COMPONENT_LIB)
get_target_property(lgfx_sources ${lgfx_target} SOURCES)
list(FILTER lgfx_sources EXCLUDE REGEX "/esp32s3/Bus_RGB\\.cpp$")
set_property(TARGET ${lgfx_target} PROPERTY SOURCES "${lgfx_sources}")
