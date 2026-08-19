#ifndef VECTOR_SCOPE_ENGINE_H
#define VECTOR_SCOPE_ENGINE_H

#include "config/DSPConfigTypes.h"

#include <QObject>
#include <algorithm>
#include <cmath>
#include <vector>

enum class VectorScopeWindow {
    Fast = 0,   // 25 ms
    Smooth = 1, // 50 ms
    Long = 2    // 100 ms
};

class VectorScopeEngine : public QObject {
    Q_OBJECT

public:
    explicit VectorScopeEngine(QObject* parent = nullptr) : QObject(parent) {}

    int visibilityCount = 0;
    bool isCapture = true;
    VectorScopeWindow window = VectorScopeWindow::Fast;
    bool showParticles = true;
    bool autoScale = true;
    int channelL = 0;
    int channelR = 1;
    float traceDecayRate = 0.85f;

    AudioSamplesData samples;

    static double windowSeconds(VectorScopeWindow w) {
        switch (w) {
        case VectorScopeWindow::Fast:
            return 0.025;
        case VectorScopeWindow::Smooth:
            return 0.050;
        case VectorScopeWindow::Long:
            return 0.100;
        }
        return 0.025;
    }

    size_t framesToFetch(double sampleRate) const {
        double sr = sampleRate > 0.0 ? sampleRate : 48000.0;
        double frames = std::round(sr * windowSeconds(window));
        return static_cast<size_t>(std::clamp(static_cast<int>(frames), 128, 262144));
    }

    void update(const AudioSamplesData& newSamples) {
        samples = newSamples;
        emit updated();
    }

    bool reset() {
        if (samples.channels.empty())
            return false;
        samples = AudioSamplesData();
        emit updated();
        return true;
    }

    void resetToDefaults() {
        isCapture = true;
        window = VectorScopeWindow::Fast;
        showParticles = true;
        autoScale = true;
        channelL = 0;
        channelR = 1;
        traceDecayRate = 0.85f;
        samples = AudioSamplesData();
        emit updated();
    }

signals:
    void updated();
};

#endif // VECTOR_SCOPE_ENGINE_H
