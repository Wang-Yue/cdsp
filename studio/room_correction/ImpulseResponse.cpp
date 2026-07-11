#include "room_correction/ImpulseResponse.h"
#include <cmath>
#include <algorithm>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

ImpulseResponse::ImpulseResponse(const std::vector<double>& samples, int sampleRate)
    : samples(samples), sampleRate(sampleRate) {}

size_t ImpulseResponse::peakIndex() const {
    if (samples.empty()) return 0;
    size_t idx = 0;
    double maxVal = -1.0;
    for (size_t i = 0; i < samples.size(); ++i) {
        double v = std::abs(samples[i]);
        if (v > maxVal) {
            maxVal = v;
            idx = i;
        }
    }
    return idx;
}

double ImpulseResponse::peakValue() const {
    if (samples.empty()) return 0.0;
    return samples[peakIndex()];
}

ImpulseResponse ImpulseResponse::windowed(size_t leftSamples, size_t rightSamples, double taperFraction) const {
    if (samples.empty()) return *this;

    size_t peak = peakIndex();
    size_t start = (peak > leftSamples) ? (peak - leftSamples) : 0;
    size_t end = std::min(samples.size(), peak + rightSamples);
    size_t winLen = end - start;

    std::vector<double> winSamples(winLen);
    size_t leftTaper = static_cast<size_t>(leftSamples * taperFraction);
    size_t rightTaper = static_cast<size_t>(rightSamples * taperFraction);

    for (size_t i = 0; i < winLen; ++i) {
        size_t srcIdx = start + i;
        double w = 1.0;

        if (i < leftTaper && leftTaper > 0) {
            w = 0.5 * (1.0 - std::cos(M_PI * static_cast<double>(i) / static_cast<double>(leftTaper)));
        } else if (i >= winLen - rightTaper && rightTaper > 0) {
            size_t dist = winLen - 1 - i;
            w = 0.5 * (1.0 - std::cos(M_PI * static_cast<double>(dist) / static_cast<double>(rightTaper)));
        }

        winSamples[i] = samples[srcIdx] * w;
    }

    return ImpulseResponse(winSamples, sampleRate);
}

std::vector<double> ImpulseResponse::schroederDecay() const {
    if (samples.empty()) return {};

    size_t n = samples.size();
    std::vector<double> decay(n);
    double sumPow = 0.0;

    for (size_t i = n; i > 0; --i) {
        double v = samples[i - 1];
        sumPow += v * v;
        decay[i - 1] = sumPow;
    }

    double total = decay[0];
    if (total <= 0.0) return std::vector<double>(n, -100.0);

    std::vector<double> decayDB(n);
    for (size_t i = 0; i < n; ++i) {
        double norm = decay[i] / total;
        decayDB[i] = (norm > 0.0) ? 10.0 * std::log10(norm) : -100.0;
    }

    return decayDB;
}

RT60Result ImpulseResponse::estimateRT60() const {
    RT60Result res;
    auto decay = schroederDecay();
    if (decay.size() < 100) return res;

    double fs = static_cast<double>(sampleRate);

    // Linear regression helper
    auto fitSlope = [&decay, fs](double startDB, double endDB) -> double {
        size_t idxStart = decay.size();
        size_t idxEnd = decay.size();

        for (size_t i = 0; i < decay.size(); ++i) {
            if (decay[i] <= startDB && idxStart == decay.size()) idxStart = i;
            if (decay[i] <= endDB && idxEnd == decay.size()) idxEnd = i;
        }

        if (idxStart >= idxEnd || idxEnd >= decay.size()) return 0.0;

        double tStart = static_cast<double>(idxStart) / fs;
        double tEnd = static_cast<double>(idxEnd) / fs;
        double dt = tEnd - tStart;
        if (dt <= 0.0) return 0.0;

        double dbDiff = endDB - startDB;
        double slope = dbDiff / dt; // dB / sec
        return (slope < 0.0) ? (-60.0 / slope) : 0.0;
    };

    res.edt = fitSlope(0.0, -10.0);
    res.t20 = fitSlope(-5.0, -25.0);
    res.t30 = fitSlope(-5.0, -35.0);

    return res;
}
