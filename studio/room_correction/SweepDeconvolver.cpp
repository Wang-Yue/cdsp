#include "room_correction/SweepDeconvolver.h"
#include "room_correction/SweepGenerator.h"
#include "room_correction/MeasurementFFT.h"
#include <cmath>
#include <algorithm>

std::vector<double> SweepDeconvolver::convolve(const std::vector<double>& a, const std::vector<double>& b) {
    if (a.empty() || b.empty()) return {};

    size_t totalLen = a.size() + b.size() - 1;
    size_t nFft = 1;
    while (nFft < totalLen) nFft <<= 1;

    std::vector<double> paddedA = a; paddedA.resize(nFft, 0.0);
    std::vector<double> paddedB = b; paddedB.resize(nFft, 0.0);

    std::vector<double> aReal, aImag, bReal, bImag;
    MeasurementFFT::forward(paddedA, aReal, aImag);
    MeasurementFFT::forward(paddedB, bReal, bImag);

    size_t bins = aReal.size();
    std::vector<double> cReal(bins), cImag(bins);

    for (size_t k = 0; k < bins; ++k) {
        cReal[k] = aReal[k] * bReal[k] - aImag[k] * bImag[k];
        cImag[k] = aReal[k] * bImag[k] + aImag[k] * bReal[k];
    }

    std::vector<double> outReal;
    MeasurementFFT::inverse(cReal, cImag, outReal);
    outReal.resize(totalLen);

    return outReal;
}

ImpulseResponse SweepDeconvolver::deconvolve(
    const std::vector<double>& captured,
    double f1,
    double f2,
    double durationSeconds,
    int sampleRate
) {
    std::vector<double> invFilter = SweepGenerator::inverseFilter(f1, f2, durationSeconds, sampleRate);
    std::vector<double> rawIR = convolve(captured, invFilter);

    // The Farina deconvolution peak lands at (inverse.size() - 1 + sweepStart)
    // Align impulse response peak
    size_t alignShift = invFilter.size() - 1;
    std::vector<double> aligned;
    if (rawIR.size() > alignShift) {
        aligned.assign(rawIR.begin() + alignShift, rawIR.end());
    } else {
        aligned = rawIR;
    }

    return ImpulseResponse(aligned, sampleRate);
}
