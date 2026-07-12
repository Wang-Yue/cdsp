#include "room_correction/MeasurementFFT.h"

#include <cmath>
#include <stdexcept>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

bool MeasurementFFT::isPowerOfTwo(size_t n) {
    return n > 0 && (n & (n - 1)) == 0;
}

size_t MeasurementFFT::nextPowerOfTwo(size_t n) {
    size_t p = 1;
    while (p < n)
        p <<= 1;
    return p;
}

void MeasurementFFT::fftComplex(const std::vector<std::complex<double>>& input,
                                std::vector<std::complex<double>>& output, bool isInverse) {
    size_t n = input.size();
    if (!isPowerOfTwo(n)) {
        size_t p = nextPowerOfTwo(n);
        std::vector<std::complex<double>> padded = input;
        padded.resize(p, 0.0);
        fftComplex(padded, output, isInverse);
        return;
    }

    output = input;

    // Bit reversal permutation
    size_t j = 0;
    for (size_t i = 0; i < n - 1; ++i) {
        if (i < j)
            std::swap(output[i], output[j]);
        size_t k = n >> 1;
        while (k <= j) {
            j -= k;
            k >>= 1;
        }
        j += k;
    }

    // Cooley-Tukey radix-2 iteration
    for (size_t len = 2; len <= n; len <<= 1) {
        double angle = 2.0 * M_PI / static_cast<double>(len) * (isInverse ? 1.0 : -1.0);
        std::complex<double> wlen(std::cos(angle), std::sin(angle));
        for (size_t i = 0; i < n; i += len) {
            std::complex<double> w(1.0, 0.0);
            for (size_t k = 0; k < len / 2; ++k) {
                std::complex<double> u = output[i + k];
                std::complex<double> v = output[i + k + len / 2] * w;
                output[i + k] = u + v;
                output[i + k + len / 2] = u - v;
                w *= wlen;
            }
        }
    }

    if (isInverse) {
        for (size_t i = 0; i < n; ++i) {
            output[i] /= static_cast<double>(n);
        }
    }
}

void MeasurementFFT::forward(const std::vector<double>& realInput, std::vector<double>& outReal,
                             std::vector<double>& outImag) {
    size_t n = realInput.size();
    if (!isPowerOfTwo(n)) {
        size_t p = nextPowerOfTwo(n);
        std::vector<double> padded = realInput;
        padded.resize(p, 0.0);
        forward(padded, outReal, outImag);
        return;
    }

    std::vector<std::complex<double>> cIn(n);
    for (size_t i = 0; i < n; ++i)
        cIn[i] = realInput[i];

    std::vector<std::complex<double>> cOut;
    fftComplex(cIn, cOut, false);

    size_t bins = n / 2 + 1;
    outReal.resize(bins);
    outImag.resize(bins);

    for (size_t k = 0; k < bins; ++k) {
        outReal[k] = cOut[k].real();
        outImag[k] = cOut[k].imag();
    }
}

void MeasurementFFT::inverse(const std::vector<double>& inReal, const std::vector<double>& inImag,
                             std::vector<double>& outReal) {
    if (inReal.empty() || inImag.size() < inReal.size()) {
        outReal.clear();
        return;
    }
    size_t bins = inReal.size();
    size_t n = (bins - 1) * 2;

    std::vector<std::complex<double>> cIn(n);
    cIn[0] = std::complex<double>(inReal[0], 0.0);
    cIn[bins - 1] = std::complex<double>(inReal[bins - 1], 0.0);

    for (size_t k = 1; k < bins - 1; ++k) {
        cIn[k] = std::complex<double>(inReal[k], inImag[k]);
        cIn[n - k] = std::complex<double>(inReal[k], -inImag[k]);
    }

    std::vector<std::complex<double>> cOut;
    fftComplex(cIn, cOut, true);

    outReal.resize(n);
    for (size_t i = 0; i < n; ++i) {
        outReal[i] = cOut[i].real();
    }
}
