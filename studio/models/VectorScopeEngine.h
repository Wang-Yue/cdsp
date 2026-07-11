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
    bool isCapture = false;
    size_t nFrames = 1024;
    bool showParticles = false;
    bool autoScale = true;

    AudioSamplesData samples;

    void update(const AudioSamplesData& newSamples) {
        samples = newSamples;
        emit updated();
    }

    void reset() {
        samples = AudioSamplesData();
        emit updated();
    }

    void resetToDefaults() {
        isCapture = false;
        nFrames = 1024;
        showParticles = false;
        autoScale = true;
        samples = AudioSamplesData();
        emit updated();
    }

signals:
    void updated();
};

#endif // VECTOR_SCOPE_ENGINE_H
