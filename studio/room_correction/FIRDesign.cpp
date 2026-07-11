#include "room_correction/FIRDesign.h"
#include "room_correction/MeasurementFFT.h"
#include <cmath>
#include <algorithm>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

std::vector<double> FIRDesign::linearPhase(
    const std::vector<BiquadParameters>& bands,
    int sampleRate,
    const FIRDesignOptions& options
) {
    int nFft = options.fftSize;
    size_t bins = nFft / 2 + 1;
    double binHz = static_cast<double>(sampleRate) / static_cast<double>(nFft);

    std::vector<double> inReal(bins), inImag(bins, 0.0);
    double preampLin = std::pow(10.0, options.preampDB / 20.0);

    for (size_t k = 0; k < bins; ++k) {
        double f = static_cast<double>(k) * binHz;
        double gainDB = 0.0;
        for (const auto& b : bands) {
            auto coeffs = BiquadCoefficients::compute(b, sampleRate);
            if (coeffs.has_value()) gainDB += coeffs.value().gainDB(f, sampleRate);
        }
        double mag = std::pow(10.0, gainDB / 20.0) * preampLin;
        inReal[k] = mag;
    }

    std::vector<double> rawIR;
    MeasurementFFT::inverse(inReal, inImag, rawIR);

    // Circular shift by N/2 samples to make non-causal linear-phase impulse response centered
    std::vector<double> centered(nFft);
    size_t half = nFft / 2;
    for (size_t i = 0; i < static_cast<size_t>(nFft); ++i) {
        centered[i] = rawIR[(i + half) % nFft];
    }

    // Apply Hann window
    for (size_t i = 0; i < static_cast<size_t>(nFft); ++i) {
        double w = 0.5 * (1.0 - std::cos(2.0 * M_PI * static_cast<double>(i) / static_cast<double>(nFft)));
        centered[i] *= w;
    }

    centered.resize(options.outputLength);
    return centered;
}

std::vector<double> FIRDesign::minimumPhase(
    const std::vector<BiquadParameters>& bands,
    int sampleRate,
    const FIRDesignOptions& options
) {
    int nFft = options.fftSize;
    size_t bins = nFft / 2 + 1;
    double binHz = static_cast<double>(sampleRate) / static_cast<double>(nFft);

    std::vector<double> magDB(bins);
    double preampLin = std::pow(10.0, options.preampDB / 20.0);

    for (size_t k = 0; k < bins; ++k) {
        double f = static_cast<double>(k) * binHz;
        double gainDB = 0.0;
        for (const auto& b : bands) {
            auto coeffs = BiquadCoefficients::compute(b, sampleRate);
            if (coeffs.has_value()) gainDB += coeffs.value().gainDB(f, sampleRate);
        }
        magDB[k] = gainDB;
    }

    // Real cepstrum minimum-phase reconstruction
    // 1. Log magnitude
    std::vector<double> logMag(bins);
    for (size_t k = 0; k < bins; ++k) {
        double mag = std::pow(10.0, magDB[k] / 20.0) * preampLin;
        logMag[k] = std::log(std::max(1e-12, mag));
    }

    // 2. IFFT log magnitude to get cepstrum
    std::vector<double> inImag(bins, 0.0);
    std::vector<double> cepstrum;
    MeasurementFFT::inverse(logMag, inImag, cepstrum);

    // 3. Fold minimum-phase cepstrum operator
    size_t n = cepstrum.size();
    std::vector<double> mpCepstrum(n, 0.0);
    mpCepstrum[0] = cepstrum[0];
    mpCepstrum[n / 2] = cepstrum[n / 2];

    for (size_t i = 1; i < n / 2; ++i) {
        mpCepstrum[i] = 2.0 * cepstrum[i];
    }

    // 4. FFT minimum-phase cepstrum
    std::vector<double> cReal, cImag;
    MeasurementFFT::forward(mpCepstrum, cReal, cImag);

    // 5. Exponentiate complex cepstrum spectrum to yield minimum phase complex response
    std::vector<double> hReal(bins), hImag(bins);
    for (size_t k = 0; k < bins; ++k) {
        double magExp = std::exp(cReal[k]);
        hReal[k] = magExp * std::cos(cImag[k]);
        hImag[k] = magExp * std::sin(cImag[k]);
    }

    // 6. IFFT complex spectrum to get final minimum-phase impulse response
    std::vector<double> minPhaseIR;
    MeasurementFFT::inverse(hReal, hImag, minPhaseIR);

    minPhaseIR.resize(options.outputLength);
    return minPhaseIR;
}

std::vector<double> FIRDesign::linearPhaseFromMagDB(
    const std::vector<double>& magDB,
    int sampleRate,
    const FIRDesignOptions& options
) {
    int nFft = options.fftSize;
    size_t bins = nFft / 2 + 1;

    std::vector<double> hRe(bins), hIm(bins);
    double preampLin = std::pow(10.0, options.preampDB / 20.0);

    for (size_t k = 0; k < bins; ++k) {
        double mag = std::pow(10.0, magDB[k] / 20.0) * preampLin;
        double phase = -M_PI * static_cast<double>(k);
        hRe[k] = mag * std::cos(phase);
        hIm[k] = mag * std::sin(phase);
    }

    std::vector<double> rawIR;
    MeasurementFFT::inverse(hRe, hIm, rawIR);

    for (size_t i = 0; i < static_cast<size_t>(nFft); ++i) {
        double w = 0.5 * (1.0 - std::cos(2.0 * M_PI * static_cast<double>(i) / static_cast<double>(nFft)));
        rawIR[i] *= w;
    }

    rawIR.resize(options.outputLength);
    return rawIR;
}

std::vector<double> FIRDesign::minimumPhaseFromMagDB(
    const std::vector<double>& magDB,
    int sampleRate,
    const FIRDesignOptions& options
) {
    int nFft = options.fftSize;
    size_t bins = nFft / 2 + 1;

    double preampLin = std::pow(10.0, options.preampDB / 20.0);

    std::vector<double> logMag(bins);
    for (size_t k = 0; k < bins; ++k) {
        double mag = std::pow(10.0, magDB[k] / 20.0) * preampLin;
        logMag[k] = std::log(std::max(1e-12, mag));
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

    minPhaseIR.resize(options.outputLength);
    return minPhaseIR;
}

std::vector<double> FIRDesign::fromMeasurement(
    const FrequencyResponse& measured,
    const TargetCurve& target,
    int sampleRate,
    const FIRDesignMeasurementOptions& options
) {
    int nFft = options.fftSize;
    size_t bins = nFft / 2 + 1;
    double binHz = static_cast<double>(sampleRate) / static_cast<double>(nFft);

    std::vector<double> targetMagDB(bins);

    for (size_t k = 0; k < bins; ++k) {
        double f = static_cast<double>(k) * binHz;
        double targetDB = target.evaluate(f);
        size_t srcBin = static_cast<size_t>(std::round(f / (static_cast<double>(measured.sampleRate) / static_cast<double>(measured.fftSize))));
        double measDB = measured.magnitudeDB(srcBin);

        double invDB = targetDB - measDB;
        invDB = std::min(options.maxBoostDB, invDB);

        if (f < options.minFreqHz || f > options.maxFreqHz) {
            invDB = 0.0;
        }

        targetMagDB[k] = invDB;
    }

    FIRDesignOptions opt;
    opt.fftSize = options.fftSize;
    opt.outputLength = options.fftSize;
    opt.preampDB = options.preampDB;

    if (options.phaseBlend <= 0.05) {
        return minimumPhaseFromMagDB(targetMagDB, sampleRate, opt);
    } else if (options.phaseBlend >= 0.95) {
        return linearPhaseFromMagDB(targetMagDB, sampleRate, opt);
    }

    auto minP = minimumPhaseFromMagDB(targetMagDB, sampleRate, opt);
    auto linP = linearPhaseFromMagDB(targetMagDB, sampleRate, opt);

    std::vector<double> blended(nFft);
    double b = options.phaseBlend;
    for (size_t i = 0; i < static_cast<size_t>(nFft); ++i) {
        blended[i] = (1.0 - b) * minP[i] + b * linP[i];
    }

    return blended;
}
