#ifndef SPECTRUM_ENGINE_H
#define SPECTRUM_ENGINE_H

#include "config/DSPConfigTypes.h"

#include <QObject>
#include <optional>
#include <vector>

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
        isCapture = true;
        channel = std::nullopt;
        nBins = 30;
        minFreq = 25.0;
        maxFreq = 20000.0;
        data = SpectrumData();
        emit updated();
    }

signals:
    void updated();
};

#endif // SPECTRUM_ENGINE_H
