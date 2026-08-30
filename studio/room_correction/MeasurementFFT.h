#ifndef MEASUREMENT_FFT_H
#define MEASUREMENT_FFT_H

#include <complex>  // for complex
#include <stddef.h> // for size_t
#include <vector>   // for vector

class MeasurementFFT {
public:
    // Forward FFT: real signal (N samples) -> complex spectrum (N/2 + 1 bins)
    // N must be a power of 2.
    static void forward(const std::vector<double>& realInput, std::vector<double>& outReal,
                        std::vector<double>& outImag);

    // Inverse FFT: complex spectrum (N/2 + 1 bins) -> real signal (N samples)
    static void inverse(const std::vector<double>& realInput, const std::vector<double>& imagInput,
                        std::vector<double>& outReal);

    // Complex FFT: N complex input -> N complex output
    static void fftComplex(const std::vector<std::complex<double>>& input, std::vector<std::complex<double>>& output,
                           bool inverse = false);

private:
    static bool isPowerOfTwo(size_t n);
    static size_t nextPowerOfTwo(size_t n);
};

#endif // MEASUREMENT_FFT_H
