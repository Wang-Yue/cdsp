#ifndef SPECTROGRAM_ENGINE_H
#define SPECTROGRAM_ENGINE_H

#include "config/DSPConfigTypes.h"

#include <QObject>
#include <deque>
#include <optional>
#include <vector>

enum class ColorPalette { Classic, Default = Classic, Viridis, Magma, Plasma, Inferno, Jet };

class SpectrogramEngine : public QObject {
    Q_OBJECT

public:
    explicit SpectrogramEngine(QObject* parent = nullptr) : QObject(parent) {}

    int visibilityCount = 0;
    bool isCapture = true;
    std::optional<int> channel = std::nullopt;
    size_t nBins = 200;
    double minFreq = 20.0;
    double maxFreq = 20000.0;
    bool show3D = false;
    ColorPalette colorPalette = ColorPalette::Classic;

    std::deque<SpectrumData> history;
    size_t maxHistory = 300;
    double timeWindow = 10.0;

    void pushSpectrum(const SpectrumData& newData) {
        history.push_back(newData);
        if (history.size() > maxHistory) {
            history.pop_front();
        }
        emit updated();
    }

    void reset() {
        history.clear();
        emit updated();
    }

    void resetToDefaults() {
        isCapture = true;
        channel = std::nullopt;
        nBins = 200;
        minFreq = 20.0;
        maxFreq = 20000.0;
        show3D = false;
        colorPalette = ColorPalette::Classic;
        history.clear();
        emit updated();
    }

signals:
    void updated();
};

#endif // SPECTROGRAM_ENGINE_H
