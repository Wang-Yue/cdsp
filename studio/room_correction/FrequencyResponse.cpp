#include "room_correction/FrequencyResponse.h"
#include "room_correction/MeasurementFFT.h"
#include <cmath>
#include <algorithm>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

FrequencyResponse::FrequencyResponse(const std::vector<double>& real, const std::vector<double>& imag, int sampleRate, int fftSize)
    : real(real), imag(imag), sampleRate(sampleRate), fftSize(fftSize) {}

double FrequencyResponse::frequency(size_t bin) const {
    double binHz = static_cast<double>(sampleRate) / static_cast<double>(fftSize);
    return static_cast<double>(bin) * binHz;
}

double FrequencyResponse::magnitude(size_t bin) const {
    if (bin >= real.size()) return 0.0;
    double r = real[bin];
    double i = imag[bin];
    return std::sqrt(r * r + i * i);
}

double FrequencyResponse::magnitudeDB(size_t bin) const {
    double mag = magnitude(bin);
    return (mag > 0.0) ? 20.0 * std::log10(mag) : -100.0;
}

double FrequencyResponse::phase(size_t bin) const {
    if (bin >= real.size()) return 0.0;
    return std::atan2(imag[bin], real[bin]);
}

std::vector<double> FrequencyResponse::unwrappedPhase() const {
    size_t count = bins();
    std::vector<double> unwrap(count);
    if (count == 0) return unwrap;

    unwrap[0] = phase(0);
    double cumulativePhase = unwrap[0];

    for (size_t i = 1; i < count; ++i) {
        double pPrev = phase(i - 1);
        double pCurr = phase(i);
        double diff = pCurr - pPrev;

        while (diff > M_PI) diff -= 2.0 * M_PI;
        while (diff < -M_PI) diff += 2.0 * M_PI;

        cumulativePhase += diff;
        unwrap[i] = cumulativePhase;
    }
    return unwrap;
}

std::vector<double> FrequencyResponse::groupDelay() const {
    size_t count = bins();
    std::vector<double> gd(count, 0.0);
    if (count < 2) return gd;

    auto unwrap = unwrappedPhase();
    double df = static_cast<double>(sampleRate) / static_cast<double>(fftSize);
    double dw = 2.0 * M_PI * df;

    for (size_t i = 1; i < count - 1; ++i) {
        double dPhase = unwrap[i + 1] - unwrap[i - 1];
        gd[i] = -dPhase / (2.0 * dw);
    }
    gd[0] = gd[1];
    gd[count - 1] = gd[count - 2];

    return gd;
}

FrequencyResponse FrequencyResponse::from(const ImpulseResponse& ir, int targetFftSize) {
    int nFft = targetFftSize;
    if (ir.samples.size() > static_cast<size_t>(nFft)) {
        size_t p = 1;
        while (p < ir.samples.size()) p <<= 1;
        nFft = static_cast<int>(p);
    }

    std::vector<double> padded = ir.samples;
    padded.resize(nFft, 0.0);

    std::vector<double> outReal, outImag;
    MeasurementFFT::forward(padded, outReal, outImag);

    return FrequencyResponse(outReal, outImag, ir.sampleRate, nFft);
}

FrequencyResponse FrequencyResponse::fdw(const ImpulseResponse& ir, double cycles, int targetFftSize) {
    size_t peak = ir.peakIndex();
    int fs = ir.sampleRate;
    int nFft = targetFftSize;

    std::vector<double> fdwIR(ir.samples.size(), 0.0);

    for (size_t i = 0; i < ir.samples.size(); ++i) {
        double distSamples = std::abs(static_cast<double>(i) - static_cast<double>(peak));
        double distSec = distSamples / static_cast<double>(fs);

        // FDW width scales inversely with frequency f
        // Window length at f Hz is cycles / f
        fdwIR[i] = ir.samples[i];
    }

    ImpulseResponse windowedIR(fdwIR, fs);
    return from(windowedIR, nFft);
}

std::vector<std::pair<double, FrequencyResponse>> FrequencyResponse::stft(
    const ImpulseResponse& ir,
    int sliceCount,
    double maxTimeSeconds,
    int windowLength,
    int targetFftSize
) {
    std::vector<std::pair<double, FrequencyResponse>> slices;
    if (ir.samples.empty()) return slices;

    size_t peak = ir.peakIndex();
    double fs = static_cast<double>(ir.sampleRate);
    double dt = maxTimeSeconds / static_cast<double>(std::max(1, sliceCount - 1));

    for (int s = 0; s < sliceCount; ++s) {
        double timeSec = static_cast<double>(s) * dt;
        size_t offsetSamples = static_cast<size_t>(timeSec * fs);
        size_t startIdx = peak + offsetSamples;

        if (startIdx >= ir.samples.size()) break;

        std::vector<double> sliceSamples(windowLength, 0.0);
        for (int i = 0; i < windowLength; ++i) {
            size_t idx = startIdx + i;
            if (idx < ir.samples.size()) {
                double w = 0.5 * (1.0 - std::cos(2.0 * M_PI * static_cast<double>(i) / static_cast<double>(windowLength)));
                sliceSamples[i] = ir.samples[idx] * w;
            }
        }

        ImpulseResponse sliceIR(sliceSamples, ir.sampleRate);
        FrequencyResponse fr = from(sliceIR, targetFftSize);
        slices.push_back({ timeSec, fr });
    }

    return slices;
}
