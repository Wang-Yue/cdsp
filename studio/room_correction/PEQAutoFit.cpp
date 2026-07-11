#include "room_correction/PEQAutoFit.h"
#include <cmath>
#include <algorithm>
#include <limits>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

std::vector<double> PEQAutoFit::logFrequencyGrid(double fMin, double fMax, size_t count) {
    std::vector<double> grid(count);
    if (count == 0) return grid;
    if (count == 1) { grid[0] = fMin; return grid; }

    double logMin = std::log10(fMin);
    double logMax = std::log10(fMax);
    double step = (logMax - logMin) / static_cast<double>(count - 1);

    for (size_t i = 0; i < count; ++i) {
        grid[i] = std::pow(10.0, logMin + static_cast<double>(i) * step);
    }
    return grid;
}

std::vector<double> PEQAutoFit::smoothLogOctave(const std::vector<double>& magDB, const std::vector<double>& freqs, double octaves) {
    if (magDB.size() != freqs.size() || magDB.empty()) return magDB;
    size_t n = magDB.size();
    std::vector<double> smoothed(n);

    double factor = std::pow(2.0, octaves / 2.0);

    for (size_t i = 0; i < n; ++i) {
        double f = freqs[i];
        double fLow = f / factor;
        double fHigh = f * factor;

        double sum = 0.0;
        int count = 0;
        for (size_t j = 0; j < n; ++j) {
            if (freqs[j] >= fLow && freqs[j] <= fHigh) {
                sum += magDB[j];
                count++;
            }
        }
        smoothed[i] = (count > 0) ? (sum / count) : magDB[i];
    }
    return smoothed;
}

std::vector<double> PEQAutoFit::sampleMagnitudeDB(const FrequencyResponse& fr, const std::vector<double>& atFrequencies) {
    std::vector<double> result(atFrequencies.size(), -100.0);
    if (fr.bins() == 0) return result;

    double binHz = static_cast<double>(fr.sampleRate) / static_cast<double>(fr.fftSize);

    for (size_t i = 0; i < atFrequencies.size(); ++i) {
        double f = atFrequencies[i];
        double binFloat = f / binHz;
        size_t bin0 = static_cast<size_t>(std::floor(binFloat));
        size_t bin1 = std::min(fr.bins() - 1, bin0 + 1);

        if (bin0 >= fr.bins()) {
            result[i] = fr.magnitudeDB(fr.bins() - 1);
        } else {
            double t = binFloat - static_cast<double>(bin0);
            double db0 = fr.magnitudeDB(bin0);
            double db1 = fr.magnitudeDB(bin1);
            result[i] = db0 + t * (db1 - db0);
        }
    }
    return result;
}

std::vector<BiquadParameters> PEQAutoFit::fit(
    const std::vector<double>& measuredMagDB,
    const std::vector<double>& freqs,
    const TargetCurve& target,
    int sampleRate,
    const PEQAutoFitOptions& options
) {
    std::vector<BiquadParameters> result;
    if (measuredMagDB.size() != freqs.size() || freqs.empty() || options.bandCount <= 0) {
        return result;
    }

    size_t gridCount = freqs.size();
    std::vector<double> targetMagDB(gridCount);
    for (size_t i = 0; i < gridCount; ++i) {
        targetMagDB[i] = target.evaluate(freqs[i]);
    }

    // Residual error = measured - target
    std::vector<double> residual(gridCount);
    for (size_t i = 0; i < gridCount; ++i) {
        residual[i] = measuredMagDB[i] - targetMagDB[i];
    }

    // 1. Seed candidate peaking bands at peak residual locations
    std::vector<BiquadParameters> bands;
    for (int b = 0; b < options.bandCount; ++b) {
        size_t maxErrIdx = 0;
        double maxAbsErr = -1.0;
        for (size_t i = 0; i < gridCount; ++i) {
            double absErr = std::abs(residual[i]);
            if (absErr > maxAbsErr) {
                maxAbsErr = absErr;
                maxErrIdx = i;
            }
        }

        double seedFreq = freqs[maxErrIdx];
        double seedGain = -residual[maxErrIdx];
        seedGain = std::max(-options.maxGainDB, std::min(options.maxGainDB, seedGain));

        if (options.modalMode && seedFreq < options.schroederHz) {
            if (seedGain > 0) seedGain = 0;
        }

        BiquadParameters p;
        p.type = BiquadType::Peaking;
        p.freq = seedFreq;
        p.gain = seedGain;
        p.q = (options.modalMode && seedFreq < options.schroederHz) ? options.modalMinQ : 1.41;
        bands.push_back(p);

        // Subtract band response from residual
        auto coeffs = BiquadCoefficients::compute(p, sampleRate);
        if (coeffs.has_value()) {
            for (size_t i = 0; i < gridCount; ++i) {
                residual[i] += coeffs.value().gainDB(freqs[i], sampleRate);
            }
        }
    }

    // 2. Coordinate descent optimization on (freq, gain, Q)
    const int numPasses = 5;
    for (int pass = 0; pass < numPasses; ++pass) {
        for (size_t b = 0; b < bands.size(); ++b) {
            auto& band = bands[b];
            if (!band.freq.has_value() || !band.gain.has_value() || !band.q.has_value()) continue;

            double currentF = band.freq.value();
            double currentG = band.gain.value();
            double currentQ = band.q.value();

            // Evaluate current total error
            auto evalTotalErr = [&](const std::vector<BiquadParameters>& bList) -> double {
                double errSum = 0.0;
                for (size_t i = 0; i < gridCount; ++i) {
                    double eqResponse = 0.0;
                    for (const auto& bp : bList) {
                        auto c = BiquadCoefficients::compute(bp, sampleRate);
                        if (c.has_value()) eqResponse += c.value().gainDB(freqs[i], sampleRate);
                    }
                    double diff = (measuredMagDB[i] + eqResponse) - targetMagDB[i];
                    errSum += diff * diff;
                }
                return errSum;
            };

            // Golden section search for best frequency
            double bestF = currentF;
            double minErr = evalTotalErr(bands);

            for (double fStep : {0.85, 0.95, 1.0, 1.05, 1.15}) {
                double testF = std::max(20.0, std::min(20000.0, currentF * fStep));
                bands[b].freq = testF;
                double e = evalTotalErr(bands);
                if (e < minErr) { minErr = e; bestF = testF; }
            }
            bands[b].freq = bestF;

            // Golden section search for best gain
            double bestG = currentG;
            for (double gStep : {-3.0, -1.0, 0.0, 1.0, 3.0}) {
                double testG = currentG + gStep;
                testG = std::max(-options.maxGainDB, std::min(options.maxGainDB, testG));
                if (options.modalMode && bestF < options.schroederHz && testG > 0) testG = 0;

                bands[b].gain = testG;
                double e = evalTotalErr(bands);
                if (e < minErr) { minErr = e; bestG = testG; }
            }
            bands[b].gain = bestG;

            // Golden section search for best Q
            double minQ = (options.modalMode && bestF < options.schroederHz) ? options.modalMinQ : 0.5;
            double bestQ = currentQ;
            for (double qStep : {0.7, 0.9, 1.0, 1.1, 1.4}) {
                double testQ = std::max(minQ, std::min(10.0, currentQ * qStep));
                bands[b].q = testQ;
                double e = evalTotalErr(bands);
                if (e < minErr) { minErr = e; bestQ = testQ; }
            }
            bands[b].q = bestQ;
        }
    }

    // 3. Cleanup stage: drop bands with negligible gain < 0.2 dB
    for (const auto& b : bands) {
        if (b.gain.has_value() && std::abs(b.gain.value()) >= 0.2) {
            result.push_back(b);
        }
    }

    return result;
}
