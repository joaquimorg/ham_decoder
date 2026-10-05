#include "fft.h"

#pragma GCC optimize("O2")

#include <math.h>
#include <stdlib.h>

#ifdef ESP_PLATFORM
#include "esp_heap_caps.h"
#endif

bool ComplexFft::init(int size)
{
    if (size < 2 || (size & (size - 1)))
        return false;
#ifdef ESP_PLATFORM
    cos_t = (float *)heap_caps_malloc(size / 2 * sizeof(float), MALLOC_CAP_SPIRAM);
    sin_t = (float *)heap_caps_malloc(size / 2 * sizeof(float), MALLOC_CAP_SPIRAM);
#else
    cos_t = (float *)malloc(size / 2 * sizeof(float));
    sin_t = (float *)malloc(size / 2 * sizeof(float));
#endif
    if (!cos_t || !sin_t)
        return false;
    n = size;
    for (int k = 0; k < n / 2; k++) {
        cos_t[k] = cosf(2.0f * (float)M_PI * k / n);
        sin_t[k] = sinf(2.0f * (float)M_PI * k / n);
    }
    return true;
}

void ComplexFft::run(float *xr, float *xi) const
{
    for (int i = 1, j = 0; i < n; i++) {
        int bit = n >> 1;
        for (; j & bit; bit >>= 1)
            j ^= bit;
        j ^= bit;
        if (i < j) {
            float t = xr[i]; xr[i] = xr[j]; xr[j] = t;
            t = xi[i]; xi[i] = xi[j]; xi[j] = t;
        }
    }
    for (int len = 2; len <= n; len <<= 1) {
        const int h = len / 2;
        const int step = n / len;
        for (int i = 0; i < n; i += len) {
            for (int k = 0; k < h; k++) {
                const float wr = cos_t[k * step];
                const float wi = -sin_t[k * step];
                const int a = i + k;
                const int b = a + h;
                const float tr = xr[b] * wr - xi[b] * wi;
                const float ti = xr[b] * wi + xi[b] * wr;
                xr[b] = xr[a] - tr;
                xi[b] = xi[a] - ti;
                xr[a] += tr;
                xi[a] += ti;
            }
        }
    }
}
