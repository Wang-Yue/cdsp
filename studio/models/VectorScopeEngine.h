#ifndef VECTOR_SCOPE_ENGINE_H
#define VECTOR_SCOPE_ENGINE_H

#include "config/DSPConfigTypes.h"

#include <QObject>
#include <vector>

class VectorScopeEngine : public QObject {
    Q_OBJECT

public:
    explicit VectorScopeEngine(QObject* parent = nullptr) : QObject(parent) {}

    int visibilityCount = 0;
    bool isCapture = true;
    size_t nFrames = 512;
    bool showParticles = true;
    bool autoScale = true;
    int channelL = 0;
    int channelR = 1;
    float traceDecayRate = 0.85f;

    AudioSamplesData samples;

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
        nFrames = 512;
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
