#include "room_correction/FIRDesign.h"

#include "room_correction/MeasurementFFT.h"

#include <algorithm>
#include <cmath>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

static double wrappedNear(double phi, double reference) {
    double p = phi;
    while (p - reference > M_PI)
        p -= 2.0 * M_PI;
    while (p - reference < -M_PI)
        p += 2.0 * M_PI;
    return p;
}

static size_t peakIndex(const std::vector<double>& ir) {
    size_t idx = 0;
    double bestAbs = 0.0;
    for (size_t i = 0; i < ir.size(); ++i) {
        double v = std::abs(ir[i]);
        if (v > bestAbs) {
            bestAbs = v;
            idx = i;
        }
    }
    return idx;
}

static std::vector<double> computeMinimumPhaseAngle(const std::vector<double>& magnitude, int fftSize,
                                                    double floorLin) {
    size_t bins = magnitude.size();
    int n = fftSize;
    double logFloor = std::log(floorLin);
    std::vector<double> logMag(bins);
    for (size_t k = 0; k < bins; ++k) {
        logMag[k] = std::log(std::max(magnitude[k], floorLin));
        if (!std::isfinite(logMag[k]))
            logMag[k] = logFloor;
    }

    std::vector<double> inImag(bins, 0.0);
    std::vector<double> cepstrum;
    MeasurementFFT::inverse(logMag, inImag, cepstrum);

    std::vector<double> causal(n, 0.0);
    causal[0] = cepstrum[0];
    if (n / 2 >= 1) {
        for (size_t i = 1; i < static_cast<size_t>(n / 2); ++i) {
            causal[i] = 2.0 * cepstrum[i];
        }
        causal[n / 2] = cepstrum[n / 2];
    }

    std::vector<double> re(bins), im(bins);
    MeasurementFFT::forward(causal, re, im);
    return im;
}

std::vector<double> FIRDesign::linearPhase(const std::vector<BiquadParameters>& bands, int sampleRate,
                                           const FIRDesignOptions& options) {
    int nFft = options.fftSize;
    size_t bins = nFft / 2 + 1;
    double binHz = static_cast<double>(sampleRate) / static_cast<double>(nFft);

    std::vector<BiquadCoefficients> computedCoeffs;
    computedCoeffs.reserve(bands.size());
    for (const auto& b : bands) {
        auto c = BiquadCoefficients::compute(b, sampleRate);
        if (c.has_value())
            computedCoeffs.push_back(c.value());
    }

    std::vector<double> hRe(bins), hIm(bins);
    double preampLin = std::pow(10.0, options.preampDB / 20.0);
    double floorLin = std::pow(10.0, options.floorDB / 20.0);

    for (size_t k = 0; k < bins; ++k) {
        double f = static_cast<double>(k) * binHz;
        double gainDB = 0.0;
        for (const auto& coeffs : computedCoeffs) {
            gainDB += coeffs.gainDB(f, sampleRate);
        }
        double magLin = std::pow(10.0, gainDB / 20.0);
        double mag = std::max(floorLin, magLin) * preampLin;
        double phase = -M_PI * static_cast<double>(k);
        hRe[k] = mag * std::cos(phase);
        hIm[k] = mag * std::sin(phase);
    }

    std::vector<double> ir;
    MeasurementFFT::inverse(hRe, hIm, ir);
    return ir;
}

std::vector<double> FIRDesign::minimumPhase(const std::vector<BiquadParameters>& bands, int sampleRate,
                                            const FIRDesignOptions& options) {
    int nFft = options.fftSize;
    size_t bins = nFft / 2 + 1;
    double binHz = static_cast<double>(sampleRate) / static_cast<double>(nFft);

    std::vector<BiquadCoefficients> computedCoeffs;
    computedCoeffs.reserve(bands.size());
    for (const auto& b : bands) {
        auto c = BiquadCoefficients::compute(b, sampleRate);
        if (c.has_value())
            computedCoeffs.push_back(c.value());
    }

    std::vector<double> magDB(bins);
    for (size_t k = 0; k < bins; ++k) {
        double f = static_cast<double>(k) * binHz;
        double gainDB = 0.0;
        for (const auto& coeffs : computedCoeffs) {
            gainDB += coeffs.gainDB(f, sampleRate);
        }
        magDB[k] = gainDB;
    }

    FIRDesignOptions opt = options;
    opt.preampDB = options.preampDB;
    return minimumPhaseFromMagDB(magDB, sampleRate, opt);
}

std::vector<double> FIRDesign::linearPhaseFromMagDB(const std::vector<double>& magDB, int sampleRate,
                                                    const FIRDesignOptions& options) {
    int nFft = options.fftSize;
    size_t bins = nFft / 2 + 1;

    std::vector<double> hRe(bins), hIm(bins);
    double preampLin = std::pow(10.0, options.preampDB / 20.0);
    double floorLin = std::pow(10.0, options.floorDB / 20.0);

    for (size_t k = 0; k < bins; ++k) {
        double magLin = std::pow(10.0, magDB[k] / 20.0);
        double mag = std::max(floorLin, magLin) * preampLin;
        double phase = -M_PI * static_cast<double>(k);
        hRe[k] = mag * std::cos(phase);
        hIm[k] = mag * std::sin(phase);
    }

    std::vector<double> rawIR;
    MeasurementFFT::inverse(hRe, hIm, rawIR);
    int outLen = options.outputLength.value_or(nFft);
    if (outLen < static_cast<int>(rawIR.size())) {
        auto maxIt =
            std::max_element(rawIR.begin(), rawIR.end(), [](double a, double b) { return std::abs(a) < std::abs(b); });
        size_t peakIdx = std::distance(rawIR.begin(), maxIt);
        int halfLen = outLen / 2;
        int startIdx = std::max(0, static_cast<int>(peakIdx) - halfLen);
        std::vector<double> truncated(outLen, 0.0);
        for (int i = 0; i < outLen; ++i) {
            int src = startIdx + i;
            if (src >= 0 && static_cast<size_t>(src) < rawIR.size()) {
                truncated[i] = rawIR[src];
            }
        }
        return truncated;
    }
    return rawIR;
}

std::vector<double> FIRDesign::minimumPhaseFromMagDB(const std::vector<double>& magDB, int sampleRate,
                                                     const FIRDesignOptions& options) {
    int nFft = options.fftSize;
    size_t bins = nFft / 2 + 1;

    double floorLin = std::pow(10.0, options.floorDB / 20.0);
    double preampLin = std::pow(10.0, options.preampDB / 20.0);

    std::vector<double> logMag(bins);
    for (size_t k = 0; k < bins; ++k) {
        double magLin = std::pow(10.0, magDB[k] / 20.0);
        logMag[k] = std::log(std::max(floorLin, magLin) * preampLin);
    }

    std::vector<double> inImag(bins, 0.0);
    std::vector<double> cepstrum;
    MeasurementFFT::inverse(logMag, inImag, cepstrum);

    size_t n = cepstrum.size();
    std::vector<double> mpCepstrum(n, 0.0);
    mpCepstrum[0] = cepstrum[0];
    mpCepstrum[n / 2] = cepstrum[n / 2];

    for (size_t i = 1; i < n / 2; ++i) {
        mpCepstrum[i] = 2.0 * cepstrum[i];
    }

    std::vector<double> cReal, cImag;
    MeasurementFFT::forward(mpCepstrum, cReal, cImag);

    std::vector<double> hReal(bins), hImag(bins);
    for (size_t k = 0; k < bins; ++k) {
        double magExp = std::exp(cReal[k]);
        hReal[k] = magExp * std::cos(cImag[k]);
        hImag[k] = magExp * std::sin(cImag[k]);
    }

    std::vector<double> minPhaseIR;
    MeasurementFFT::inverse(hReal, hImag, minPhaseIR);

    int outLen = options.outputLength.value_or(nFft);
    minPhaseIR.resize(outLen);
    return minPhaseIR;
}

std::vector<double> FIRDesign::fromMeasurement(const FrequencyResponse& measured, const TargetCurve& target,
                                               int sampleRate, const FIRDesignMeasurementOptions& options) {
    if (measured.bins() == 0)
        return {};
    int n = options.fftSize;
    size_t bins = n / 2 + 1;

    double measuredBinHz = static_cast<double>(measured.sampleRate) / static_cast<double>(measured.fftSize);
    double designBinHz = static_cast<double>(sampleRate) / static_cast<double>(n);
    double floorLin = std::pow(10.0, options.floorDB / 20.0);
    double preampLin = std::pow(10.0, options.preampDB / 20.0);
    double maxBoostLin = std::pow(10.0, options.maxBoostDB / 20.0);
    double taperOctaves = 0.5;
    double lowEdgeLog = std::log10(options.minFreqHz);
    double highEdgeLog = std::log10(options.maxFreqHz);
    double blend = std::max(0.0, std::min(1.0, options.phaseBlend));

    std::vector<double> corrMag(bins, preampLin);
    std::vector<double> targetAngle(bins, 0.0);

    for (size_t k = 0; k < bins; ++k) {
        double freq = static_cast<double>(k) * designBinHz;
        double delayPhase = -M_PI * static_cast<double>(k);
        double corr = preampLin;
        double correction = 0.0;

        if (freq >= options.minFreqHz && freq <= options.maxFreqHz) {
            size_t mBin = static_cast<size_t>(std::round(freq / measuredBinHz));
            size_t mb = std::min(measured.bins() - 1, mBin);
            double mRe = measured.real[mb];
            double mIm = measured.imag[mb];
            double mMag = std::sqrt(mRe * mRe + mIm * mIm);

            if (mMag >= floorLin) {
                double targetDB = target.evaluate(freq);
                double targetMag = std::pow(10.0, targetDB / 20.0);
                double c = targetMag / mMag;
                c = std::min(c, maxBoostLin);
                corr = c * preampLin;
                correction = -std::atan2(mIm, mRe);

                double logF = std::log10(freq);
                double lowDist = (logF - lowEdgeLog) / taperOctaves;
                double highDist = (highEdgeLog - logF) / taperOctaves;
                double edge = std::min(lowDist, highDist);
                if (edge < 1.0) {
                    double w = 0.5 * (1.0 - std::cos(M_PI * std::max(0.0, edge)));
                    corr = corr * w + preampLin * (1.0 - w);
                    correction = correction * w;
                }
            }
        }
        corrMag[k] = corr;
        targetAngle[k] = delayPhase + correction;
    }

    std::vector<double> hRe(bins), hIm(bins);
    if (blend >= 1.0 - 1e-9) {
        for (size_t k = 0; k < bins; ++k) {
            hRe[k] = corrMag[k] * std::cos(targetAngle[k]);
            hIm[k] = corrMag[k] * std::sin(targetAngle[k]);
        }
    } else {
        std::vector<double> minPhaseAngle = computeMinimumPhaseAngle(corrMag, n, floorLin);
        for (size_t k = 0; k < bins; ++k) {
            double phi = blend * wrappedNear(targetAngle[k], minPhaseAngle[k]) + (1.0 - blend) * minPhaseAngle[k];
            hRe[k] = corrMag[k] * std::cos(phi);
            hIm[k] = corrMag[k] * std::sin(phi);
        }
    }

    std::vector<double> ir;
    MeasurementFFT::inverse(hRe, hIm, ir);

    size_t centre = blend >= 0.99 ? static_cast<size_t>(n / 2) : peakIndex(ir);
    size_t halfWin = std::min(centre, static_cast<size_t>(n) - centre - 1);
    if (halfWin > 0) {
        for (size_t i = 0; i < static_cast<size_t>(n); ++i) {
            size_t dist = (i > centre) ? (i - centre) : (centre - i);
            if (dist > halfWin) {
                ir[i] = 0.0;
            } else {
                double w = 0.5 * (1.0 + std::cos(M_PI * static_cast<double>(dist) / static_cast<double>(halfWin)));
                ir[i] *= w;
            }
        }
    }

    return ir;
}
