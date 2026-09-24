// [ham_decoder] Force-included on the board by CMakeLists.txt: ft8_lib's
// buffers (waterfalls, FFT frames: ~15 KB each, just under the size malloc()
// already sends to PSRAM) go to PSRAM, falling back to internal RAM.
#pragma once
#include <stdlib.h>
#include "esp_heap_caps.h"
#define malloc(n)    heap_caps_malloc_prefer((n), 2, MALLOC_CAP_SPIRAM, MALLOC_CAP_DEFAULT)
#define calloc(n, m) heap_caps_calloc_prefer((n), (m), 2, MALLOC_CAP_SPIRAM, MALLOC_CAP_DEFAULT)
