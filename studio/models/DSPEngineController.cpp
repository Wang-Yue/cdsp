#include "models/DSPEngineController.h"
#include "models/LogManager.h"

DSPEngineController::DSPEngineController(
    std::shared_ptr<CDSPEngine> engine,
    std::shared_ptr<AudioDeviceManager> devices,
    std::shared_ptr<AudioSettings> settings,
    std::shared_ptr<PipelineStore> pipeline,
    QObject* parent
) : QObject(parent), m_engine(engine), m_devices(devices), m_settings(settings), m_pipeline(pipeline) {

    connect(m_devices.get(), &AudioDeviceManager::configChanged, this, [this]() {
        if (status == ProcessingState::Running) {
            applyConfig();
        }
    });

    connect(m_pipeline.get(), &PipelineStore::pipelineChanged, this, [this]() {
        if (status == ProcessingState::Running) {
            applyConfig();
        }
    });
}

DSPConfiguration DSPEngineController::buildConfiguration() const {
    DSPConfiguration config;

    int sampleRate = m_devices->captureConfig.sampleRate;
    int channelCount = m_devices->captureConfig.channels;

    config.devices.samplerate = sampleRate;
    config.devices.chunksize = m_settings->chunkSize;
    config.devices.enableRateAdjust = m_settings->enableRateAdjust;

    if (m_settings->resamplerEnabled) {
        ResamplerConfig resCfg;
        resCfg.type = m_settings->resamplerType;
        if (m_settings->resamplerUseProfile) {
            resCfg.profile = resamplerProfileToString(m_settings->resamplerProfile);
        } else {
            resCfg.sincLen = m_settings->resamplerSincLen;
            resCfg.oversamplingFactor = m_settings->resamplerOversamplingFactor;
            resCfg.window = m_settings->resamplerWindow;
            resCfg.fCutoff = m_settings->resamplerFCutoff;
        }
        config.devices.resampler = resCfg;
    }

    config.devices.capture = m_devices->captureConfig.toCaptureDeviceConfig();
    config.devices.playback = m_devices->playbackConfig.toPlaybackDeviceConfig();

    auto buildRes = m_pipeline->buildPipeline(sampleRate, channelCount);
    config.filters = buildRes.filters;
    config.mixers = buildRes.mixers;
    config.processors = buildRes.processors;
    config.pipeline = buildRes.steps;

    return config;
}

void DSPEngineController::startEngine() {
    status = ProcessingState::Starting;
    emit statusChanged(status);

    DSPConfiguration config = buildConfiguration();
    std::string jsonStr = config.toJsonString();

    std::string err;
    bool ok = m_engine->start(jsonStr, err);
    if (ok) {
        status = ProcessingState::Running;
        syncFaders();
        LogManager::instance()->appendLog(LogLevel::Info, "DSP Engine started successfully.");
    } else {
        status = ProcessingState::Inactive;
        lastErrorMessage = err;
        LogManager::instance()->appendLog(LogLevel::Error, QString::fromStdString("Engine start failed: " + err));
    }
    emit statusChanged(status);
}

void DSPEngineController::stopEngine() {
    m_engine->stop();
    status = ProcessingState::Inactive;
    emit statusChanged(status);
    LogManager::instance()->appendLog(LogLevel::Info, "DSP Engine stopped.");
}

void DSPEngineController::applyConfig() {
    if (status != ProcessingState::Running) return;
    DSPConfiguration config = buildConfiguration();
    std::string jsonStr = config.toJsonString();

    std::string err;
    bool ok = m_engine->start(jsonStr, err);
    if (ok) {
        syncFaders();
        emit configApplied();
        LogManager::instance()->appendLog(LogLevel::Info, "Dynamic configuration applied.");
    } else {
        lastErrorMessage = err;
        LogManager::instance()->appendLog(LogLevel::Error, QString::fromStdString("Apply config failed: " + err));
    }
}

void DSPEngineController::setFaderVolume(Fader fader, float db, bool instant) {
    m_settings->setVolume(db, fader);
    m_engine->setFaderVolume(fader, db, instant);
}

void DSPEngineController::setFaderMute(Fader fader, bool mute) {
    m_settings->setMuted(mute, fader);
    m_engine->setFaderMute(fader, mute);
}

void DSPEngineController::syncFaders() {
    for (Fader f : {Fader::Main, Fader::Aux1, Fader::Aux2, Fader::Aux3, Fader::Aux4}) {
        m_engine->setFaderVolume(f, m_settings->getVolume(f), true);
        m_engine->setFaderMute(f, m_settings->getMuted(f));
    }
}

void DSPEngineController::updateStatus(const StateUpdate& update) {
    if (status != update.state) {
        status = update.state;
        emit statusChanged(status);
    }
    if (update.stopReason.type == StopReasonType::CaptureFormatChange || update.stopReason.type == StopReasonType::PlaybackFormatChange) {
        int newRate = update.stopReason.formatChangeRate;
        if (newRate > 0) {
            LogManager::instance()->appendLog(LogLevel::Warn, QString("Format change detected (%1 Hz), restarting engine...").arg(newRate));
            m_devices->captureConfig.sampleRate = newRate;
            m_devices->playbackConfig.sampleRate = newRate;
            startEngine();
        }
    }
}
