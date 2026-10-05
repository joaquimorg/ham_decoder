#pragma once

// In-place radix-2 complex FFT of a fixed power-of-two size (tables built
// once). For the narrow decoders (QRSS, Olivia/Contestia); the analyzer has
// its own for the 1024-point spectrum.

class ComplexFft {
public:
    // n: power of two. Tables in PSRAM on the board.
    bool init(int n);
    int size() const { return n; }
    // Forward transform (e^-j), not scaled.
    void run(float *re, float *im) const;

private:
    int n = 0;
    float *cos_t = nullptr, *sin_t = nullptr;
};
