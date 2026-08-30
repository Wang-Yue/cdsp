#ifndef DSP_ENGINE_CONTROLLER_H
#define DSP_ENGINE_CONTROLLER_H

#include "config/DSPConfigTypes.h"       // for ProcessingState, Fader, ProcessingStopReason, DSPConfiguration
#include "engine/CDSPEngine.h"           // for CDSPEngine
#include "models/AudioDeviceManager.h"   // for AudioDeviceManager
#include "models/AudioSettings.h"        // for AudioSettings
#include "models/LevelState.h"           // for LevelState
#include "models/MonitoringController.h" // for MonitoringController
#include "models/PipelineStore.h"        // for PipelineStore

#include <QObject> // for QObject, Q_OBJECT, signals
#include <QTimer>  // for QTimer
#include <memory>  // for shared_ptr
#include <string>  // for basic_string, string

class DSPEngineController : public QObject {
    Q_OBJECT

public:
    DSPEngineController(std::shared_ptr<CDSPEngine> engine, std::shared_ptr<AudioDeviceManager> devices,
                        std::shared_ptr<AudioSettings> settings, std::shared_ptr<PipelineStore> pipeline,
                        std::shared_ptr<MonitoringController> monitoring = nullptr,
                        std::shared_ptr<LevelState> levels = nullptr, QObject* parent = nullptr);

    ProcessingState status = ProcessingState::Inactive;
    ProcessingStopReason lastStopReason;
    std::string lastErrorMessage;

    DSPConfiguration buildConfiguration() const;
    std::shared_ptr<CDSPEngine> engine() const { return m_engine; }
    std::shared_ptr<AudioDeviceManager> devices() const { return m_devices; }
    std::shared_ptr<AudioSettings> settings() const { return m_settings; }
    std::shared_ptr<PipelineStore> pipelineStore() const { return m_pipeline; }
    std::shared_ptr<LevelState> levels() const { return m_levels; }
    std::shared_ptr<MonitoringController> monitoring() const { return m_monitoring; }

    void setMonitoringController(std::shared_ptr<MonitoringController> monitoring);

    void startEngine();
    void stopEngine();
    void applyConfig();

    void setFaderVolume(Fader fader, float db, bool instant = false);
    void setFaderMute(Fader fader, bool mute);
    void toggleFaderMute(Fader fader);

signals:
    void statusChanged(ProcessingState state);
    void statusUpdated(ProcessingState state, const ProcessingStopReason& stopReason);
    void configApplied();

private:
    void runApplyConfigTask();
    void applyConfigAsync();
    void syncFaders();

    std::shared_ptr<CDSPEngine> m_engine;
    std::shared_ptr<AudioDeviceManager> m_devices;
    std::shared_ptr<AudioSettings> m_settings;
    std::shared_ptr<PipelineStore> m_pipeline;
    std::shared_ptr<MonitoringController> m_monitoring;
    std::shared_ptr<LevelState> m_levels;

    QTimer m_applyConfigTimer;
};

#endif // DSP_ENGINE_CONTROLLER_H
