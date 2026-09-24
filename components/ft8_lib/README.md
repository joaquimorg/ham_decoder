# ft8_lib

FT8/FT4 decoder library by Kārlis Goba, MIT licence (see LICENSE).

- Source: https://github.com/kgoba/ft8_lib
- Commit: 9fec6ca39886edbf96f4f5e71edc76da5074e871 (2025-08-24)
- Files: `ft8/`, `fft/` (KISS FFT) and `common/monitor.*`, `common/common.h`.

Local changes (marked `[ham_decoder]`):

- `common/monitor.h/.c`: the FFT input/output buffers of `monitor_process()`
  (~30 KB for FT8) were variable-length arrays on the stack; they are now
  allocated once in `monitor_init()`, so the analysis task can feed the
  monitor with its small stack.
- `common/monitor.c`: log level LOG_INFO -> LOG_WARN (no messages at init).
- `esp_psram_alloc.h` (new, force-included by CMakeLists.txt on the board):
  `malloc`/`calloc` prefer PSRAM for the library's ~15 KB buffers.
