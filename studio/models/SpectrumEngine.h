#ifndef SPECTRUM_ENGINE_H
#define SPECTRUM_ENGINE_H

#include "config/DSPConfigTypes.h"
#include <QObject>
#include <vector>
#include <optional>

class SpectrumEngine : public QObject {
    Q_OBJECT

public:
    explicit SpectrumEngine(QObject* parent = nullptr) : QObject(parent) {}

    int visibilityCount = 0;
    bool isCapture = false;
    std::optional<int> channel = std::nullopt;
    size_t nBins = 60;
    double minFreq = 20.0;
    double maxFreq = 20000.0;

    SpectrumData data;

    void update(const SpectrumData& newData) {
        data = newData;
        emit updated();
    }

    void reset() {
        data = SpectrumData();
        emit updated();
    }

    void resetToDefaults() {
        isCapture = false;
        channel = std::nullopt;
        nBins = 60;
        minFreq = 20.0;
        maxFreq = 20000.0;
        data = SpectrumData();
        emit updated();
    }

signals:
    void updated();
};

#endif // SPECTRUM_ENGINE_H
