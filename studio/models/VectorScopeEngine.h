#ifndef VECTOR_SCOPE_ENGINE_H
#define VECTOR_SCOPE_ENGINE_H

#include "config/DSPConfigTypes.h"

#include <QObject>
#include <QSettings>
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
    explicit VectorScopeEngine(QObject* parent = nullptr) : QObject(parent) { loadSettings(); }

    int visibilityCount = 0;
    bool isCapture = true;
    VectorScopeWindow window = VectorScopeWindow::Fast;
    bool showParticles = true;
    bool autoScale = true;
    int channelL = 0;
    int channelR = 1;
    float traceDecayRate = 0.85f;

    AudioSamplesData samples;

    void loadSettings() {
        QSettings s("DSPMonitor", "MonitorQt");
        isCapture = s.value("vectorscope_is_capture", true).toBool();
        window = static_cast<VectorScopeWindow>(
            s.value("vectorscope_window", static_cast<int>(VectorScopeWindow::Fast)).toInt());
        showParticles = s.value("vectorscope_show_particles", true).toBool();
        autoScale = s.value("vectorscope_auto_scale", true).toBool();
        channelL = s.value("vectorscope_channel_l", 0).toInt();
        channelR = s.value("vectorscope_channel_r", 1).toInt();
        traceDecayRate = s.value("vectorscope_trace_decay_rate", 0.85f).toFloat();
    }

    void saveSettings() const {
        QSettings s("DSPMonitor", "MonitorQt");
        s.setValue("vectorscope_is_capture", isCapture);
        s.setValue("vectorscope_window", static_cast<int>(window));
        s.setValue("vectorscope_show_particles", showParticles);
        s.setValue("vectorscope_auto_scale", autoScale);
        s.setValue("vectorscope_channel_l", channelL);
        s.setValue("vectorscope_channel_r", channelR);
        s.setValue("vectorscope_trace_decay_rate", traceDecayRate);
    }

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
        saveSettings();
        emit updated();
    }

signals:
    void updated();
};

#endif // VECTOR_SCOPE_ENGINE_H
