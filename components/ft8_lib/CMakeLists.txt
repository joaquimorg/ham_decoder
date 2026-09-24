# ft8_lib (FT8/FT4 decoding), vendored - see README.md.
idf_component_register(SRCS "ft8/constants.c" "ft8/crc.c" "ft8/decode.c" "ft8/encode.c"
                            "ft8/ldpc.c" "ft8/message.c" "ft8/text.c"
                            "common/monitor.c" "fft/kiss_fft.c" "fft/kiss_fftr.c"
                       INCLUDE_DIRS "."
                       PRIV_REQUIRES heap)
# Third-party code: keep its warnings out of the -Werror build. Its buffers go
# to PSRAM (esp_psram_alloc.h).
target_compile_options(${COMPONENT_LIB} PRIVATE -w -O2
                       -include ${CMAKE_CURRENT_LIST_DIR}/esp_psram_alloc.h)
