#ifndef SPECTRUM_ENGINE_H
#define SPECTRUM_ENGINE_H

#include "config/DSPConfigTypes.h"

#include <QObject>
#include <optional>
#include <vector>

enum class FFTWindowFunction { Hann, Hamming, Blackman, FlatTop, Rectangular };
enum class OctaveSmoothing { None, OneThird, OneSixth, OneTwelfth, OneTwentyFourth };

class SpectrumEngine : public QObject {
    Q_OBJECT

public:
    explicit SpectrumEngine(QObject* parent = nullptr) : QObject(parent) {}

    int visibilityCount = 0;
    bool isCapture = true;
    std::optional<int> channel = std::nullopt;
    size_t nBins = 30;
    double minFreq = 25.0;
    double maxFreq = 20000.0;
    double minDB = -120.0;
    double maxDB = 0.0;
    FFTWindowFunction windowFunction = FFTWindowFunction::Hann;
    OctaveSmoothing smoothing = OctaveSmoothing::None;
    float peakHoldDecayRate = 0.95f;

    SpectrumData data;

    void update(const SpectrumData& newData) {
        data = newData;
        emit updated();
    }

    bool reset() {
        if (data.magnitudes.empty() && data.frequencies.empty())
            return false;
        data = SpectrumData();
        emit updated();
        return true;
    }

    void resetToDefaults() {
        isCapture = true;
        channel = std::nullopt;
        nBins = 30;
        minFreq = 25.0;
        maxFreq = 20000.0;
        minDB = -120.0;
        maxDB = 0.0;
        windowFunction = FFTWindowFunction::Hann;
        smoothing = OctaveSmoothing::None;
        peakHoldDecayRate = 0.95f;
        data = SpectrumData();
        emit updated();
    }

signals:
    void updated();
};

#endif // SPECTRUM_ENGINE_H
