#include "room_correction/FrequencyResponse.h"

#include "room_correction/MeasurementFFT.h"

#include <algorithm>
#include <cmath>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

FrequencyResponse::FrequencyResponse(const std::vector<double>& real, const std::vector<double>& imag, int sampleRate,
                                     int fftSize)
    : real(real), imag(imag), sampleRate(sampleRate), fftSize(fftSize) {}

double FrequencyResponse::frequency(size_t bin) const {
    double binHz = static_cast<double>(sampleRate) / static_cast<double>(fftSize);
    return static_cast<double>(bin) * binHz;
}

double FrequencyResponse::magnitude(size_t bin) const {
    if (bin >= real.size())
        return 0.0;
    double r = real[bin];
    double i = imag[bin];
    return std::sqrt(r * r + i * i);
}

double FrequencyResponse::magnitudeDB(size_t bin) const {
    double mag = magnitude(bin);
    return (mag > 0.0) ? 20.0 * std::log10(mag) : -100.0;
}

double FrequencyResponse::phase(size_t bin) const {
    if (bin >= real.size())
        return 0.0;
    return std::atan2(imag[bin], real[bin]);
}

std::vector<double> FrequencyResponse::unwrappedPhase() const {
    size_t count = bins();
    std::vector<double> unwrap(count);
    if (count == 0)
        return unwrap;

    unwrap[0] = phase(0);
    double cumulativePhase = unwrap[0];

    for (size_t i = 1; i < count; ++i) {
        double pPrev = phase(i - 1);
        double pCurr = phase(i);
        double diff = pCurr - pPrev;

        while (diff > M_PI)
            diff -= 2.0 * M_PI;
        while (diff < -M_PI)
            diff += 2.0 * M_PI;

        cumulativePhase += diff;
        unwrap[i] = cumulativePhase;
    }
    return unwrap;
}

std::vector<double> FrequencyResponse::groupDelay() const {
    size_t count = bins();
    std::vector<double> gd(count, 0.0);
    if (count < 2)
        return gd;

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
        while (p < ir.samples.size())
            p <<= 1;
        nFft = static_cast<int>(p);
    }

    std::vector<double> padded = ir.samples;
    padded.resize(nFft, 0.0);

    std::vector<double> outReal, outImag;
    MeasurementFFT::forward(padded, outReal, outImag);

    return FrequencyResponse(outReal, outImag, ir.sampleRate, nFft);
}

FrequencyResponse FrequencyResponse::fdw(const ImpulseResponse& ir, double cycles, int targetFftSize) {
    int n = targetFftSize;
    if (n <= 0)
        n = static_cast<int>(ir.samples.size());
    if (n % 2 != 0)
        n += 1;
    size_t bins = n / 2 + 1;

    std::vector<double> re(bins, 0.0);
    std::vector<double> im(bins, 0.0);

    double twoPi = 2.0 * M_PI;
    size_t p = ir.peakIndex();
    size_t count = ir.samples.size();

    for (size_t k = 0; k < bins; ++k) {
        size_t kEff = std::max(static_cast<size_t>(1), k);
        double w_k = cycles * static_cast<double>(n) / static_cast<double>(kEff);
        double h_k = w_k / 2.0;

        int startIdx = std::max(0, static_cast<int>(std::floor(static_cast<double>(p) - h_k)));
        int endIdx = std::min(static_cast<int>(count) - 1, static_cast<int>(std::ceil(static_cast<double>(p) + h_k)));

        double rSum = 0.0;
        double iSum = 0.0;

        double kOverN = static_cast<double>(k) / static_cast<double>(n);
        for (int i = startIdx; i <= endIdx; ++i) {
            double d = std::abs(static_cast<double>(i) - static_cast<double>(p));
            if (d <= h_k && h_k > 0.0) {
                double w = 0.5 * (1.0 + std::cos(M_PI * d / h_k));
                double angle = twoPi * kOverN * static_cast<double>(i);
                rSum += ir.samples[i] * w * std::cos(angle);
                iSum -= ir.samples[i] * w * std::sin(angle);
            }
        }
        re[k] = rSum;
        im[k] = iSum;
    }

    return FrequencyResponse(re, im, ir.sampleRate, n);
}

std::vector<std::pair<double, FrequencyResponse>> FrequencyResponse::stft(const ImpulseResponse& ir, int sliceCount,
                                                                          double maxTimeSeconds, int windowLength,
                                                                          int targetFftSize) {
    std::vector<std::pair<double, FrequencyResponse>> slices;
    if (ir.samples.empty())
        return slices;

    size_t peak = ir.peakIndex();
    double fs = static_cast<double>(ir.sampleRate);
    double dt = maxTimeSeconds / static_cast<double>(std::max(1, sliceCount - 1));

    for (int s = 0; s < sliceCount; ++s) {
        double timeSec = static_cast<double>(s) * dt;
        size_t offsetSamples = static_cast<size_t>(timeSec * fs);
        size_t startIdx = peak + offsetSamples;

        if (startIdx >= ir.samples.size())
            break;

        std::vector<double> sliceSamples(windowLength, 0.0);
        for (int i = 0; i < windowLength; ++i) {
            size_t idx = startIdx + i;
            if (idx < ir.samples.size()) {
                double w =
                    0.5 * (1.0 - std::cos(2.0 * M_PI * static_cast<double>(i) / static_cast<double>(windowLength - 1)));
                sliceSamples[i] = ir.samples[idx] * w;
            }
        }

        ImpulseResponse sliceIR(sliceSamples, ir.sampleRate);
        FrequencyResponse fr = from(sliceIR, targetFftSize);
        slices.push_back({timeSec, fr});
    }

    return slices;
}
