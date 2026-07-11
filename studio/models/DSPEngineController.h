#ifndef DSP_ENGINE_CONTROLLER_H
#define DSP_ENGINE_CONTROLLER_H

#include "engine/CDSPEngine.h"
#include "models/AudioDeviceManager.h"
#include "models/AudioSettings.h"
#include "models/PipelineStore.h"
#include <QObject>
#include <QTimer>
#include <QDateTime>
#include <memory>
#include <string>

class DSPEngineController : public QObject {
    Q_OBJECT

public:
    DSPEngineController(
        std::shared_ptr<CDSPEngine> engine,
        std::shared_ptr<AudioDeviceManager> devices,
        std::shared_ptr<AudioSettings> settings,
        std::shared_ptr<PipelineStore> pipeline,
        QObject* parent = nullptr
    );

    ProcessingState status = ProcessingState::Inactive;
    ProcessingStopReason lastStopReason;
    std::string lastErrorMessage;

    DSPConfiguration buildConfiguration() const;
    std::shared_ptr<AudioDeviceManager> devices() const { return m_devices; }
    std::shared_ptr<AudioSettings> settings() const { return m_settings; }
    std::shared_ptr<PipelineStore> pipelineStore() const { return m_pipeline; }

    void startEngine();
    void stopEngine();
    void applyConfig();

    void setFaderVolume(Fader fader, float db, bool instant = false);
    void setFaderMute(Fader fader, bool mute);

    void updateStatus(const StateUpdate& update);

signals:
    void statusChanged(ProcessingState state);
    void statusUpdated(ProcessingState state, const ProcessingStopReason& stopReason);
    void configApplied();

private:
    std::shared_ptr<CDSPEngine> m_engine;
    std::shared_ptr<AudioDeviceManager> m_devices;
    std::shared_ptr<AudioSettings> m_settings;
    std::shared_ptr<PipelineStore> m_pipeline;

    QTimer m_reconnectTimer;
    int m_retryCount = 0;
    const int m_maxRetries = 5;
    bool m_userStopped = true;
    QDateTime m_lastStartTime;

    void scheduleAutoRestart(int delayMs);
    void syncFaders();
};

#endif // DSP_ENGINE_CONTROLLER_H
