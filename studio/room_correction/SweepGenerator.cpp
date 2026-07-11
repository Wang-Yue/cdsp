#include "room_correction/SweepGenerator.h"

#include <algorithm>
#include <cmath>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

std::vector<double> SweepGenerator::generate(double f1, double f2, double durationSeconds, int sampleRate,
                                             double fadeInSeconds, double fadeOutSeconds) {
    if (f1 <= 0.0 || f2 <= f1 || durationSeconds <= 0.0 || sampleRate <= 0 ||
        f2 > static_cast<double>(sampleRate) / 2.0) {
        return {};
    }
    size_t n = static_cast<size_t>(std::round(durationSeconds * static_cast<double>(sampleRate)));
    if (n == 0)
        return {};

    double actualT = static_cast<double>(n) / static_cast<double>(sampleRate);
    double r = std::log(f2 / f1) / actualT;
    double k = 2.0 * M_PI * f1 / r;

    std::vector<double> sweep(n, 0.0);
    double invFs = 1.0 / static_cast<double>(sampleRate);

    for (size_t i = 0; i < n; ++i) {
        double t = static_cast<double>(i) * invFs;
        sweep[i] = std::sin(k * (std::exp(r * t) - 1.0));
    }

    size_t inSamples = static_cast<size_t>(std::max(0.0, fadeInSeconds) * static_cast<double>(sampleRate));
    size_t outSamples = static_cast<size_t>(std::max(0.0, fadeOutSeconds) * static_cast<double>(sampleRate));
    applyTapers(sweep, inSamples, outSamples);

    return sweep;
}

std::vector<double> SweepGenerator::inverseFilter(double f1, double f2, double durationSeconds, int sampleRate) {
    std::vector<double> sweep = generate(f1, f2, durationSeconds, sampleRate, 0.0, 0.0);
    size_t n = sweep.size();
    if (n == 0)
        return {};
    double actualT = static_cast<double>(n) / static_cast<double>(sampleRate);
    double r = std::log(f2 / f1) / actualT;
    double invFs = 1.0 / static_cast<double>(sampleRate);

    std::vector<double> inv(n, 0.0);
    for (size_t i = 0; i < n; ++i) {
        double t = static_cast<double>(i) * invFs;
        inv[i] = sweep[n - 1 - i] * std::exp(-r * t);
    }
    return inv;
}

std::pair<std::vector<double>, std::vector<double>>
SweepGenerator::sweepAndInverse(double f1, double f2, double durationSeconds, int sampleRate, double fadeInSeconds,
                                double fadeOutSeconds) {
    auto sw = generate(f1, f2, durationSeconds, sampleRate, fadeInSeconds, fadeOutSeconds);
    auto inv = inverseFilter(f1, f2, durationSeconds, sampleRate);
    return {sw, inv};
}

void SweepGenerator::applyTapers(std::vector<double>& buffer, size_t fadeInSamples, size_t fadeOutSamples) {
    size_t n = buffer.size();
    size_t fIn = std::min(fadeInSamples, n);
    size_t fOut = std::min(fadeOutSamples, n);

    if (fIn > 0) {
        for (size_t i = 0; i < fIn; ++i) {
            double w = 0.5 * (1.0 - std::cos(M_PI * static_cast<double>(i) / static_cast<double>(fIn)));
            buffer[i] *= w;
        }
    }
    if (fOut > 0) {
        for (size_t i = 0; i < fOut; ++i) {
            double w = 0.5 * (1.0 - std::cos(M_PI * static_cast<double>(i) / static_cast<double>(fOut)));
            buffer[n - 1 - i] *= w;
        }
    }
}
