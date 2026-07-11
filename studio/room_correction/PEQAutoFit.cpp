#include "room_correction/PEQAutoFit.h"

#include <algorithm>
#include <cmath>
#include <functional>
#include <limits>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

std::vector<double> PEQAutoFit::logFrequencyGrid(double fMin, double fMax, size_t count) {
    std::vector<double> grid(count);
    if (count == 0)
        return grid;
    if (count == 1) {
        grid[0] = fMin;
        return grid;
    }

    double logMin = std::log10(fMin);
    double logMax = std::log10(fMax);
    double step = (logMax - logMin) / static_cast<double>(count - 1);

    for (size_t i = 0; i < count; ++i) {
        grid[i] = std::pow(10.0, logMin + static_cast<double>(i) * step);
    }
    return grid;
}

std::vector<double> PEQAutoFit::smoothLogOctave(const std::vector<double>& magDB, const std::vector<double>& freqs,
                                                double octaves) {
    return smoothLogOctave(magDB, freqs, octaves, octaves, 1.0, 2.0);
}

std::vector<double> PEQAutoFit::smoothLogOctave(const std::vector<double>& values,
                                                const std::vector<double>& frequencies, double midOctaves,
                                                double trebleOctaves, double transitionLowHz, double transitionHighHz) {
    if (values.size() != frequencies.size() || values.empty())
        return values;
    size_t n = values.size();
    std::vector<double> logF(n);
    for (size_t i = 0; i < n; ++i) {
        logF[i] = std::log10(std::max(frequencies[i], 1.0));
    }
    double log10_2 = std::log10(2.0);
    double lowLog = std::log10(transitionLowHz);
    double highLog = std::log10(transitionHighHz);

    std::vector<double> out(n);
    for (size_t i = 0; i < n; ++i) {
        double t = 0.0;
        if (logF[i] <= lowLog) {
            t = 0.0;
        } else if (logF[i] >= highLog) {
            t = 1.0;
        } else {
            double u = (logF[i] - lowLog) / (highLog - lowLog);
            t = u * u * (3.0 - 2.0 * u);
        }
        double octWidth = midOctaves + t * (trebleOctaves - midOctaves);
        double sigma = octWidth * log10_2 / 2.0;
        double radius = 3.0 * sigma;
        double sum = 0.0;
        double wsum = 0.0;

        for (size_t j = 0; j < n; ++j) {
            double d = logF[j] - logF[i];
            if (std::abs(d) > radius) {
                if (d > radius)
                    break;
                continue;
            }
            double w = std::exp(-0.5 * d * d / (sigma * sigma));
            sum += w * values[j];
            wsum += w;
        }
        out[i] = wsum > 0.0 ? sum / wsum : values[i];
    }
    return out;
}

std::vector<double> PEQAutoFit::sampleMagnitudeDB(const FrequencyResponse& fr,
                                                  const std::vector<double>& atFrequencies) {
    std::vector<double> result(atFrequencies.size(), -100.0);
    if (fr.bins() == 0)
        return result;

    double binHz = static_cast<double>(fr.sampleRate) / static_cast<double>(fr.fftSize);

    for (size_t i = 0; i < atFrequencies.size(); ++i) {
        double f = atFrequencies[i];
        int bin = static_cast<int>(std::round(f / binHz));
        size_t clampedBin = static_cast<size_t>(std::clamp(bin, 0, static_cast<int>(fr.bins() - 1)));
        result[i] = fr.magnitudeDB(clampedBin);
    }
    return result;
}

static void accumulateBandResponse(const BiquadParameters& band, const std::vector<double>& frequencies, int sampleRate,
                                   std::vector<double>& residual) {
    auto coeffs = BiquadCoefficients::compute(band, sampleRate);
    if (!coeffs.has_value())
        return;
    for (size_t i = 0; i < frequencies.size(); ++i) {
        residual[i] += coeffs.value().gainDB(frequencies[i], sampleRate);
    }
}

static std::optional<BiquadParameters> seedPeak(const std::vector<double>& residual,
                                                const std::vector<double>& frequencies,
                                                const PEQAutoFitOptions& options) {
    int bestIdx = -1;
    double bestAbs = 0.0;

    for (size_t i = 0; i < residual.size(); ++i) {
        double f = frequencies[i];
        if (f < options.minFreqHz || f > options.maxFreqHz)
            continue;
        double v = residual[i];
        if (options.modalMode && f <= options.schroederHz && v <= 0)
            continue;
        double a = std::abs(v);
        if (a > bestAbs) {
            bestAbs = a;
            bestIdx = static_cast<int>(i);
        }
    }

    if (bestIdx < 0 || bestAbs < options.convergenceDB)
        return std::nullopt;

    double peak = residual[bestIdx];
    double halfTarget = std::abs(peak) * 0.5;
    double sign = peak >= 0 ? 1.0 : -1.0;
    int l = bestIdx;
    while (l > 0 && residual[l - 1] * sign >= halfTarget)
        l--;
    int r = bestIdx;
    while (r < static_cast<int>(residual.size()) - 1 && residual[r + 1] * sign >= halfTarget)
        r++;

    double f0 = frequencies[bestIdx];
    double bw = std::max(frequencies[r] - frequencies[l], f0 * 0.05);
    bool modalActive = options.modalMode && f0 <= options.schroederHz;
    double qFloor = modalActive ? std::max(options.minQ, options.modalMinQ) : options.minQ;
    double q = std::max(qFloor, std::min(options.maxQ, f0 / bw));
    double gain = std::max(-options.maxGainDB, std::min(options.maxGainDB, -peak));

    BiquadParameters p;
    p.type = BiquadType::Peaking;
    p.freq = f0;
    p.gain = gain;
    p.q = q;
    return p;
}

static std::optional<BiquadParameters> seedShelf(BiquadType type, std::pair<double, double> edgeBand, double cornerHz,
                                                 const std::vector<double>& residual,
                                                 const std::vector<double>& frequencies,
                                                 const PEQAutoFitOptions& options) {
    std::vector<double> samples;
    for (size_t i = 0; i < frequencies.size(); ++i) {
        if (frequencies[i] >= edgeBand.first && frequencies[i] <= edgeBand.second) {
            samples.push_back(residual[i]);
        }
    }
    if (samples.size() < 4)
        return std::nullopt;
    std::sort(samples.begin(), samples.end());
    double median = samples[samples.size() / 2];
    if (std::abs(median) < options.convergenceDB)
        return std::nullopt;
    double gain = std::max(-options.maxGainDB, std::min(options.maxGainDB, -median));

    BiquadParameters p;
    p.type = type;
    p.freq = cornerHz;
    p.gain = gain;
    p.q = 0.71;
    return p;
}

static double cost(const BiquadParameters& band, const std::vector<double>& rwb, const std::vector<double>& frequencies,
                   int sampleRate) {
    auto coeffs = BiquadCoefficients::compute(band, sampleRate);
    if (!coeffs.has_value())
        return std::numeric_limits<double>::infinity();
    double total = 0.0;
    for (size_t i = 0; i < frequencies.size(); ++i) {
        double r = rwb[i] + coeffs.value().gainDB(frequencies[i], sampleRate);
        total += r * r;
    }
    return total;
}

static double goldenSectionSearch(double lo, double hi, double tolerance, bool logSpace,
                                  std::function<double(double)> f) {
    double phi = (std::sqrt(5.0) - 1.0) / 2.0;
    double a = logSpace ? std::log10(lo) : lo;
    double b = logSpace ? std::log10(hi) : hi;
    if (a >= b)
        return logSpace ? std::pow(10.0, a) : a;

    double x1 = b - phi * (b - a);
    double x2 = a + phi * (b - a);
    double v1 = f(logSpace ? std::pow(10.0, x1) : x1);
    double v2 = f(logSpace ? std::pow(10.0, x2) : x2);

    int iterations = 0;
    while (std::abs(b - a) > tolerance && iterations < 40) {
        iterations++;
        if (v1 < v2) {
            b = x2;
            x2 = x1;
            v2 = v1;
            x1 = b - phi * (b - a);
            v1 = f(logSpace ? std::pow(10.0, x1) : x1);
        } else {
            a = x1;
            x1 = x2;
            v1 = v2;
            x2 = a + phi * (b - a);
            v2 = f(logSpace ? std::pow(10.0, x2) : x2);
        }
    }
    double mid = (a + b) / 2.0;
    return logSpace ? std::pow(10.0, mid) : mid;
}

static BiquadParameters optimizeBand(const BiquadParameters& band, const std::vector<double>& rwb,
                                     const std::vector<double>& frequencies, int sampleRate,
                                     const PEQAutoFitOptions& options) {
    BiquadParameters curr = band;
    for (int iter = 0; iter < 2; ++iter) {
        // Optimize Gain
        curr.gain = goldenSectionSearch(-options.maxGainDB, options.maxGainDB, 0.02, false, [&](double g) {
            BiquadParameters b = curr;
            b.gain = g;
            return cost(b, rwb, frequencies, sampleRate);
        });

        if (curr.type != BiquadType::Lowshelf && curr.type != BiquadType::Highshelf) {
            // Optimize Q
            curr.q = goldenSectionSearch(options.minQ, options.maxQ, 0.005, true, [&](double q) {
                BiquadParameters b = curr;
                b.q = q;
                return cost(b, rwb, frequencies, sampleRate);
            });
            // Optimize Freq
            double f0 = curr.freq.value_or(1000.0);
            double lo = std::max(options.minFreqHz, f0 / 2.0);
            double hi = std::min(options.maxFreqHz, f0 * 2.0);
            if (hi > lo) {
                curr.freq = goldenSectionSearch(lo, hi, 0.001, true, [&](double freq) {
                    BiquadParameters b = curr;
                    b.freq = freq;
                    return cost(b, rwb, frequencies, sampleRate);
                });
            }
        } else {
            // Shelf Q & Freq
            curr.q = goldenSectionSearch(0.4, 0.7, 0.005, true, [&](double q) {
                BiquadParameters b = curr;
                b.q = q;
                return cost(b, rwb, frequencies, sampleRate);
            });
            double f0 = curr.freq.value_or(1000.0);
            double lo = std::max(options.minFreqHz, f0 / 2.0);
            double hi = std::min(options.maxFreqHz, f0 * 2.0);
            if (hi > lo) {
                curr.freq = goldenSectionSearch(lo, hi, 0.001, true, [&](double freq) {
                    BiquadParameters b = curr;
                    b.freq = freq;
                    return cost(b, rwb, frequencies, sampleRate);
                });
            }
        }
    }
    return curr;
}

static double parameterDelta(const BiquadParameters& a, const BiquadParameters& b) {
    double af = a.freq.value_or(1.0);
    double bf = b.freq.value_or(1.0);
    double df = std::abs(af - bf) / std::max(1.0, af);
    double dg = std::abs(a.gain.value_or(0.0) - b.gain.value_or(0.0)) / 10.0;
    double aq = a.q.value_or(1.0);
    double bq = b.q.value_or(1.0);
    double dq = std::abs(aq - bq) / std::max(0.1, aq);
    return std::max({df, dg, dq});
}

std::vector<BiquadParameters> PEQAutoFit::fit(const std::vector<double>& measuredMagDB,
                                              const std::vector<double>& freqs, const TargetCurve& target,
                                              int sampleRate, const PEQAutoFitOptions& options) {
    size_t n = freqs.size();
    if (measuredMagDB.size() != n || n <= 4)
        return {};

    std::vector<double> rawResidual(n);
    for (size_t i = 0; i < n; ++i) {
        rawResidual[i] = measuredMagDB[i] - target.evaluate(freqs[i]);
    }

    std::vector<double> baseResidual =
        smoothLogOctave(rawResidual, freqs, options.smoothingOctaves, options.trebleSmoothingOctaves,
                        options.smoothingTransitionLow, options.smoothingTransitionHigh);

    std::vector<BiquadParameters> bands;
    std::vector<double> residual = baseResidual;

    bool suppressLowShelf = options.modalMode && options.lowShelfFreqHz <= options.schroederHz;
    if (options.addEndpointShelves && !suppressLowShelf) {
        auto lo = seedShelf(BiquadType::Lowshelf, {options.minFreqHz, options.lowShelfFreqHz}, options.lowShelfFreqHz,
                            residual, freqs, options);
        if (lo.has_value()) {
            bands.push_back(lo.value());
            accumulateBandResponse(lo.value(), freqs, sampleRate, residual);
        }
        auto hi = seedShelf(BiquadType::Highshelf, {options.highShelfFreqHz, options.maxFreqHz},
                            options.highShelfFreqHz, residual, freqs, options);
        if (hi.has_value()) {
            bands.push_back(hi.value());
            accumulateBandResponse(hi.value(), freqs, sampleRate, residual);
        }
    }

    int peakBudget = std::max(0, options.bandCount - static_cast<int>(bands.size()));
    for (int i = 0; i < peakBudget; ++i) {
        auto peak = seedPeak(residual, freqs, options);
        if (!peak.has_value())
            break;
        bands.push_back(peak.value());
        accumulateBandResponse(peak.value(), freqs, sampleRate, residual);
    }

    for (int iter = 0; iter < options.refinementIterations; ++iter) {
        double maxChange = 0.0;
        for (size_t i = 0; i < bands.size(); ++i) {
            std::vector<double> rwb = baseResidual;
            for (size_t j = 0; j < bands.size(); ++j) {
                if (j != i)
                    accumulateBandResponse(bands[j], freqs, sampleRate, rwb);
            }
            BiquadParameters orig = bands[i];
            BiquadParameters opt = optimizeBand(orig, rwb, freqs, sampleRate, options);
            bands[i] = opt;
            maxChange = std::max(maxChange, parameterDelta(orig, opt));
        }
        if (maxChange < 0.001)
            break;
    }

    bands.erase(
        std::remove_if(bands.begin(), bands.end(),
                       [&](const BiquadParameters& b) { return std::abs(b.gain.value_or(0.0)) < options.dropGainDB; }),
        bands.end());

    return bands;
}
