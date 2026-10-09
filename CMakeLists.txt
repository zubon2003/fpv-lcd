cmake_minimum_required(VERSION 3.16)

# S2 uses the ROM USB-OTG CDC driver; keep its pinned compatibility fix
# scoped to this target so other chips retain their SDK components.
if(IDF_TARGET STREQUAL "esp32s2")
    list(APPEND EXTRA_COMPONENT_DIRS "${CMAKE_CURRENT_LIST_DIR}/platform/esp32s2/esp_usb_cdc_rom_console")
endif()

option(ESP_SDR_STREAMING "Build the S31 Ethernet/high-speed USB receiver" OFF)
# Stand-alone LCD band monitor (ESP32-C5): idf.py -D FPV_LCD_MONITOR=ON ...
option(FPV_LCD_MONITOR "Build the stand-alone FPV band monitor on an ILI9341 LCD" OFF)
# Its display: ILI9341 (2.8" 320x240) or RESTOUCH35 (Waveshare Pico-ResTouch-LCD-3.5, 480x320)
set(FPV_PANEL "ILI9341" CACHE STRING "FPV monitor display: ILI9341 or RESTOUCH35")
if(ESP_SDR_STREAMING)
    if(NOT IDF_TARGET STREQUAL "esp32s31")
        message(FATAL_ERROR "ESP_SDR_STREAMING requires ESP32-S31")
    endif()
    list(APPEND EXTRA_COMPONENT_DIRS "${CMAKE_CURRENT_LIST_DIR}/main/targets/esp32s31/streaming")
else()
    set(ENV{IDF_COMPONENT_MANAGER} "0")
endif()
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
idf_build_set_property(MINIMAL_BUILD ON)
idf_build_set_property(ESP_SDR_STREAMING "${ESP_SDR_STREAMING}")
idf_build_set_property(FPV_LCD_MONITOR "${FPV_LCD_MONITOR}")
idf_build_set_property(FPV_PANEL "${FPV_PANEL}")
project(esp_sdr)
