# This file was automatically generated for projects
# without default 'CMakeLists.txt' file.

FILE(GLOB_RECURSE app_sources ${CMAKE_SOURCE_DIR}/src/*.*)

idf_component_register(SRCS ${app_sources}
                       INCLUDE_DIRS "../include"
                       PRIV_REQUIRES ft8_lib esp_adc esp_driver_i2s esp_wifi esp_netif esp_event nvs_flash esp_http_server esp_timer
                                     esp_lcd esp_driver_spi esp_driver_i2c esp_driver_ledc esp_driver_gpio ui_fonts)

# JS8 text dictionary (JSC, from JS8Call; tools/js8/gen_tables.py), in flash.
# PlatformIO also needs it in board_build.embed_files (platformio.ini).
target_add_binary_data(${COMPONENT_TARGET} "../data/jsc_dict.bin" BINARY)

# Board selected by platformio.ini (-DRX_BOARD=...); none = DevKitC.
if(RX_BOARD)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE BOARD_${RX_BOARD})
endif()
# Audio input selected by platformio.ini (-DRX_AUDIO=ADC|PCM1808|ES8311); none = config.h.
if(RX_AUDIO)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE RX_AUDIO_${RX_AUDIO})
endif()
# -DRX_NO_MONITOR=1: no monitor output on the speaker.
if(RX_NO_MONITOR)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE RX_NO_MONITOR)
endif()
