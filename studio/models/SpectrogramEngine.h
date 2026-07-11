#ifndef SPECTROGRAM_ENGINE_H
#define SPECTROGRAM_ENGINE_H

#include "config/DSPConfigTypes.h"
#include <QObject>
#include <vector>
#include <deque>
#include <optional>

class SpectrogramEngine : public QObject {
    Q_OBJECT

public:
    explicit SpectrogramEngine(QObject* parent = nullptr) : QObject(parent) {}

    int visibilityCount = 0;
    bool isCapture = false;
    std::optional<int> channel = std::nullopt;
    size_t nBins = 100;
    double minFreq = 20.0;
    double maxFreq = 20000.0;
    bool show3D = false;

    std::deque<SpectrumData> history;
    size_t maxHistory = 50;

    void pushSpectrum(const SpectrumData& newData) {
        history.push_front(newData);
        if (history.size() > maxHistory) {
            history.pop_back();
        }
        emit updated();
    }

    void resetToDefaults() {
        isCapture = false;
        channel = std::nullopt;
        nBins = 100;
        minFreq = 20.0;
        maxFreq = 20000.0;
        show3D = false;
        history.clear();
        emit updated();
    }

signals:
    void updated();
};

#endif // SPECTROGRAM_ENGINE_H
