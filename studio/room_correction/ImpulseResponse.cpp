#include "room_correction/ImpulseResponse.h"

#include <algorithm>
#include <cmath>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

ImpulseResponse::ImpulseResponse(const std::vector<double>& samples, int sampleRate, size_t zeroIndex)
    : samples(samples), sampleRate(sampleRate), zeroIndex(zeroIndex) {}

size_t ImpulseResponse::peakIndex() const {
    if (samples.empty())
        return 0;
    size_t best = 0;
    double bestVal = std::abs(samples[0]);
    for (size_t i = 1; i < samples.size(); ++i) {
        double v = std::abs(samples[i]);
        if (v > bestVal) {
            bestVal = v;
            best = i;
        }
    }
    return best;
}

double ImpulseResponse::peakValue() const {
    if (samples.empty())
        return 0.0;
    return samples[peakIndex()];
}

ImpulseResponse ImpulseResponse::centeredOnPeak() const {
    ImpulseResponse out = *this;
    out.zeroIndex = peakIndex();
    return out;
}

ImpulseResponse ImpulseResponse::windowed(size_t leftSamples, size_t rightSamples, double taperFraction) const {
    size_t n = leftSamples + rightSamples;
    std::vector<double> out(n, 0.0);

    int srcStart = static_cast<int>(zeroIndex) - static_cast<int>(leftSamples);
    for (size_t i = 0; i < n; ++i) {
        int src = srcStart + static_cast<int>(i);
        if (src >= 0 && static_cast<size_t>(src) < samples.size()) {
            out[i] = samples[src];
        }
    }

    size_t leftTaper = static_cast<size_t>(static_cast<double>(leftSamples) * taperFraction);
    size_t rightTaper = static_cast<size_t>(static_cast<double>(rightSamples) * taperFraction);

    if (leftTaper > 0) {
        for (size_t i = 0; i < std::min(leftTaper, n); ++i) {
            double w = 0.5 * (1.0 - std::cos(M_PI * static_cast<double>(i) / static_cast<double>(leftTaper)));
            out[i] *= w;
        }
    }

    if (rightTaper > 0) {
        for (size_t i = 0; i < std::min(rightTaper, n); ++i) {
            double w = 0.5 * (1.0 - std::cos(M_PI * static_cast<double>(i) / static_cast<double>(rightTaper)));
            out[n - 1 - i] *= w;
        }
    }

    return ImpulseResponse(out, sampleRate, leftSamples);
}

std::vector<double> ImpulseResponse::schroederDecay() const {
    if (samples.empty())
        return {};

    size_t p = zeroIndex;
    if (p >= samples.size())
        return {};

    size_t n = samples.size() - p;
    std::vector<double> energy(n, 0.0);
    double sum = 0.0;

    for (size_t i = n; i > 0; --i) {
        double s = samples[p + i - 1];
        sum += s * s;
        energy[i - 1] = sum;
    }

    if (sum <= 0.0)
        return {};
    double invTotal = 1.0 / sum;

    std::vector<double> decayDB(n);
    for (size_t i = 0; i < n; ++i) {
        double ratio = std::max(energy[i] * invTotal, 1e-12);
        decayDB[i] = 10.0 * std::log10(ratio);
    }

    return decayDB;
}

double ImpulseResponse::rt60(double startDB, double endDB) const {
    auto decay = schroederDecay();
    if (decay.size() <= 1)
        return 0.0;

    int idxStart = -1;
    int idxEnd = -1;

    for (size_t i = 0; i < decay.size(); ++i) {
        if (idxStart < 0 && decay[i] <= startDB)
            idxStart = static_cast<int>(i);
        if (idxEnd < 0 && decay[i] <= endDB)
            idxEnd = static_cast<int>(i);
    }

    if (idxStart < 0 || idxEnd < 0 || idxEnd <= idxStart)
        return 0.0;

    double dt = static_cast<double>(idxEnd - idxStart) / static_cast<double>(sampleRate);
    double dDb = decay[idxStart] - decay[idxEnd];
    if (dDb <= 0.0)
        return 0.0;

    return dt * (60.0 / dDb);
}

RT60Result ImpulseResponse::estimateRT60() const {
    RT60Result res;
    auto decay = schroederDecay();
    if (decay.size() < 100)
        return res;

    double fs = static_cast<double>(sampleRate);

    auto fitSlope = [&decay, fs](double startDB, double endDB) -> double {
        size_t idxStart = decay.size();
        size_t idxEnd = decay.size();

        for (size_t i = 0; i < decay.size(); ++i) {
            if (decay[i] <= startDB && idxStart == decay.size())
                idxStart = i;
            if (decay[i] <= endDB && idxEnd == decay.size())
                idxEnd = i;
        }

        if (idxStart >= idxEnd || idxEnd >= decay.size())
            return 0.0;

        double tStart = static_cast<double>(idxStart) / fs;
        double tEnd = static_cast<double>(idxEnd) / fs;
        double dt = tEnd - tStart;
        if (dt <= 0.0)
            return 0.0;

        double dDb = decay[idxStart] - decay[idxEnd];
        if (dDb <= 0.0)
            return 0.0;

        return dt * (60.0 / dDb);
    };

    res.edt = fitSlope(0.0, -10.0);
    res.t20 = fitSlope(-5.0, -25.0);
    res.t30 = fitSlope(-5.0, -35.0);

    return res;
}
