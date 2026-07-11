#ifndef DSP_ENGINE_CONTROLLER_H
#define DSP_ENGINE_CONTROLLER_H

#include "engine/CDSPEngine.h"
#include "models/AudioDeviceManager.h"
#include "models/AudioSettings.h"
#include "models/PipelineStore.h"
#include <QObject>
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
    std::string lastErrorMessage;

    DSPConfiguration buildConfiguration() const;

    void startEngine();
    void stopEngine();
    void applyConfig();

    void setFaderVolume(Fader fader, float db, bool instant = false);
    void setFaderMute(Fader fader, bool mute);

    void updateStatus(const StateUpdate& update);

signals:
    void statusChanged(ProcessingState state);
    void configApplied();

private:
    std::shared_ptr<CDSPEngine> m_engine;
    std::shared_ptr<AudioDeviceManager> m_devices;
    std::shared_ptr<AudioSettings> m_settings;
    std::shared_ptr<PipelineStore> m_pipeline;

    void syncFaders();
};

#endif // DSP_ENGINE_CONTROLLER_H
