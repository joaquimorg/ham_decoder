// QRSS grabber: a tone off the centre must land in the right bin, a tone
// outside the view must not alias into it.
#include <math.h>
#include <stdint.h>
#include <stdio.h>

#include "qrss.h"
#include "ui_hub.h"

static int cols = 0, peak_bin = -1;
static float peak_db = 0.0f, rest_db = 0.0f;

void ui_push_qrss_column(const uint8_t *col)
{
    int b = 0;
    double sum = 0;
    for (int i = 0; i < UI_QRSS_BINS; i++) {
        if (col[i] > col[b])
            b = i;
        sum += col[i];
    }
    cols++;
    peak_bin = b;
    peak_db = col[b] / 2.0f - 140.0f;
    rest_db = (float)(sum / UI_QRSS_BINS) / 2.0f - 140.0f;
}

static void run(float off_hz, float amp)
{
    qrss_init();
    qrss_set_center(1400.0f);
    static float block[FFT_SIZE];
    float ph = 0.0f;
    for (int b = 0; b < 60; b++) {
        for (int i = 0; i < FFT_SIZE; i++) {
            ph += 2.0f * (float)M_PI * (1400.0f + off_hz) / DSP_SAMPLE_RATE;
            if (ph > 2.0f * (float)M_PI)
                ph -= 2.0f * (float)M_PI;
            block[i] = amp * sinf(ph);
        }
        qrss_process(block, FFT_SIZE);
    }
    const int want = UI_QRSS_BINS / 2 + (int)lroundf(off_hz / qrss_bin_hz());
    printf("tom %+6.1f Hz: %d colunas, pico no bin %d (esperado %d) %.1f dB, media %.1f dB\n", off_hz, cols,
           peak_bin, want, peak_db, rest_db);
}

int main()
{
    run(10.0f, 0.1f);
    run(-30.0f, 0.1f);
    run(120.0f, 0.1f);    // outside the view: only leakage
    return 0;
}
